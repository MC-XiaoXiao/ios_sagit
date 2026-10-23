"""Ad-hoc signing for fixed-size engineering Mach-O executables and pinned bundles.

Preserves existing requirement and XML/DER entitlement blobs, authenticates
their special slots and every code page, and claims no CMS/platform identity.
The original LC_CODE_SIGNATURE allocation and total file size are retained.
"""
import hashlib
import plistlib
import struct
from detached_codesign import HEADER, PAGE

PREFIX = b'org.mi6.development.'
ENGINEERING_PREFIXES = (PREFIX, b'org.darwin.development.')
BLOB_MAGICS = {2: 0xfade0c01, 5: 0xfade7171, 7: 0xfade7172}
# Native 22H374 QuartzCore independently checks these per-process rights for
# displayable/secure contexts. Limit the additional names to this diagnostic
# executable; a loadable library cannot grant its host process entitlements.
NATIVE_TOOL_ENTITLEMENTS = {
    'org.darwin.development.inputctl': frozenset({
        'com.apple.private.hid.client.event-monitor',
        'com.apple.private.hid.client.admin',
        'com.apple.private.hid.client.debug-tool',
        'com.apple.private.hid.client.event-dispatch',
        'com.apple.hid.system.user-access-service',
    }),
    'org.darwin.development.displayctl': frozenset({
        'com.apple.QuartzCore.displayable-context',
        'com.apple.QuartzCore.secure-mode',
        'com.apple.QuartzCore.display-state',
        'com.apple.QuartzCore.global-capture',
        'com.apple.QuartzCore.secure-capture',
    }),
    # This diagnostic uses task_read_for_pid only to verify the live Setup
    # singleton and patched instructions. It never executes or writes remotely.
    'org.darwin.development.uipatchctl': frozenset({
        'task_for_pid-allow',
        'com.apple.system-task-ports',
    }),
}
# The native iOS Sandbox consumes a class-name array. Bind its exact contents
# to this diagnostic identity; other tools still accept Boolean values only.
NATIVE_TOOL_ARRAY_ENTITLEMENTS = {
    'org.darwin.development.powerctl': {
        'com.apple.security.exception.iokit-user-client-class': ('PowerUserClient',),
    },
}
# Explicitly authorized development derivatives of the pinned native service.
# Preserve its existing policy bytes; this cannot add a service entitlement.
NATIVE_SERVICE_POLICIES = {
    'org.darwin.development.datamigrator': {
        2: '987920904eab650e75788c054aa0b0524e6a80bfc71aa32df8d237a61743f986',
        5: '6c4e926027ffe856a3a6d95134d5600ee8943f41846b6724a2410645faf9bb9b',
        7: '3b14641e48d0dfd1429f84bf045df44c8f9a38ef7cde34448eefc3e874f2a7a7',
    },
    'org.darwin.development.systemappmigrator': {
        2: '987920904eab650e75788c054aa0b0524e6a80bfc71aa32df8d237a61743f986',
    },
    'org.darwin.development.springboardmigrator': {
        2: '987920904eab650e75788c054aa0b0524e6a80bfc71aa32df8d237a61743f986',
    },
}


def boolean_entitlement_blobs(entitlements, identifier=None):
    native = NATIVE_TOOL_ENTITLEMENTS.get(identifier, frozenset())
    arrays = NATIVE_TOOL_ARRAY_ENTITLEMENTS.get(identifier, {})
    def authorized(key, value):
        if not isinstance(key, str) or '\0' in key:
            return False
        if key in arrays:
            return type(value) is list and all(type(item) is str for item in value) and tuple(value) == arrays[key]
        return (key.startswith('org.darwin.') or key in native) and type(value) is bool
    if not isinstance(entitlements, dict) or not entitlements or any(
            not authorized(k, v)
            for k,v in entitlements.items()):
        raise ValueError('new tool entitlement policy requires values authorized for its identifier')
    def der(tag, payload):
        size=len(payload)
        length=bytes([size]) if size < 128 else bytes([0x80+(size.bit_length()+7)//8])+size.to_bytes((size.bit_length()+7)//8,'big')
        return bytes([tag])+length+payload
    xml=plistlib.dumps(entitlements,sort_keys=True)
    def value_der(value):
        if type(value) is bool:
            return der(1, b'\xff' if value else b'\0')
        return der(0x30, b''.join(der(0x0c, item.encode()) for item in value))
    entries=b''.join(der(0x30,der(0x0c,k.encode())+value_der(v))
                     for k,v in sorted(entitlements.items()))
    encoded=der(0x70,der(2,b'\1')+der(0xb0,entries))
    return {5:struct.pack('>II',BLOB_MAGICS[5],8+len(xml))+xml,
            7:struct.pack('>II',BLOB_MAGICS[7],8+len(encoded))+encoded}


def signature_location(image, allow_bundle=False):
    if (len(image) < 32 or struct.unpack_from('<3I', image) != (0xfeedfacf, 0x100000c, 0) or
            struct.unpack_from('<I', image, 12)[0] not in ((2, 8) if allow_bundle else (2,))):
        raise ValueError('expected thin arm64/ALL executable')
    count, size = struct.unpack_from('<II', image, 16)
    end = 32 + size; pos = 32; found = []; executable = []
    if end > len(image): raise ValueError('truncated Mach-O commands')
    for _ in range(count):
        if pos + 8 > end: raise ValueError('truncated Mach-O command')
        cmd, length = struct.unpack_from('<II', image, pos)
        if length < 8 or length % 8 or pos + length > end:
            raise ValueError('invalid Mach-O command')
        if cmd == 0x1d:
            if length != 16: raise ValueError('invalid code signature command')
            found.append(struct.unpack_from('<II', image, pos+8))
        if cmd == 0x19:
            if length < 72: raise ValueError('truncated Mach-O segment')
            off, extent = struct.unpack_from('<QQ', image, pos+40)
            protection, = struct.unpack_from('<i', image, pos+60)
            if protection & 4 and extent: executable.append((off, off+extent))
        pos += length
    if pos != end or len(found) != 1: raise ValueError('expected one code signature')
    off, capacity = found[0]
    if off < end or capacity < 12 or off + capacity != len(image):
        raise ValueError('unsupported code signature allocation')
    if not executable or any(a < 0 or b > off for a, b in executable):
        raise ValueError('invalid executable range')
    return off, capacity, min(a for a, _ in executable), max(b for _, b in executable)


def signature_blobs(signature):
    if len(signature) < 12: raise ValueError('truncated signature')
    magic, size, count = struct.unpack_from('>III', signature)
    table = 12 + 8*count
    if magic != 0xfade0cc0 or not table <= size <= len(signature):
        raise ValueError('invalid signature superblob')
    result = {}; spans = []
    for i in range(count):
        kind, off = struct.unpack_from('>II', signature, 12+8*i)
        if kind in result or not table <= off <= size-8:
            raise ValueError('invalid signature index')
        _, length = struct.unpack_from('>II', signature, off)
        if length < 8 or off+length > size: raise ValueError('invalid signature blob')
        result[kind] = signature[off:off+length]; spans.append((off, off+length))
    spans.sort()
    if any(a[1] > b[0] for a, b in zip(spans, spans[1:])):
        raise ValueError('overlapping signature blobs')
    return result, size


def resign_macho(image, identifier):
    name = identifier.encode() + b'\0'
    if not name.startswith(ENGINEERING_PREFIXES) or b'\0' in name[:-1]:
        raise ValueError('expected explicit engineering signing identifier')
    off, capacity, exec_base, exec_end = signature_location(image, identifier in NATIVE_SERVICE_POLICIES)
    blobs, _ = signature_blobs(image[off:])
    retained = {k: v for k, v in blobs.items() if k in BLOB_MAGICS}
    for kind, blob in retained.items():
        if struct.unpack_from('>I', blob)[0] != BLOB_MAGICS[kind]:
            raise ValueError('invalid preserved signing blob')
    special = max(retained, default=0)
    specials = b''.join(hashlib.sha256(retained[k]).digest() if k in retained else bytes(32)
                        for k in range(special, 0, -1))
    hashes = b''.join(hashlib.sha256(image[p:min(p+PAGE, off)]).digest() for p in range(0, off, PAGE))
    hash_offset = HEADER.size + len(name) + len(specials)
    exec_flags = 1 if struct.unpack_from('<I', image, 12)[0] == 2 else 0
    directory = HEADER.pack(0xfade0c02, hash_offset+len(hashes), 0x20400, 2,
        hash_offset, HEADER.size, special, (off+PAGE-1)//PAGE, off,
        32, 2, 0, 12, 0, 0, 0, 0, off, exec_base, exec_end-exec_base, exec_flags)
    blobs = {0: directory + name + specials + hashes, **retained}
    cursor = 12 + 8*len(blobs); indexes = []; bodies = []
    for kind, blob in sorted(blobs.items()):
        indexes.append(struct.pack('>II', kind, cursor)); bodies.append(blob); cursor += len(blob)
    signature = struct.pack('>III', 0xfade0cc0, cursor, len(blobs)) + b''.join(indexes+bodies)
    if len(signature) > capacity: raise ValueError('engineering signature does not fit original allocation')
    result = image[:off] + signature + bytes(capacity-len(signature))
    return result, verify_macho(result)


def verify_macho(image):
    off, capacity, exec_base, exec_end = signature_location(image, allow_bundle=True)
    blobs, total = signature_blobs(image[off:])
    if any(image[off+total:]) or not set(blobs) <= {0, *BLOB_MAGICS} or 0 not in blobs:
        raise ValueError('unsupported engineering signature blobs or padding')
    directory = blobs[0]
    if len(directory) < HEADER.size: raise ValueError('truncated engineering CodeDirectory')
    fields = HEADER.unpack_from(directory)
    magic, length, version, flags, hashes, ident, special, slots, limit = fields[:9]
    if (magic, length, version, flags, ident) != (0xfade0c02, len(directory), 0x20400, 2, HEADER.size):
        raise ValueError('unsupported engineering CodeDirectory')
    exec_flags = 1 if struct.unpack_from('<I', image, 12)[0] == 2 else 0
    if fields[9:13] != (32, 2, 0, 12) or fields[13:] != (0, 0, 0, 0, off, exec_base, exec_end-exec_base, exec_flags):
        raise ValueError('unsupported engineering signing policy')
    if special != max(set(blobs)-{0}, default=0) or limit != off or slots != (off+PAGE-1)//PAGE:
        raise ValueError('engineering signature coverage mismatch')
    names_end = hashes-32*special
    if names_end <= ident or not directory[ident:names_end].startswith(ENGINEERING_PREFIXES):
        raise ValueError('invalid engineering identifier')
    name = directory[ident:names_end]
    if name[-1:] != b'\0' or b'\0' in name[:-1] or hashes+32*slots != length:
        raise ValueError('invalid engineering hash layout')
    for k in range(1, special+1):
        blob = blobs.get(k)
        if blob and struct.unpack_from('>I', blob)[0] != BLOB_MAGICS[k]:
            raise ValueError('invalid engineering special blob')
        expected = hashlib.sha256(blob).digest() if blob else bytes(32)
        if directory[hashes-32*k:hashes-32*(k-1)] != expected:
            raise ValueError('engineering special-slot hash mismatch')
    expected = b''.join(hashlib.sha256(image[p:min(p+PAGE, off)]).digest() for p in range(0, off, PAGE))
    if directory[hashes:] != expected: raise ValueError('engineering code page hash mismatch')
    entitlements = plistlib.loads(blobs[5][8:]) if 5 in blobs else {}
    native_policy = NATIVE_SERVICE_POLICIES.get(name[:-1].decode())
    if struct.unpack_from('<I', image, 12)[0] == 8 and native_policy is None:
        raise ValueError('native bundles require an explicitly pinned signing policy')
    if native_policy is not None:
        actual = {k: hashlib.sha256(v).hexdigest() for k,v in blobs.items() if k != 0}
        if actual != native_policy:
            raise ValueError('native service signing policy differs from pinned original blobs')
    elif name.startswith(b'org.darwin.development.'):
        expected=boolean_entitlement_blobs(entitlements, name[:-1].decode())
        if set(blobs) != {0,5,7} or any(blobs[k] != v for k,v in expected.items()):
            raise ValueError('new tool XML/DER entitlement policy mismatch')
    return {'identifier': name[:-1].decode(), 'cdhash': hashlib.sha256(directory).digest()[:20].hex(),
            'code_limit': off, 'signature_bytes': total, 'signature_capacity': capacity,
            'entitlements': entitlements, 'preserved_blob_slots': sorted(set(blobs)-{0}),
            'cms_present': False, 'platform_identity_claimed': False}


def sign_new_macho(image, identifier, entitlements, *, compact=False):
    """Sign a newly cross-linked tool with an explicit, identity-bound policy.

    Unlike resign_macho this reserves a larger signature at EOF and updates
    __LINKEDIT before hashing. It refuses inputs with existing entitlements or
    requirements so it cannot silently rewrite the policy of a native binary.
    DER uses the version-1 application tag/dictionary layout observed in the
    pinned iOS 18 signatures. XML and DER encode exactly the same dictionary.
    """
    if not identifier.startswith('org.darwin.development.'):
        raise ValueError('new tools require a generic engineering identifier')
    retained=boolean_entitlement_blobs(entitlements, identifier)
    off, capacity, _, _ = signature_location(image)
    blobs, _ = signature_blobs(image[off:])
    if set(blobs) != {0}:
        raise ValueError('new tool signer refuses existing signing policy')
    cursor=12+8*(1+len(retained));indexes=[];bodies=[]
    for kind,blob in {0:blobs[0],**retained}.items():
        indexes.append(struct.pack('>II',kind,cursor));bodies.append(blob);cursor+=len(blob)
    initial=struct.pack('>III',0xfade0cc0,cursor,1+len(retained))+b''.join(indexes+bodies)
    # Small standalone diagnostics can omit the spare signing page. The same
    # signer still verifies the complete signature fits the reserved space.
    allocation=(max(capacity,len(initial))+(4095 if compact else 8191))//4096*4096
    code=bytearray(image[:off]);pos=32;linkedit=0
    for _ in range(struct.unpack_from('<I',code,16)[0]):
        cmd,size=struct.unpack_from('<II',code,pos)
        if cmd == 0x1d:struct.pack_into('<II',code,pos+8,off,allocation)
        if cmd == 0x19 and code[pos+8:pos+24].rstrip(b'\0') == b'__LINKEDIT':
            linkedit+=1
            start,=struct.unpack_from('<Q',code,pos+40)
            if start > off:raise ValueError('invalid new tool linkedit')
            extent=off+allocation-start
            struct.pack_into('<Q',code,pos+32,(extent+16383)//16384*16384)
            struct.pack_into('<Q',code,pos+48,extent)
        pos+=size
    if linkedit != 1:raise ValueError('new tool requires one linkedit segment')
    signed,report=resign_macho(bytes(code)+initial+bytes(allocation-len(initial)),identifier)
    if report['entitlements'] != entitlements:raise ValueError('new tool entitlement mismatch')
    return signed,report
