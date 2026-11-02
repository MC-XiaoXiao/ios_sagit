"""Explicit development port for the audited 22H374 AMFI PMGRAON latch.

Qualcomm has no Apple PMGRAON security register. Skip only that hardware
transaction; continue at the native developer-mode resolution code. Do not
advertise a VM or manufacture an ApplePMGRNub with successful dummy methods.
"""
from project import ROOT
import hashlib
import importlib.util
from pathlib import Path
import struct


def patch_latch_function(code):
    # Audited function at provider __text+0x5b54, size 0x258. ADRP/ADD and BL
    # operands relocate, whereas this prologue, condition and branch do not.
    if len(code) != 0x258 or code[:20] != bytes.fromhex(
            'ff4301d1f65702a9f44f03a9fd7b04a9fd030191'):
        raise ValueError('unsupported AMFI latch function prologue')
    if code[28:36] != bytes.fromhex('1f050071a1000054'):
        raise ValueError('unsupported AMFI latch condition')
    if struct.unpack_from('<I', code, 0x30)[0] != 0x14000023:
        raise ValueError('unsupported AMFI latch continuation')
    # The native AVP path jumps to +0xbc after the latch; enter that same
    # continuation without changing the VM flag or printing an AVP claim.
    result = bytearray(code)
    struct.pack_into('<I', result, 0x20, 0x14000027)
    return bytes(result)


def prepare_no_apple_pmgr(source, destination):
    from project.analysis import verify_kernelcache as verifier
    data = source.read_bytes(); outer = verifier.Image(data)
    _, kexts = verifier.inventory.inspect(source)
    selected = [k for k in kexts if k['CFBundleIdentifier'] == 'com.apple.driver.AppleMobileFileIntegrity']
    if len(selected) != 1:
        raise ValueError('expected one AMFI module')
    image = verifier.Image(data, outer.offset(selected[0]['_PrelinkExecutableLoadAddr'], 32), outer=outer)
    texts = [s for s in image.sections if s['name'] == '__text']
    if len(texts) != 1 or texts[0]['size'] != 136428:
        raise ValueError('unsupported AMFI executable layout')
    address = texts[0]['vm'] + 0x5b54
    offset = image.offset(address, 0x258)
    code = data[offset:offset+0x258]
    # Confirm both ADRP+ADD references identify this exact hardware operation.
    def referenced_string(relative):
        adrp, add = struct.unpack_from('<II', code, relative)
        if adrp & 0x9f00001f != 0x90000000 or add & 0xffc003ff != 0x91000000:
            raise ValueError('unsupported AMFI string reference')
        imm = ((adrp >> 29) & 3) | (((adrp >> 5) & 0x7ffff) << 2)
        if imm & (1 << 20): imm -= 1 << 21
        target = ((address+relative) & ~0xfff) + (imm << 12) + ((add >> 10) & 0xfff)
        start = outer.offset(target)
        return data[start:data.index(0, start)]
    if referenced_string(0x24) != b'AMFI: skipping PMGRAON latch due to AVP\n' or referenced_string(0x34) != b'ApplePMGRNub':
        raise ValueError('AMFI latch identity mismatch')
    replacement = patch_latch_function(code)
    result = data[:offset] + replacement + data[offset+len(code):]
    destination.write_bytes(result)
    return {'policy': 'development-no-apple-pmgraon-latch',
            'persistent_security_latch_available': False,
            'source': str(source), 'source_sha256': hashlib.sha256(data).hexdigest(),
            'output_sha256': hashlib.sha256(result).hexdigest(),
            'instruction_address': hex(address+0x20), 'file_offset': offset+0x20,
            'before': code[0x20:0x24].hex(), 'after': replacement[0x20:0x24].hex()}


"""Select XNU's existing unthrottled I/O mode for development boots."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import subprocess


def prepare_unthrottled_io(source, destination):
    from project.analysis import verify_kernelcache as verifier
    data = source.read_bytes()
    image = verifier.Image(data)
    symbols = subprocess.check_output(
        ['llvm-nm', '--defined-only', '--format=posix', str(source)], text=True)
    matches = [line.split() for line in symbols.splitlines()
               if line.split()[0] == '_lowpri_throttle_enabled']
    if len(matches) != 1 or matches[0][1] not in ('D', 'd'):
        raise ValueError('expected one writable XNU lowpri_throttle_enabled symbol')
    address = int(matches[0][2], 16)
    section = next((s for s in image.sections
                    if s['vm'] <= address and address + 4 <= s['vm'] + s['size']), None)
    if not section or section['name'] != '__data' or not image.owner(address, 4)['prot'] & 2:
        raise ValueError('I/O throttle setting is not in writable initialized data')
    offset = image.offset(address, 4)
    if struct.unpack_from('<I', data, offset)[0] != 1:
        raise ValueError('expected enabled native I/O throttle setting')
    result = data[:offset] + bytes(4) + data[offset + 4:]
    destination.write_bytes(result)
    return {
        'policy': 'development-unthrottled-io',
        'symbol': '_lowpri_throttle_enabled', 'address': hex(address),
        'file_offset': offset, 'before': 1, 'after': 0,
        'runtime_sysctl': 'debug.lowpri_throttle_enabled',
        'source': str(source), 'source_sha256': hashlib.sha256(data).hexdigest(),
        'output_sha256': hashlib.sha256(result).hexdigest(),
    }


"""Adapt the audited watchdogd service allowlist to the Qualcomm backend.

The native daemon, IOWatchdogUserClient rule and all privilege/entitlement
checks remain intact. Only the exact hardware service class literal changes.
Both class names occupy 21 bytes, so the compiled regex, pool offsets and
profile decision nodes keep their original layout.
"""
import hashlib
import importlib.util
from pathlib import Path

APPLE_CLASS = b'AppleARMWatchdogTimer'
PLATFORM_CLASS = b'QualcommWatchdogTimer'
SERVICE_RECORD = b'\x1a\0\x54' + APPLE_CLASS + b'\x0f\0\x0f\x0a' + bytes(4)
PROFILE_CONTEXT = (
    b'\x17\0/usr/libexec/watchdogd\0' + bytes(7)
    + b'\x19\0\x53IOWatchdogUserClient\x0f\0\x0f\x0a' + bytes(5)
)


def patch_watchdog_profile(pool):
    needle = PROFILE_CONTEXT + SERVICE_RECORD
    if len(APPLE_CLASS) != len(PLATFORM_CLASS) or pool.count(needle) != 1:
        raise ValueError('unsupported watchdogd service policy record')
    offset = pool.index(needle) + len(PROFILE_CONTEXT) + 3
    result = pool[:offset] + PLATFORM_CLASS + pool[offset + len(APPLE_CLASS):]
    return result, offset


def prepare_watchdog_service_policy(source, destination):
    from project.analysis import verify_kernelcache as verifier
    data = source.read_bytes(); outer = verifier.Image(data)
    _, kexts = verifier.inventory.inspect(source)
    sandbox = [k for k in kexts if k['CFBundleIdentifier'] == 'com.apple.security.sandbox']
    backend = [k for k in kexts if k['CFBundleIdentifier'] == 'org.darwin.driver.QcomWatchdog']
    if len(sandbox) != 1 or len(backend) != 1:
        raise ValueError('expected native Sandbox and Qualcomm watchdog modules')
    classes = {p.get('IOClass') for p in backend[0].get('IOKitPersonalities', {}).values()}
    if PLATFORM_CLASS.decode() not in classes:
        raise ValueError('watchdog policy class does not match the installed backend')
    image = verifier.Image(data, outer.offset(sandbox[0]['_PrelinkExecutableLoadAddr'], 32), outer=outer)
    pools = [s for s in image.sections if s['segment'] == '__TEXT' and s['name'] == '__const']
    if len(pools) != 1 or pools[0]['size'] != 1818849:
        raise ValueError('unsupported Sandbox profile layout')
    pool = pools[0]; start = image.offset(pool['vm'], pool['size'])
    replacement, relative = patch_watchdog_profile(data[start:start + pool['size']])
    result = data[:start] + replacement + data[start + pool['size']:]
    destination.write_bytes(result)
    return {'policy': 'development-watchdog-service-class',
            'profile': 'watchdogd', 'from_class': APPLE_CLASS.decode(),
            'to_class': PLATFORM_CLASS.decode(), 'user_client': 'IOWatchdogUserClient',
            'source': str(source), 'source_sha256': hashlib.sha256(data).hexdigest(),
            'output_sha256': hashlib.sha256(result).hexdigest(),
            'literal_address': hex(pool['vm'] + relative), 'file_offset': start + relative,
            'before': APPLE_CLASS.hex(), 'after': PLATFORM_CLASS.hex()}
