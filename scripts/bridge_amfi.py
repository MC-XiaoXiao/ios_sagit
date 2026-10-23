"""Explicit development port for the audited 22H374 AMFI PMGRAON latch.

Qualcomm has no Apple PMGRAON security register. Skip only that hardware
transaction; continue at the native developer-mode resolution code. Do not
advertise a VM or manufacture an ApplePMGRNub with successful dummy methods.
"""
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
    spec = importlib.util.spec_from_file_location('amfi_verifier',
        Path(__file__).with_name('verify-kernelcache.py'))
    verifier = importlib.util.module_from_spec(spec); spec.loader.exec_module(verifier)
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
