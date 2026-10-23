"""Volatile CHRP v2 NVRAM snapshot consumed by XNU's IONVRAMCHRPHandler.

No EFI variable store or device partition is read or written. The snapshot travels
inline in Apple DT, whose RAM remains reserved below topOfKernelData.
"""
import struct
import zlib


def checksum(header):
    total = header[0] + sum(header[2:16])
    while total > 255:
        total = (total & 255) + (total >> 8)
    return total


def header(signature, size, name):
    data = bytearray(struct.pack('<BBH12s', signature, 0, size // 16, name))
    data[1] = checksum(data)
    return data


def empty_bank(size):
    # V2's common partition ends at 0x2000; remaining space is system NVRAM.
    if not 0x4000 <= size <= 0x10000 or size % 16:
        raise ValueError('CHRP bank must be 16-byte aligned, between 16 and 64 KiB')
    data = bytearray(size)
    data[:16] = header(0x5a, 32, b'2nvram')
    data[32:48] = header(0x70, 0x2000 - 32, b'2common')
    data[0x2000:0x2010] = header(0x70, size - 0x2000, b'2system')
    struct.pack_into('<I', data, 16, zlib.adler32(data[20:]))
    return bytes(data)


def validate_bank(data, size):
    if len(data) != size or not 0x4000 <= size <= 0x10000 or size % 16:
        raise ValueError('invalid CHRP bank size')
    if data[4:16] != b'2nvram'.ljust(12, b'\0') or struct.unpack_from('<H', data, 2)[0] != 2:
        raise ValueError('expected CHRP v2 Apple header')
    if struct.unpack_from('<I', data, 16)[0] != zlib.adler32(data[20:]):
        raise ValueError('invalid CHRP Adler32')
    pos, partitions = 0, {}
    while pos < size:
        hdr = data[pos:pos+16]
        if len(hdr) != 16 or hdr[1] != checksum(hdr):
            raise ValueError('invalid CHRP partition checksum')
        length = struct.unpack_from('<H', hdr, 2)[0] * 16
        if length < 16 or pos + length > size:
            raise ValueError('invalid CHRP partition bounds')
        name = bytes(hdr[4:16]).rstrip(b'\0')
        if name in partitions:
            raise ValueError('duplicate CHRP partition')
        partitions[name] = (pos + 16, length - 16)
        pos += length
    if not all(name in partitions and partitions[name][1] > 0 for name in (b'2common', b'2system')):
        raise ValueError('missing CHRP common/system partition')
    return partitions


def chosen_properties(props):
    """Complete explicitly configured FDT properties; never reinterpret proxy bytes."""
    keys = ('nvram-bank-size', 'nvram-bank-count', 'nvram-current-bank')
    if not any(k in props for k in (*keys, 'nvram-proxy-data')):
        return props
    if any(k not in props or len(props[k]) != 4 for k in keys):
        raise ValueError('NVRAM handoff needs three uint32 bank properties')
    size, count, current = (struct.unpack('>I', props[k])[0] for k in keys)
    if count != 1 or current != 0:
        raise ValueError('volatile NVRAM handoff supports one bank, current bank zero')
    data = props.get('nvram-proxy-data')
    if data is None:
        data = empty_bank(size)
    validate_bank(data, size)
    return dict(props, **{'nvram-proxy-data': data})


def with_boot_args(data, command_line):
    """Seed boot-args in an explicitly requested, empty volatile common bank."""
    if not isinstance(command_line, bytes) or b'\0' in command_line or len(command_line) >= 1024:
        raise ValueError('invalid volatile NVRAM boot-args')
    command_line.decode('ascii')
    parts = validate_bank(data, len(data))
    offset, length = parts[b'2common']
    if any(data[offset:offset+length]):
        raise ValueError('refusing to replace existing NVRAM variables')
    variable = b'boot-args=' + command_line + b'\0'
    if len(variable) >= length:
        raise ValueError('volatile NVRAM boot-args exceed common bank')
    result = bytearray(data)
    result[offset:offset+len(variable)] = variable
    struct.pack_into('<I', result, 16, zlib.adler32(result[20:]))
    validate_bank(result, len(result))
    return bytes(result)
