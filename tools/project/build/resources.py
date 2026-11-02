"""Offline SoC firmware resource extraction."""
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import tempfile
from project.build import context as build_state


def usb_elf_reader(data, bits):
    """Read file-backed PT_LOAD bytes only; never infer zero-filled/live RAM."""
    if bits not in (32, 64):
        raise ValueError('unsupported USB firmware ELF class')
    size, phsize = (52, 32) if bits == 32 else (64, 56)
    if len(data) < size or data[:7] != b'\x7fELF' + bytes((1 if bits == 32 else 2, 1, 1)):
        raise ValueError('invalid USB firmware ELF header')
    machine = struct.unpack_from('<H', data, 18)[0]
    if machine != (40 if bits == 32 else 183):
        raise ValueError('unexpected USB firmware architecture')
    if bits == 32:
        phoff = struct.unpack_from('<I', data, 28)[0]
        stride, count = struct.unpack_from('<HH', data, 42)
    else:
        phoff = struct.unpack_from('<Q', data, 32)[0]
        stride, count = struct.unpack_from('<HH', data, 54)
    if stride != phsize or not 1 <= count <= 64 or phoff < size or phoff + count * stride > len(data):
        raise ValueError('invalid USB firmware program headers')
    loads = []
    for i in range(count):
        fields = struct.unpack_from('<8I' if bits == 32 else '<IIQQQQQQ', data, phoff + i * stride)
        if fields[0] != 1:
            continue
        offset, address, filesz, memsz = (fields[1], fields[2], fields[4], fields[5]) if bits == 32 else (fields[2], fields[3], fields[5], fields[6])
        if filesz > memsz or offset + filesz > len(data) or address + memsz > 1 << bits:
            raise ValueError('invalid USB firmware load segment')
        if filesz:
            if any(address < end and start < address + filesz for start, end, _ in loads):
                raise ValueError('overlapping USB firmware load segments')
            loads.append((address, address + filesz, offset))
    if not loads:
        raise ValueError('USB firmware has no file-backed loads')

    def read(address, length):
        if type(address) is not int or type(length) is not int or address < 0 or length < 1:
            raise ValueError('invalid USB firmware read')
        for start, end, offset in loads:
            if start <= address and address + length <= end:
                begin = offset + address - start
                return data[begin:begin + length]
        raise ValueError('USB firmware read is not file-backed')
    return read


def usb_hyp_assignments(data):
    """Decode the reviewed stock HYP layout, not a context allocation API."""
    if hashlib.sha256(data).hexdigest() != '4cde44ade81667b050df0b216e4b58fb69685dee441ba13f2e2289622103e638':
        raise ValueError('unsupported USB HYP firmware hash')
    read = usb_elf_reader(data, 64)
    count, pointer = struct.unpack('<QQ', read(0x8584b578, 16))
    if count != 6 or pointer != 0x858438c8:
        raise ValueError('unsupported USB HYP device table')
    devices = []
    for i in range(count):
        address = pointer + i * 0x14c8
        header = read(address, 0x80)
        name = header[4:24].split(b'\0', 1)[0].decode('ascii')
        assignment_count = header[16]
        if not 1 <= assignment_count <= 24:
            raise ValueError('invalid USB HYP assignment count')
        assignments = []
        for slot in range(assignment_count):
            record_address = address + 0x80 + slot * 0xd8
            record = read(record_address, 0xd8)
            sid_count = struct.unpack_from('<H', record, 22)[0]
            if sid_count > 48:
                raise ValueError('invalid USB HYP SID count')
            if not sid_count:
                continue
            vmid, guard = struct.unpack_from('<I', record, 8)[0], struct.unpack_from('<I', record, 16)[0]
            sids = [struct.unpack_from('<I', record, 24 + n * 4)[0] for n in range(sid_count)]
            assignments.append({'assignment_index': slot, 'record_address': hex(record_address),
                'declared_vmid': vmid, 'lookup_guard_word': guard,
                'lookup_eligible_in_initial_file': bool(guard), 'stage2_bank_byte': record[20],
                'smrs': [hex(sid) for sid in sids]})
        devices.append({'index': i, 'record_address': hex(address), 'name': name,
            'smmu_base': hex(struct.unpack_from('<I', header)[0]),
            'assignment_count': assignment_count, 'assignments': assignments})
    # This SID is the reference IORT named-component route, not a live binding.
    matches = []
    for device in devices:
        if device['smmu_base'] != '0x1680000':
            continue
        for row in device['assignments']:
            matched = [smr for smr in row['smrs'] if (int(smr, 16) ^ 0x1400) & 0xffff & ~((int(smr, 16) >> 16) & 0x7fff) == 0]
            if matched:
                matches.append({'device_index': device['index'], **row, 'matched_smrs': matched})
    smc_header, smc_parameters, smc_function = struct.unpack('<QQQ', read(0x85840320, 24))
    if (smc_header, smc_parameters, smc_function) != (0x02000c1800000000, 0x84, 0x85819b84):
        raise ValueError('unsupported USB HYP dynamic SID entry')
    return {'devices': devices, 'reference_usb_sid': '0x1400', 'reference_usb_smmu': '0x1680000',
        'reference_usb_matches': matches, 'device_lookup_function': '0x85821efc',
        'sid_lookup_function': '0x85824884', 'device_stride': '0x14c8', 'assignment_stride': '0xd8',
        'assignment_index_is_a_usb_context_grant': False,
        'dynamic_sid_call': {'smc_id': hex(smc_header >> 32), 'parameters': hex(smc_parameters),
            'handler': hex(smc_function), 'external_instance_mapping': '0x85821f18',
            'accepted_internal_device': 4, 'accepted_device_name': devices[4]['name'],
            'reference_usb_internal_device': 0, 'usb_allocator_contract': False,
            'scope': 'Reviewed handler requires the MMNOC internal device, then switches known security-domain configurations. It does not allocate USB contexts or return an ASID.'},
        'physical_namespace_or_free_bank_grant': None,
        'scope': 'File-initialized policy only. A zero lookup guard is skipped by the reviewed lookup; subsequent firmware initialization is not replayed.'}


def analyze_usb_resources(directory, output):
    """Reproduce static resource extraction offline without creating boot policy."""
    directory, output = Path(directory).resolve(), Path(output).resolve()
    pins = {
        'hyp.mbn': '4cde44ade81667b050df0b216e4b58fb69685dee441ba13f2e2289622103e638',
        'rpm.mbn': '9a624fbd215b72fd148b8a781656e717364067866236f1ce8ca4cc15b774cc24',
        'tz.mbn': 'd2c417987bb4210eb774b11c3cd61aa5cd4b1260d1dd3313506490ee9026d911',
        'devcfg.mbn': 'fc2b0a3d491c73006dc998b51b7277a575a457266def8677824c8e3c8d422577',
        'xbl.elf': '89e9d225db731fd3c7139ec0070eb234cd5c0aa3adbe124388727b55b1c764b7',
    }
    files, images = [], {}
    protected = {(directory / name).resolve() for name in pins}
    reference = build_state.ROOT / 'local/reference/qcom/msm8998/usb'
    manifest_path = reference / 'sources.json'
    manifest = json.loads(manifest_path.read_text())
    sources = []
    for name in ('board/gcc.c', 'board/vdd-level.h', 'board/regulator-level.h', 'board/clock-rpm.c', 'board/arm-smmu.c'):
        path = reference / name
        protected.add(path.resolve())
        data = path.read_bytes()
        actual = hashlib.sha256(data).hexdigest()
        if actual != manifest[name]['sha256']:
            raise ValueError('USB kernel reference hash mismatch: ' + name)
        sources.append({'file': str(path.relative_to(build_state.ROOT)), 'url': manifest[name]['url'], 'sha256': actual})
        images[name] = data
    if output in protected or output == manifest_path.resolve() or output.is_relative_to(directory):
        raise ValueError('USB extraction output must be outside its firmware/reference inputs')
    for name, expected in pins.items():
        path = directory / name
        if path.stat().st_size > 0x800000:
            raise ValueError('USB firmware exceeds the reviewed input size')
        data = path.read_bytes()
        actual = hashlib.sha256(data).hexdigest()
        if actual != expected:
            raise ValueError('unsupported USB firmware hash: ' + name)
        images[name] = data
        files.append({'file': str(path), 'bytes': len(data), 'sha256': actual})
    hyp = usb_hyp_assignments(images['hyp.mbn'])
    rpm = usb_elf_reader(images['rpm.mbn'], 32)
    # The published MBN uses physical addresses. Its Cortex-M code/data use
    # aliases smaller by0x200000; no analysis ELF is created or executed here.
    cpu_word = lambda address: struct.unpack('<I', rpm(address + 0x200000, 4))[0]
    services = {'request': hex(cpu_word(0xd14c)), 'command': hex(cpu_word(0xd150)),
                'registered_alternate': hex(cpu_word(0x5260)), 'alternate_callback': hex(cpu_word(0x525c) & ~1)}
    if services != {'request': '0x716572', 'command': '0x646d63', 'registered_alternate': '0x626473', 'alternate_callback': '0x4cd8'}:
        raise ValueError('unsupported USB RPM service layout')
    # These reviewed callers write *initial* EE requests inside the RPM.
    # They are outside the AP NpaDxe cache and can be replaced by later
    # requests. Preserve only the observed set; an absent sleep call is not 0.
    # Instructions are tied to the exact original MBN hash checked above.
    def proxy_vote(call, branch, ee, set_name, type_literal, key_address, value_at, instruction, value):
        if (rpm(call + 0x200000, 4).hex() != branch or
                rpm(value_at + 0x200000, len(bytes.fromhex(instruction))).hex() != instruction):
            raise ValueError('unsupported USB RPM initial proxy caller')
        return {'call_address': hex(call), 'ee': ee, 'set': set_name,
            'resource_type': hex(cpu_word(type_literal)), 'resource_id': 0,
            'key': hex(cpu_word(key_address)), 'value': value,
            'type_literal_address': hex(type_literal), 'key_address': hex(key_address),
            'value_instruction_address': hex(value_at), 'value_instruction_bytes': instruction}
    spm_key = cpu_word(0x12870)
    initial_votes = [
        proxy_vote(0x12804, 'fcf758fb', 0, 'active', 0x12874, spm_key, 0x127e8, '0125', 1),
        proxy_vote(0x12826, 'fcf765fb', 0, 'sleep', 0x12874, spm_key, 0x1280e, '0026', 0),
        proxy_vote(0x12846, 'fcf737fb', 1, 'active', 0x12874, spm_key, 0x127e8, '0125', 1),
        proxy_vote(0x12866, 'fcf745fb', 1, 'sleep', 0x12874, spm_key, 0x1280e, '0026', 0),
        proxy_vote(0x250f6, 'e9f7dffe', 0, 'active', 0x2513c, cpu_word(0x25138), 0x250d0, '0120', 1),
        proxy_vote(0x25636, 'e9f73ffc', 0, 'active', 0x25648, 0x25644, 0x25610, '4ff48071', 256),
    ]
    gcc = images['board/gcc.c'].decode()
    table = re.search(r'static struct clk_freq_tbl ftbl_usb30_master_clk_src\[\] = \{(.*?)\n\};', gcc, re.S)
    clock = re.search(r'static struct rcg_clk usb30_master_clk_src = \{(.*?)\n\};', gcc, re.S)
    mapping = re.search(r'VDD_DIG_FMAX_MAP4\((.*?)\)', clock[1], re.S) if clock else None
    if not table or not mapping:
        raise ValueError('unsupported USB kernel clock table')
    rates = [int(v) for v in re.findall(r'\bF\(\s*(\d+)\s*,', table[1])]
    pairs = re.findall(r'\b([A-Z_]+)\s*,\s*(\d+)', mapping[1])
    vdd, regulator = images['board/vdd-level.h'].decode(), images['board/regulator-level.h'].decode()
    # vdd_dig is an ordered voltage array, so map its enum members by order.
    enum = re.search(r'enum vdd_dig_levels\s*\{(.*?)\}', vdd, re.S)
    array = re.search(r'static int vdd_corner\[\]\s*=\s*\{(.*?)\}', vdd, re.S)
    if not enum or not array:
        raise ValueError('unsupported USB kernel voltage mapping')
    names = re.findall(r'\bVDD_DIG_([A-Z0-9_]+)\s*,', enum[1])
    levels = re.findall(r'\bRPM_REGULATOR_LEVEL_([A-Z0-9_]+)\b', array[1])
    values = {k: int(v) for k, v in re.findall(r'\bRPM_REGULATOR_LEVEL_([A-Z0-9_]+)\s*=\s*(\d+)', regulator)}
    if len(names) != len(levels) or len(pairs) != 4:
        raise ValueError('unsupported USB kernel voltage array')
    corners = dict(zip(names, levels))
    fmax = [{'corner': k, 'maximum_hz': int(hz), 'rpm_level': values[corners[k]]} for k, hz in pairs]
    supported = [{'hz': hz, 'minimum_active_cx_level': next(r['rpm_level'] for r in fmax if hz <= r['maximum_hz'])} for hz in rates]
    report = {'version': 1, 'component': 'offline-usb-resource-evidence',
        'firmware_files': files, 'kernel_sources': sources,
        'hyp_static_namespace': hyp,
        'usb_core_clock': {'rates': supported, 'fmax': fmax,
            'scope': 'USB minimum active CX requirement; does not specify the complete AP baseline.'},
        'rpm_wire': {'cpu_alias_delta': '0x200000', 'reviewed_v0_dispatch': '0xce40',
            'reviewed_v1_dispatch': '0xcfcc', 'reviewed_service_registration': '0x5238', **services,
            'full_AP_active_sleep_readback_contract_proven': False,
            'scope': 'The reviewed ordinary path applies requests. The identified alternate service is SystemDB, whose RPM dump emits debug events; no complete per-set AP baseline reply is established.'},
        'rpm_initial_proxy_votes': {'reviewed_active_function': '0xeeb8',
            'reviewed_sleep_function': '0xeef4', 'resource_ee_request': '0xaba8',
            'votes': initial_votes, 'complete_AP_active_sleep_baseline': False,
            'current_device_values_proven': False,
            'scope': 'File-backed initial proxy callers only. APSS SPM active/sleep, XO active and CX active are identified; MPSS SPM is a separate EE. Later RPM/AP requests are not replayed. No missing set or resource is inferred.'},
        'production_inputs': {'ap_domain_baselines': None, 'ap_clock_baselines': None,
            'ap_clock_limits': None, 'ap_interconnect_baselines': None,
            'ap_regulator_floors': None, 'usb_context_namespace_grant': None},
        'firmware_matches_current_device_proven': False, 'runtime_resource_grants': 0,
        'production_profile_created': False, 'production_USB_nodes_enabled': False,
        'firmware_executed': False, 'hardware_tested': False}
    output.parent.mkdir(parents=True, exist_ok=True)
    # Commit only a fully decoded report; a rejected input preserves old output.
    handle, name = tempfile.mkstemp(prefix=output.name + '.', suffix='.tmp', dir=output.parent)
    temporary = Path(name)
    try:
        with os.fdopen(handle, 'w') as stream:
            stream.write(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    return report


def collect_power_clocks():
    """Generate reviewed CC descriptors from hash-pinned reference C, never execute it."""
    from project.build import native as _component_native
    from fractions import Fraction
    reference = build_state.ROOT / 'local/reference/qcom/msm8998/power'
    manifest = json.loads((reference / 'sources.json').read_text())
    for filename, entry in manifest.items():
        if _component_native.digest(reference / filename) != entry['sha256']:
            raise RuntimeError('power reference checksum mismatch: ' + filename)
    for module, filename, binding in [('QcomMMCC', 'mmcc-msm8998.c', 'qcom,mmcc-msm8998.h'),
                                       ('QcomGPUCC', 'gpucc-msm8998.c', 'qcom,gpucc-msm8998.h')]:
        source = (reference / filename).read_text()
        ids = {name: int(value) for name, value in re.findall(r'^#define\s+(\w+)\s+(\d+)', (reference / binding).read_text(), re.M)}
        exposed = {name: ids[key] for key, name in re.findall(r'\[(\w+)\] = &(\w+)\.clkr', source)}
        def structures(kind):
            return dict(re.findall(r'static (?:const )?struct ' + kind + r' (\w+)(?:\[\])? = \{(.*?)\n\};', source, re.S))
        def field(body, key, default=None):
            match = re.search(r'\.'+key+r' = (\w+)', body)
            if match: return match[1]
            if default is not None: return default
            raise RuntimeError('missing ' + key)
        plls = structures('clk_alpha_pll'); roots = structures('clk_rcg2'); branches = structures('clk_branch'); domains = structures('gdsc')
        tables = structures('freq_tbl'); maps = structures('parent_map')
        # Linux v6.12 omits MND width on MCLK and has two inconsistent ratios.
        # Qualcomm uses full-register MND writes and sign-extends 8/16-bit
        # hardware readback in clock-local2.c. Keep this correction explicit.
        vendor = (reference/'clock-mmss-8998.c').read_text()
        mclk_table = re.search(r'static struct clk_freq_tbl ftbl_mclk_clk_src\[\] = \{(.*?)\n\};',vendor,re.S)[1]
        mclk_ratios = {int(hz):(int(m),int(n)) for hz,m,n in re.findall(r'F_MM\(\s*(\d+),[^,]+,[^,]+,\s*(\d+),\s*(\d+)\)',mclk_table)}
        pll_index = {name: i for i, name in enumerate(plls)}
        parents = {'P_XO': -1, 'P_GPLL0': -2, 'P_GPLL0_DIV': -3}
        for name, i in pll_index.items(): parents['P_'+name.upper()+'_OUT_EVEN'] = i
        domain_ids = {name: ids[key] for key, name in re.findall(r'\[(\w+)\] = &(\w+),', source) if name in domains}
        output = ['// SPDX-License-Identifier: GPL-2.0-only', '// Generated by tools/project/build/toolchain.py collect-power from pinned Linux v6.12 data.',
                  '#pragma once', '#include <QcomClockHardware.h>', 'namespace '+module+'Data {', 'using namespace QcomPower;']
        for name, body in roots.items():
            table = field(body, 'freq_tbl', '')
            parent_map = {name: int(value) for name, value in re.findall(r'\{ (\w+), (\d+) \}', maps[field(body, 'parent_map')])}
            frequencies = []
            if table:
                body_table = tables[table]
                for line in body_table.splitlines():
                    if 'F(' not in line: continue
                    match = re.search(r'F\((\d+), (\w+), ([\d.]+), (\d+), (\d+)\)', line)
                    if not match: raise RuntimeError('unreviewed frequency expression: '+line)
                    hz, parent, div, m, n = match.groups()
                    if module == 'QcomMMCC' and re.fullmatch(r'mclk[0-3]_clk_src',name):
                        if not re.search(r'static struct rcg_clk '+name+r' = \{\s*\.cmd_rcgr_reg = \w+,\s*\.set_rate = set_rate_mnd,',vendor):
                            raise RuntimeError('MCLK lacks Qualcomm MND evidence: '+name)
                        m,n = mclk_ratios[int(hz)]
                    frequencies.append((int(hz),parent_map[parent],int(Fraction(div)*2-1),int(m),int(n)))
            if frequencies:
                output.append('static constexpr CCRate rates_'+name+'[] = {'+','.join('{%d,%d,%d,%d,%d}'%f for f in frequencies)+'};')
            roots[name] = (body, frequencies, parent_map)
        output.append('static constexpr CCPll plls[] = {')
        for name, body in plls.items():
            reg = int(field(body,'offset'),0)
            mask = re.search(r'\.enable_mask = BIT\((\d+)\)',body)
            output.append('    {%d,%d,"%s",0x%x,0x%x,0x%x,%s},'%(exposed[name],exposed[name+'_out_even'],name,reg,
                int(field(body,'enable_reg','0'),0),1<<int(mask[1]) if mask else 0,'false' if module=='QcomGPUCC' else 'true'))
        output += ['};','static constexpr CCRoot roots[] = {']
        for name, (body, frequencies, parent_map) in roots.items():
            encoded = [-100]*8
            for parent, selector in parent_map.items(): encoded[selector] = parents.get(parent,-100)
            output.append('    {%d,"%s",0x%x,%d,{%s},%s,%d},'%(exposed[name],name,int(field(body,'cmd_rcgr'),0),
                (32 if module == 'QcomMMCC' and re.fullmatch(r'mclk[0-3]_clk_src',name) else int(field(body,'mnd_width','0'))),','.join(map(str,encoded)),('rates_'+name) if frequencies else 'nullptr',len(frequencies)))
        output += ['};','static constexpr CCBranch branches[] = {']
        for name, body in branches.items():
            parent = re.search(r'&(\w+)\.clkr.hw',body)
            root = exposed.get(parent[1],0xffffffff) if parent and parent[1] in roots else 0xffffffff
            reg = int(field(body,'enable_reg'),0); bit = int(re.search(r'\.enable_mask = BIT\((\d+)\)',body)[1])
            if module=='QcomGPUCC': domain = 2 if name=='gfx3d_clk' else 1
            elif name.startswith('mdss'): domain = 4
            elif name.startswith('video_subcore0'): domain = 2
            elif name.startswith('video_subcore1'): domain = 3
            elif name.startswith('video'): domain = 1
            elif name.startswith('camss_vfe0'): domain = 6
            elif name.startswith('camss_vfe1'): domain = 7
            elif name.startswith('camss_cpp'): domain = 8
            elif name.startswith('camss'): domain = 5
            elif name.startswith('bimc_smmu'): domain = 9
            else: domain = -1
            hwcg = re.search(r'\.hwcg_bit = (\d+)',body)
            output.append('    {%d,"%s",0x%x,0x%x,%d,%d,%s,%s,0x%x},'%(exposed[name],name,reg,1<<bit,root,domain,
                'true' if 'BRANCH_HALT_SKIP' in body else 'false','true' if 'CLK_IS_CRITICAL' in body else 'false',1<<int(hwcg[1]) if hwcg else 0))
        output += ['};','static constexpr CCReset resets[] = {']
        reset_map = dict(re.findall(r'\[(\w+)\] = \{ (0x[0-9a-f]+) \}',source))
        for key, reg in reset_map.items(): output.append('    {%d,%s},'%(ids[key],reg))
        output += ['};']
        for name, body in domains.items():
            cxcs = re.search(r'\.cxcs = \(unsigned int \[\]\)\{(.*?)\}',body,re.S)
            if cxcs: output.append('static constexpr uint32_t memories_'+name+'[] = {'+','.join(re.findall(r'0x[0-9a-f]+',cxcs[1]))+'};')
        output.append('static constexpr CCDomain domains[] = {')
        flags = {'VOTABLE':1,'HW_CTRL':2,'CLAMP_IO':4,'SW_RESET':8,'AON_RESET':16,'NO_RET_PERIPH':32}
        for name, body in domains.items():
            parent = re.search(r'\.parent = &(\w+)\.pd',body); cxcs = re.search(r'\.cxc_count = (\d+)',body)
            reset = re.search(r'\.resets = \(unsigned int \[\]\)\{ (\w+) \}',body)
            f = re.search(r'\.flags = ([^,\n]+)',body); bits = 0
            if f:
                for flag in f[1].split('|'): bits |= flags[flag.strip()]
            output.append('    {%d,0x%x,0x%x,0x%x,0x%x,%d,%d,%s,%d},'%(domain_ids[name],int(field(body,'gdscr'),0),
                int(field(body,'gds_hw_ctrl','0'),0),int(field(body,'clamp_io_ctrl','0'),0),int(reset_map[reset[1]],0) if reset else 0,
                domain_ids[parent[1]] if parent else -1,bits,'memories_'+name if cxcs else 'nullptr',int(cxcs[1]) if cxcs else 0))
        output += ['};','static constexpr CCDescription description = {roots,sizeof(roots)/sizeof(roots[0]),plls,sizeof(plls)/sizeof(plls[0]),',
                   '    branches,sizeof(branches)/sizeof(branches[0]),resets,sizeof(resets)/sizeof(resets[0]),domains,sizeof(domains)/sizeof(domains[0]),'+
                   ('true' if module=='QcomGPUCC' else 'false')+'};','}']
        (build_state.DRIVERS_ROOT/'clock'/module/'ClockTables.h').write_text('\n'.join(output)+'\n')
        print(module+': '+str(len(roots))+' roots, '+str(len(branches))+' branches, '+str(len(domains))+' domains')
