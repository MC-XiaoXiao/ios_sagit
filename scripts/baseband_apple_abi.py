"""Record measured iPad scalar calls; no framework patching or device access."""
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'build/baseband/python'))
import capstone
from apfs_sealed import SealedImage
from baseband_dyld import Cache, Files, digest


def analyze(root=ROOT):
    reference_path = root / 'docs/baseband/ipad-runtime-libraries.json'
    reference = json.loads(reference_path.read_text())
    image = SealedImage(root / reference['source'])
    try:
        cache = Cache(Files(image, root / 'build/baseband/reference/libzbitmap.so'))
        library = next(l for l in reference['libraries'] if l['path'].endswith('/libTelephonyBasebandDynamic.dylib'))
        actual = cache.library(library['path'])
        if actual['uuid'] != library['uuid'] or actual['commands_sha256'] != library['commands_sha256']:
            raise ValueError('runtime reference changed')
        iokit = cache.library('/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit')
        names = {int(e['address'], 16): e['name'] for e in iokit['exports'] if 'address' in e}
        decoder = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        decoder.detail = True

        def disassemble(address, count):
            return list(decoder.disasm(cache.read(address, count), address))

        def resolve(address):
            ins = disassemble(address, 12)
            if len(ins) == 3 and ins[0].mnemonic == 'adrp' and ins[1].mnemonic == 'add' and ins[2].mnemonic == 'br':
                a, b = ins[0].op_str.split(', #'), ins[1].op_str.split(', ')
                if a[0] == 'x16' and b[:2] == ['x16', 'x16'] and ins[2].op_str == 'x16':
                    address = int(a[1], 0) + int(b[2][1:], 0)
            return address, names.get(address)

        selected = ('ResetModem', 'ForceResetModem', 'PowercycleModem', 'PowerOnModem',
                    'GetBasebandState', 'SetBasebandState', 'GetManagerState', 'SetManagerState',
                    'GetReset', 'GetResetReq', 'GetPMUExtOn', 'GetDiagReady', 'GetFSEraseDone')
        observations, unresolved = [], []
        helpers = {}
        for export in library['exports']:
            if export['name'] not in tuple('_TelephonyBaseband' + n for n in selected):
                continue
            entry = int(export['address'], 16)
            prefix = disassemble(entry, 8)
            if len(prefix) != 2 or prefix[1].mnemonic != 'b':
                unresolved.append({'name': export['name'], 'reason': 'not a two-instruction tail wrapper'})
                continue
            implementation = prefix[1].operands[0].imm
            instructions, calls = [], []
            for ins in disassemble(implementation, 2048):
                instructions.append(ins)
                if ins.mnemonic == 'bl':
                    calls.append(ins.operands[0].imm)
                if ins.mnemonic == 'ret':
                    break
            # The helper is identified by a call to the actual IOKit export,
            # not by its name, a string or an SDK declaration.
            scalar_helper = None
            for target in calls:
                if target in helpers:
                    if helpers[target]: scalar_helper = target
                    continue
                if target < cache.images[library['path']] or target >= cache.images[library['path']] + 0x21000:
                    continue
                found = False
                for child in disassemble(target, 128):
                    if child.mnemonic == 'bl':
                        _, name = resolve(child.operands[0].imm)
                        if name == '_IOConnectCallScalarMethod': found = True
                    if child.mnemonic == 'ret': break
                helpers[target] = found
                if found: scalar_helper = target
            if scalar_helper is None:
                unresolved.append({'name': export['name'], 'reason': 'no verified scalar helper in implementation'})
                continue
            if calls.count(scalar_helper) != 1:
                unresolved.append({'name': export['name'], 'reason': 'multiple scalar calls; API requires control-flow analysis'})
                continue
            constants, slots, measured = {}, {}, None
            for ins in instructions:
                op = ins.op_str
                mov = re.fullmatch(r'[wx](\d+), (#[^,]+|[wx]\d+|[wx]zr)', op)
                add = re.fullmatch(r'x(\d+), sp, #(0x[0-9a-f]+|\d+)', op)
                store = re.fullmatch(r'([wx](?:\d+|zr)), \[sp(?:, #(0x[0-9a-f]+|\d+))?\]', op)
                if ins.mnemonic == 'mov' and mov:
                    dest, value = int(mov[1]), mov[2]
                    constants[dest] = (int(value[1:], 0) if value.startswith('#') else 0 if value.endswith('zr')
                                       else constants.get(int(value[1:])))
                elif ins.mnemonic == 'add' and add:
                    constants[int(add[1])] = ('stack', int(add[2], 0))
                elif ins.mnemonic == 'str' and store:
                    value = 0 if store[1].endswith('zr') else constants.get(int(store[1][1:]))
                    slots[int(store[2] or '0', 0)] = value
                elif ins.mnemonic == 'bl':
                    if ins.operands[0].imm == scalar_helper:
                        output = constants.get(6)
                        measured = {'selector': constants.get(1), 'scalar_inputs': constants.get(4),
                                    'scalar_outputs': slots.get(output[1]) if isinstance(output, tuple) else 0 if output == 0 else None,
                                    'helper_call_address': hex(ins.address)}
                        break
                    for reg in range(19): constants.pop(reg, None)
                elif ins.mnemonic in ('ldr', 'ldur', 'ldrb', 'ldrh', 'ldrsw', 'adrp', 'adr', 'csel'):
                    register = re.match(r'[wx](\d+),', op)
                    if register: constants.pop(int(register[1]), None)
            if not measured or any(measured[k] is None for k in ('selector', 'scalar_inputs', 'scalar_outputs')):
                unresolved.append({'name': export['name'], 'reason': 'argument shape not proven by bounded constant analysis'})
                continue
            helper_ins = []
            for ins in disassemble(scalar_helper, 128):
                helper_ins.append(ins)
                if ins.mnemonic == 'ret': break
            def evidence(sequence):
                raw = b''.join(bytes(i.bytes) for i in sequence)
                return {'address': hex(sequence[0].address), 'bytes': len(raw), 'sha256': digest(raw),
                        'instructions': [{'address': hex(i.address), 'bytes': bytes(i.bytes).hex(),
                                          'mnemonic': i.mnemonic, 'operands': i.op_str} for i in sequence]}
            observations.append({'name': export['name'], 'entry': hex(entry), **measured,
                                 'implementation': evidence(instructions), 'helper': evidence(helper_ins)})
        if len(observations) < 8:
            raise ValueError('insufficient measured scalar calls: ' + str(unresolved))
        report = {'schema': 1, 'build': reference['build'], 'source': reference['source'],
                  'source_sha256': reference['source_sha256'], 'runtime_evidence_sha256': digest(reference_path.read_bytes()),
                  'library': library['path'], 'library_uuid': library['uuid'], 'iokit_uuid': iokit['uuid'],
                  'decoder': 'capstone ' + capstone.__version__, 'observations': observations,
                  'unresolved': unresolved, 'hardware_tested': False,
                  'limitations': ['Selectors and scalar counts are measured for this exact iPad runtime.',
                                  'Control value meanings, notifications, boot resources and data-plane ABI require separate implementation.',
                                  'This report neither implements AppleBaseband class matching nor enables CommCenter.']}
        target = root / 'docs/baseband/ipad-apple-control.json'
        target.write_text(json.dumps(report, indent=2) + '\n')
        return {'report': str(target), 'scalar_calls': len(observations), 'unresolved': unresolved}
    finally:
        image.close()


if __name__ == '__main__':
    print(json.dumps(analyze(), indent=2))
