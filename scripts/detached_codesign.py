"""Ad-hoc SHA-256 signatures for explicitly trusted engineering data files.

Uses the same CodeDirectory/SuperBlob format as the reference project's
tools/macos-kernel-collection/codesign.py. No CMS or platform identity is claimed.
"""
import hashlib
import struct

HEADER = struct.Struct('>9I4B4I4Q')
PAGE = 4096


def sign_data(payload, identifier):
    name = identifier.encode('utf8') + b'\0'
    count = (len(payload) + PAGE - 1) // PAGE
    hashes = b''.join(hashlib.sha256(payload[p:p + PAGE]).digest()
                      for p in range(0, len(payload), PAGE))
    start = HEADER.size + len(name)
    directory = HEADER.pack(0xfade0c02, start + len(hashes), 0x20400, 2,
        start, HEADER.size, 0, count, len(payload), 32, 2, 0, 12,
        0, 0, 0, 0, len(payload), 0, 0, 0) + name + hashes
    return struct.pack('>5I', 0xfade0cc0, 20 + len(directory), 1, 0, 20) + directory


def verify_data(payload, signature):
    """Validate the complete engineering signature and every payload page."""
    if len(signature) < 20 + HEADER.size:
        raise ValueError('truncated detached signature')
    magic, length, count, slot, offset = struct.unpack_from('>5I', signature)
    if (magic, length, count, slot, offset) != (0xfade0cc0, len(signature), 1, 0, 20):
        raise ValueError('unsupported detached signature superblob')
    directory = signature[offset:]
    fields = HEADER.unpack_from(directory)
    magic, length, version, flags, hashes, ident, special, slots, limit = fields[:9]
    if (magic, length, version, flags, ident, special) != (0xfade0c02, len(directory), 0x20400, 2, HEADER.size, 0):
        raise ValueError('unsupported detached CodeDirectory')
    if fields[9:13] != (32, 2, 0, 12) or fields[13:] != (0, 0, 0, 0, len(payload), 0, 0, 0):
        raise ValueError('unsupported detached signing policy')
    if limit != len(payload) or slots != (len(payload) + PAGE - 1) // PAGE or hashes + slots * 32 != length:
        raise ValueError('detached code limit or hash count mismatch')
    if hashes <= ident or directory[hashes - 1] != 0 or b'\0' in directory[ident:hashes - 1]:
        raise ValueError('invalid detached identifier')
    expected = b''.join(hashlib.sha256(payload[p:p + PAGE]).digest()
                        for p in range(0, len(payload), PAGE))
    if directory[hashes:] != expected:
        raise ValueError('detached payload page hash mismatch')
    return [(hashlib.sha256(directory).digest()[:20], 2)]
