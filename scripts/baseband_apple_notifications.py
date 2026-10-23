"""Measure the primary iPad interest-notification ABI; never load a framework."""
import bisect
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build/baseband/python'))
import capstone
from apfs_sealed import SealedImage
from baseband_dyld import Cache, Files, export_trie


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def starts_from_uleb(data, base, text_bytes):
    """LC_FUNCTION_STARTS deltas; reject truncated/overflowing/non-code data."""
    starts, cursor, address = [], 0, base
    while cursor < len(data):
        delta = 0
        for shift in range(0, 64, 7):
            require(cursor < len(data), 'truncated function-start delta')
            byte = data[cursor]; cursor += 1
            require(shift != 63 or byte <= 1, 'function-start delta overflow')
            delta |= (byte & 127) << shift
            if not byte & 128:
                break
        else:
            raise ValueError('function-start delta too long')
        if not delta:
            require(not any(data[cursor:]), 'nonzero data after function-start terminator')
            return starts
        require(delta <= base + text_bytes - address, 'function start outside text')
        address += delta
        require(address < base + text_bytes and not address & 3, 'unaligned/outside function start')
        starts.append(address)
        require(len(starts) <= 100000, 'too many function starts')
    raise ValueError('missing function-start terminator')


def layout(cache, base):
    header = cache.read(base, 32)
    magic, cpu, _, _, count, size, _, _ = struct.unpack('<8I', header)
    require(magic == 0xfeedfacf and cpu == 0x100000c and count <= size // 8, 'unexpected Mach-O')
    raw, pos, segments = cache.read(base, size + 32), 32, []
    function_data = exports = text = None
    for _ in range(count):
        command, length = struct.unpack_from('<II', raw, pos)
        require(length >= 8 and pos + length <= len(raw), 'Mach-O command bounds')
        if command == 0x19:
            require(length >= 72, 'short segment')
            name, va, vs, off, fs = struct.unpack_from('<16s4Q', raw, pos + 8)
            segments.append((va, vs, off, fs))
            if name.rstrip(b'\0') == b'__TEXT': text = (va, vs)
        elif command in (0x26, 0x80000033):
            require(length == 16, 'short linkedit command')
            target = struct.unpack_from('<II', raw, pos + 8)
            if command == 0x26: function_data = target
            else: exports = target
        elif command == 0x80000022:
            require(length == 48, 'short dyld command')
            exports = struct.unpack_from('<II', raw, pos + 40)
        pos += length
    require(pos == len(raw) and text and function_data, 'missing function-start evidence')

    def file_read(offset, length):
        require(length <= 4 * 1024 * 1024, 'excessive linkedit data')
        for va, _, off, size in segments:
            if off <= offset <= offset + length <= off + size:
                return cache.read(va + offset - off, length)
        raise ValueError('unmapped Mach-O file range')

    starts_raw = file_read(*function_data)
    return {'text': text, 'starts': starts_from_uleb(starts_raw, *text),
            'starts_sha256': hashlib.sha256(starts_raw).hexdigest(),
            'exports': export_trie(file_read(*exports), base) if exports else []}


def analyze(root=ROOT, output=None):
    root = Path(root)
    reference_path = root / 'docs/baseband/ipad-runtime-libraries.json'
    reference = json.loads(reference_path.read_text())
    require(reference['build'] == '22H374', 'notification analysis requires the primary iPad build')
    source = root / reference['source']
    target = Path(output or root / 'docs/baseband/ipad-apple-notifications.json').resolve()
    protected = (root / 'firmware', root / 'build/apfs', root / 'docs/baseband/reference')
    require(target.suffix == '.json' and target not in (source.resolve(), reference_path.resolve()) and
            not any(target.is_relative_to(p.resolve()) for p in protected), 'output may not overwrite input sources')
    image = SealedImage(source)
    try:
        require(image.original_root_hash.hex() == reference['input_provenance']['apfs_seal_sha256'], 'different APFS seal')
        cache = Cache(Files(image, root / 'build/baseband/reference/libzbitmap.so'))
        library = next(l for l in reference['libraries'] if l['path'].endswith('/libTelephonyBasebandDynamic.dylib'))
        actual = cache.library(library['path'])
        require(all(actual[k] == library[k] for k in ('uuid', 'commands_sha256')), 'different runtime library')
        info = layout(cache, int(actual['address'], 16))
        iokit = cache.library('/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit')
        iokit_info = layout(cache, int(iokit['address'], 16))
        names = {int(e['address'], 16): e['name'] for e in iokit_info['exports'] if 'address' in e}
        decoder = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); decoder.detail = True

        def dis(address, count):
            return list(decoder.disasm(cache.read(address, count), address))

        def function(address):
            i = bisect.bisect_left(info['starts'], address)
            require(i < len(info['starts']) and info['starts'][i] == address, 'not an actual function start')
            end = info['starts'][i + 1] if i + 1 < len(info['starts']) else sum(info['text'])
            require(0 < end - address <= 8192, 'function exceeds analysis bound')
            instructions = dis(address, end - address)
            require(sum(i.size for i in instructions) == end - address, 'incomplete ARM64 function decoding')
            return instructions

        def evidence(instructions):
            raw = b''.join(bytes(i.bytes) for i in instructions)
            return {'address': hex(instructions[0].address), 'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest(),
                    'instructions': [{'address': hex(i.address), 'bytes': bytes(i.bytes).hex(),
                                      'mnemonic': i.mnemonic, 'operands': i.op_str} for i in instructions]}

        def resolve(address):
            ins = dis(address, 12)
            if (len(ins) == 3 and [i.mnemonic for i in ins] == ['adrp', 'add', 'br'] and
                    ins[0].op_str.startswith('x16, ') and ins[1].op_str.startswith('x16, x16, ') and ins[2].op_str == 'x16'):
                address = ins[0].operands[1].imm + ins[1].operands[2].imm
            return names.get(address)

        def constant_addresses(instructions):
            registers, values = {}, {}
            for ins in instructions:
                parts = ins.op_str.split(', ')
                if ins.mnemonic in ('adr', 'adrp'):
                    registers[parts[0]] = ins.operands[1].imm
                elif ins.mnemonic == 'add' and len(parts) == 3 and parts[1] in registers and parts[2].startswith('#'):
                    registers[parts[0]] = registers[parts[1]] + ins.operands[2].imm
                    values[ins.address] = (parts[0], registers[parts[0]])
            return values

        def string(address):
            raw = cache.read(address, 512); end = raw.find(b'\0')
            require(0 <= end <= 511, 'unterminated diagnostic string')
            raw = raw[:end + 1]
            return {'address': hex(address), 'text': raw[:-1].decode('utf8'), 'bytes': len(raw),
                    'sha256': hashlib.sha256(raw).hexdigest()}

        registrations, callbacks, helper = {}, {}, None
        for short in ('RegisterForReset', 'RegisterForEvents'):
            name = '_TelephonyBaseband' + short
            exported = next(e for e in actual['exports'] if e['name'] == name)
            wrapper = function(int(exported['address'], 16))
            require(len(wrapper) == 2 and wrapper[-1].mnemonic == 'b', 'changed registration wrapper')
            impl = function(wrapper[-1].operands[0].imm)
            matches = []
            for ins in impl:
                if ins.mnemonic != 'bl' or ins.operands[0].imm not in info['starts']: continue
                child = function(ins.operands[0].imm)
                if any(i.mnemonic == 'bl' and resolve(i.operands[0].imm) == '_IOServiceAddInterestNotification' for i in child):
                    matches.append((ins.address, child))
            require(len(matches) == 1, 'ambiguous interest-registration helper')
            call_address, child = matches[0]
            if helper is not None: require(helper[0].address == child[0].address, 'different registration helpers')
            helper = child
            candidates = [(at, value) for at, (reg, value) in constant_addresses(impl).items() if reg == 'x5' and at < call_address]
            require(len(candidates) == 1, 'callback argument is not a single measured constant')
            callback = function(candidates[0][1]); callbacks[short] = callback
            # This tiny block invoke restores the caller's callback/refcon.
            closures = []
            for _, (reg, value) in constant_addresses(impl).items():
                if reg != 'x8' or value not in info['starts']: continue
                candidate = function(value)
                if [(i.mnemonic, i.op_str) for i in candidate] == [('ldp', 'x4, x0, [x0, #0x20]'), ('br', 'x4')]:
                    closures.append(candidate)
            require(len(closures) == 1, 'callback/refcon forwarding block not established')
            tokens = {(i.mnemonic, i.op_str) for i in callback}
            require(all(('mov', op) in tokens for op in ('x19, x3', 'x20, x2', 'x21, x1', 'x22, x0',
                                                       'x1, x21', 'x2, x20', 'x3, x19')), 'raw callback argument forwarding changed')
            registrations[name] = {'wrapper': evidence(wrapper), 'implementation': evidence(impl),
                'callback': evidence(callback), 'callback_invoke': evidence(closures[0]),
                'callback_arguments': ['original refcon', 'io_service_t service', 'raw uint32 messageType', 'raw messageArgument'],
                'message_argument_payload_layout_proven': False}

        interest_addresses = [value for _, (reg, value) in constant_addresses(helper).items() if reg == 'x9']
        require(len(interest_addresses) == 1, 'interest string is ambiguous')
        interest = string(interest_addresses[0]); require(interest['text'] == 'IOGeneralInterest', 'different interest kind')
        events = callbacks['RegisterForEvents']
        prefix = events[:24]
        tokens = [(i.mnemonic, i.op_str) for i in prefix]
        require(('mov', 'w8, #0x8000') in tokens and ('movk', 'w8, #0x1c00, lsl #16') in tokens and
                ('add', 'w8, w2, w8') in tokens and ('cmp', 'w8, #0x11') in tokens and
                ('ldrb', 'w11, [x9, x8]') in tokens and ('add', 'x10, x10, x11, lsl #2') in tokens and
                ('br', 'x10') in tokens, 'different notification switch shape')
        table_address = next(value for _, (reg, value) in constant_addresses(prefix).items() if reg == 'x9')
        branch_base = next(i.operands[1].imm for i in prefix if i.mnemonic == 'adr' and i.op_str.startswith('x10, '))
        table = cache.read(table_address, 18)
        by_address = {i.address: i for i in events}
        messages = []
        for index, slot in enumerate(table):
            address = branch_base + slot * 4
            require(address in by_address, 'notification switch target outside the function')
            block = []
            for ins in events:
                if ins.address < address: continue
                block.append(ins)
                if ins.mnemonic in ('b', 'bl', 'ret'): break
            labels = [value for _, (reg, value) in constant_addresses(block).items() if reg == 'x1']
            require(len(labels) == 1, 'notification diagnostic label is ambiguous')
            messages.append({'message_type': hex(0xe3ff8000 + index), 'switch_target': hex(address),
                             'diagnostic_label': string(labels[0])})
        special = []
        reset = callbacks['RegisterForReset']
        for i, ins in enumerate(reset[:-2]):
            if ins.mnemonic == 'mov' and ins.op_str.startswith('w8, #') and reset[i + 1].mnemonic == 'movk':
                high = reset[i + 1]
                if high.op_str == 'w8, #0xe3ff, lsl #16' and reset[i + 2].mnemonic == 'cmp':
                    special.append(hex(0xe3ff0000 | ins.operands[1].imm))
        require(set(special) == {'0xe3ff8007', '0xe3ff800d'}, 'different reset special cases')
        report = {'schema': 1, 'build': reference['build'], 'source': reference['source'],
            'source_sha256': reference['source_sha256'], 'apfs_seal_sha256': image.original_root_hash.hex(),
            'runtime_evidence_sha256': hashlib.sha256(reference_path.read_bytes()).hexdigest(),
            'library': actual['path'], 'library_uuid': actual['uuid'], 'commands_sha256': actual['commands_sha256'],
            'function_starts_sha256': info['starts_sha256'], 'iokit_uuid': iokit['uuid'],
            'decoder': 'capstone ' + capstone.__version__, 'registrations': registrations,
            'interest': interest, 'registration_helper': evidence(helper),
            'switch_table': {'address': hex(table_address), 'bytes': table.hex(), 'branch_base': hex(branch_base),
                             'sha256': hashlib.sha256(table).hexdigest()},
            'messages': messages, 'reset_internal_special_cases': special,
            'limitations': ['Diagnostic labels and callback arguments come from this exact iPad runtime.',
                'Labels are not proof of a Qualcomm hardware state, message ordering or payload layout.',
                'The driver does not yet publish this Apple interest ABI or claim telephony readiness.'],
            'apple_driver_notification_abi_implemented': False, 'hardware_tested': False}
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
        return report
    finally:
        image.close()


if __name__ == '__main__':
    result = analyze()
    print(json.dumps({'build': result['build'], 'messages': len(result['messages']),
                      'hardware_tested': result['hardware_tested']}))
