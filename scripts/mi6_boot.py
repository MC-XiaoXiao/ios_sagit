"""Mi 6 Android v0 boot container and the MI6BOOT1 EFI payload format."""
from __future__ import annotations

import hashlib
import lzma
import struct
import zlib
from dataclasses import dataclass

MAGIC = b'MI6BOOT1'
# Firmware revision 2 accepts a zero-delay payload. The container stays MI6BOOT1.
FIRMWARE_NAME = b'sagit-mi6boot2'
LEGACY_FIRMWARE_NAME = b'sagit-mi6boot1'
FIRMWARE_NAMES = (FIRMWARE_NAME, LEGACY_FIRMWARE_NAME)
PAYLOAD_HEADER = struct.Struct('<8sIIIIQQII16s')
AUTOBOOT = 1
LZMA = 2
MAX_IMAGE = 64 * 1024 * 1024
MAX_EFI = 256 * 1024 * 1024


def align(value: int, page: int) -> int:
    return (value + page - 1) // page * page


def check_efi(data: bytes) -> None:
    if len(data) < 64 or data[:2] != b'MZ':
        raise ValueError('payload must be an ARM64 PE/COFF EFI application')
    pe = struct.unpack_from('<I', data, 60)[0]
    if pe > len(data) - 96 or data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('invalid PE header')
    if struct.unpack_from('<H', data, pe + 4)[0] != 0xaa64:
        raise ValueError('EFI payload is not ARM64')
    if struct.unpack_from('<H', data, pe + 24)[0] != 0x20b or \
            struct.unpack_from('<H', data, pe + 92)[0] != 10:
        raise ValueError('payload must be a PE32+ EFI application')
    if len(data) > MAX_EFI:
        raise ValueError('EFI payload exceeds 256 MiB')


@dataclass(frozen=True)
class BootImage:
    header: bytes
    kernel: bytes
    ramdisk: bytes
    page: int


def parse_image(data: bytes) -> BootImage:
    if len(data) < 1632 or data[:8] != b'ANDROID!':
        raise ValueError('expected Android boot image header v0')
    kernel, _, ramdisk, _, second, _, _, page, version, _os_version = struct.unpack_from('<10I', data, 8)
    if version != 0 or page not in (2048, 4096) or not kernel or second:
        raise ValueError('only Android v0 with an appended DTB and no second stage is supported')
    start = page + align(kernel, page)
    end = start + align(ramdisk, page)
    if end != len(data):
        raise ValueError('truncated boot image or unexpected trailing data/signature')
    return BootImage(data[:page], data[page:page + kernel], data[start:start + ramdisk], page)


def assemble(image: BootImage, ramdisk: bytes, *, stamp: bool = False) -> bytes:
    header = bytearray(image.header)
    struct.pack_into('<I', header, 16, len(ramdisk))
    if stamp:
        header[48:64] = FIRMWARE_NAME.ljust(16, b'\0')
    digest = hashlib.sha1()
    for part in (image.kernel, ramdisk, b''):
        digest.update(part)
        digest.update(struct.pack('<I', len(part)))
    header[576:608] = digest.digest().ljust(32, b'\0')
    return bytes(header) + image.kernel.ljust(align(len(image.kernel), image.page), b'\0') + \
        ramdisk.ljust(align(len(ramdisk), image.page), b'\0')


def pack_payload(efi: bytes, *, autoboot: bool = True, delay: int = 0) -> bytes:
    check_efi(efi)
    if not 0 <= delay <= 30:
        raise ValueError('boot delay must be 0..30 seconds')
    # LZMA-alone, with a concrete decoded size for EDK2's LZMA SDK decoder.
    encoded = bytearray(lzma.compress(efi, format=lzma.FORMAT_ALONE,
                                     filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': 8 << 20,
                                               'lc': 3, 'lp': 0, 'pb': 2, 'mode': lzma.MODE_NORMAL,
                                               'nice_len': 64, 'mf': lzma.MF_BT4}]))
    encoded[5:13] = struct.pack('<Q', len(efi))
    header = PAYLOAD_HEADER.pack(MAGIC, 1, PAYLOAD_HEADER.size, LZMA | int(autoboot), delay,
                                 len(encoded), len(efi), zlib.crc32(encoded), zlib.crc32(efi), bytes(16))
    return header + encoded


def unpack_payload(data: bytes) -> tuple[dict, bytes]:
    if len(data) < PAYLOAD_HEADER.size:
        raise ValueError('truncated MI6BOOT1 header')
    magic, version, size, flags, delay, stored, expanded, crc, raw_crc, reserved = PAYLOAD_HEADER.unpack_from(data)
    if magic != MAGIC or version != 1 or size != PAYLOAD_HEADER.size or flags & ~3 or reserved != bytes(16):
        raise ValueError('unsupported MI6BOOT1 header')
    if not 0 <= delay <= 30 or not 64 <= expanded <= MAX_EFI or not 13 <= stored <= MAX_IMAGE or \
            stored != len(data) - size:
        raise ValueError('invalid MI6BOOT1 size or countdown')
    encoded = data[size:]
    if zlib.crc32(encoded) != crc:
        raise ValueError('EFI payload CRC32 mismatch')
    if flags & LZMA:
        if struct.unpack_from('<Q', encoded, 5)[0] != expanded or encoded[:5] != b'\x5d\0\0\x80\0':
            raise ValueError('unsupported LZMA header')
        decoder = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE, memlimit=32 << 20)
        try:
            efi = decoder.decompress(encoded, max_length=expanded + 1)
        except lzma.LZMAError as error:
            raise ValueError('invalid LZMA payload') from error
        if not decoder.eof or decoder.unused_data:
            raise ValueError('incomplete LZMA stream or trailing bytes')
    else:
        efi = encoded
    if len(efi) != expanded or zlib.crc32(efi) != raw_crc:
        raise ValueError('expanded EFI size/CRC32 mismatch')
    check_efi(efi)
    return {'autoboot': bool(flags & AUTOBOOT), 'delay_seconds': delay,
            'stored_bytes': stored, 'efi_bytes': expanded,
            'efi_sha256': hashlib.sha256(efi).hexdigest()}, efi


def package(edk2: bytes, efi: bytes | None, *, autoboot: bool = True, delay: int = 0) -> bytes:
    image = parse_image(edk2)
    firmware = image.header[48:64].rstrip(b'\0')
    if firmware not in FIRMWARE_NAMES:
        raise ValueError('EDK2 lacks the MI6BOOT1 loader; rebuild with scripts/build-sagit.sh')
    if autoboot and delay == 0 and firmware != FIRMWARE_NAME:
        raise ValueError('EDK2 lacks the zero-delay MI6BOOT1 loader; rebuild with scripts/build-sagit.sh')
    if autoboot and efi is None:
        raise ValueError('--autoboot requires --bridge')
    result = assemble(image, pack_payload(efi, autoboot=autoboot, delay=delay) if efi else b'')
    if len(result) > MAX_IMAGE:
        raise ValueError('combined image exceeds Mi 6 boot/Fastboot 64 MiB limit')
    check = parse_image(result)
    if check.kernel != image.kernel:
        raise ValueError('EDK2 kernel/DTB changed while packaging')
    if efi is not None and unpack_payload(check.ramdisk)[1] != efi:
        raise ValueError('EFI payload did not round-trip')
    return result
