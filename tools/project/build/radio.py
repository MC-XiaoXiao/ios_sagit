"""Wi-Fi and Bluetooth firmware and runtime preparation."""
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import shlex
import struct
import tempfile
import zipfile
from project.build import context as build_state


def bluetooth_bundle_configuration(profile, patch, nvm):
    """Validate explicit board policy against the existing RPM catalog and assets."""
    def integer(value, maximum=0xffffffff):
        if type(value) is not int or not 0 <= value <= maximum:
            raise ValueError('Bluetooth policy integer out of range')
        return value
    if profile.get('version') != 1 or type(profile['version']) is not int:
        raise ValueError('unsupported Bluetooth profile')
    identity = profile['controller']
    product, soc, rom = [integer(identity[k], 0xffff if k == 'rom' else 0xffffffff) for k in ('product','soc','rom')]
    if not soc or not rom:
        raise ValueError('Bluetooth ROM identity must be explicit')
    for key, data, kind, limit in [('patch', patch, 1, 0x100000), ('nvm', nvm, 2, 0x10000)]:
        if len(data) < (28 if kind == 1 else 16) or len(data) > limit:
            raise ValueError('Bluetooth asset size out of range')
        header, = struct.unpack_from('<I', data)
        if header & 255 != kind or header >> 8 != len(data)-4:
            raise ValueError('Bluetooth asset TLV header/length mismatch')
        if hashlib.sha256(data).hexdigest() != profile[key]['sha256']:
            raise ValueError('Bluetooth asset hash mismatch')
    if patch[14] > 3 or struct.unpack_from('<I', patch, 8)[0] > len(patch)-28 or struct.unpack_from('<HH', patch, 16) != (product & 0xffff, rom):
        raise ValueError('Bluetooth patch does not match the declared ROM')
    pos = 4
    while pos < len(nvm):
        if len(nvm)-pos < 12:
            raise ValueError('truncated Bluetooth NVM tag')
        length, = struct.unpack_from('<H', nvm, pos+2)
        if length > len(nvm)-pos-12:
            raise ValueError('Bluetooth NVM tag outside image')
        pos += 12+length
    if prepare_bluetooth_transport_nvm(nvm)[0] != nvm:
        raise ValueError('Bluetooth NVM transport parameters were not prepared')
    bluetooth_local_address(profile,nvm)
    rails = profile['rails']
    if not 1 <= len(rails) <= 6 or len({r['id'] for r in rails}) != len(rails):
        raise ValueError('ambiguous Bluetooth rails')
    catalog={int(i):(int(lo),int(step),int(hi)) for i,lo,step,hi in re.findall(
        r'\{(\d+),0x[0-9a-f]+,\d+,(\d+),(\d+),(\d+),"[^"]+"\}',
        (build_state.ROOT/'drivers/power/QcomRPM/RPMRails.h').read_text())}
    if not catalog:
        raise ValueError('Bluetooth RPM resource catalog unavailable')
    records = []
    for rail in rails:
        identifier = integer(rail['id'], 0xfff)
        voltage, lower, upper, floor = [integer(rail[k], 4000000) for k in ('microvolts','minimum','maximum','boot_floor')]
        if not identifier or not 300000 <= lower <= voltage <= upper or (floor and not lower <= floor <= upper):
            raise ValueError('invalid Bluetooth rail limits')
        hardware=catalog.get(identifier)
        if not hardware or not hardware[1] or not hardware[0]<=lower<=upper<=hardware[2] or \
                (voltage-hardware[0])%hardware[1] or (floor and (floor-hardware[0])%hardware[1]):
            raise ValueError('Bluetooth rail is outside the RPM resource catalog')
        records += [identifier,voltage]
    pins = profile['pins']
    if len(pins) != 4 or [p['pin'] for p in pins] != [45,46,47,48]:
        raise ValueError('Bluetooth requires BLSP1 UART3 GPIO45..48')
    mux = [integer(p['configuration'], 0x1ff) for p in pins]
    if any((cfg >> 2) & 15 != 2 for cfg in mux):
        raise ValueError('incorrect BLSP1 UART3 mux selector')
    cells = [1,104,product,soc,rom,len(rails),*records,*([0]*(12-len(records))),*mux,len(patch),len(nvm),0,0]
    return struct.pack('<26I',*cells)


def bluetooth_local_address(profile,nvm):
    address=profile.get('local_address','')
    if not isinstance(address,str) or not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}',address):
        raise ValueError('Bluetooth profile requires an explicit local_address')
    canonical=bytes.fromhex(address.replace(':',''))
    if canonical[0]&1 or canonical in (bytes(6),b'\xff'*6):
        raise ValueError('invalid Bluetooth local address')
    found=False
    pos=4
    while pos<len(nvm):
        if len(nvm)-pos<12:
            raise ValueError('truncated Bluetooth NVM tag')
        tag,length=struct.unpack_from('<HH',nvm,pos)
        if length>len(nvm)-pos-12:
            raise ValueError('Bluetooth NVM tag outside image')
        if tag==2:
            if found or length!=6 or nvm[pos+12:pos+18]!=canonical[::-1]:
                raise ValueError('Bluetooth NVM address does not match local_address')
            found=True
        pos+=12+length
    if not found:
        raise ValueError('Bluetooth NVM lacks an address tag')
    return canonical


def prepare_bluetooth_transport_nvm(data):
    """Prepare only WCN3990 host transport bits; preserve RF/address tags."""
    if len(data) < 4 or struct.unpack_from('<I',data)[0] != ((len(data)-4)<<8)|2:
        raise ValueError('invalid Bluetooth transport NVM')
    result=bytearray(data)
    found=set()
    changes=[]
    pos=4
    while pos<len(result):
        if len(result)-pos<12:
            raise ValueError('truncated Bluetooth NVM tag')
        tag,length=struct.unpack_from('<HH',result,pos)
        if length>len(result)-pos-12:
            raise ValueError('Bluetooth NVM tag outside image')
        if tag in (17,27):
            if tag in found or length<(3 if tag==17 else 1):
                raise ValueError('ambiguous Bluetooth transport NVM tag')
            found.add(tag)
            payload=pos+12
            updates=[(0,result[payload]|0x80),(2,0x11)] if tag==17 else [(0,result[payload]|1)]
            for offset,value in updates:
                absolute=payload+offset
                if result[absolute]!=value:
                    changes.append({'tag':tag,'offset':offset,'before':result[absolute],'after':value})
                    result[absolute]=value
        pos+=12+length
    if found!={17,27}:
        raise ValueError('Bluetooth NVM requires HCI and sleep transport tags')
    return bytes(result),changes


def prepare_bluetooth_daemon(source):
    """Select the observed native H4/POSIX socket path, preserving launchd IPC."""
    from project.build import native as _component_native
    source=Path(source)
    model=json.loads((build_state.FIRMWARE_DIR / 'models/bluetooth.json').read_text())
    record=model['user_space']['files']['/System/Library/LaunchDaemons/com.apple.bluetoothd.plist']
    if _component_native.digest(source)!=record['sha256']:
        raise ValueError('unsupported Bluetooth launchd source')
    daemon=plistlib.loads(source.read_bytes())
    if daemon.get('Label')!='com.apple.bluetoothd' or daemon.get('ProgramArguments')!=['/usr/sbin/bluetoothd'] or \
            daemon.get('UserName')!='mobile' or not daemon.get('MachServices'):
        raise ValueError('unsupported Bluetooth launchd contract')
    environment=daemon.setdefault('EnvironmentVariables',{})
    transport='H4:POSIX:com.apple.uart.bluetooth,3200000'
    if environment.get('BT_HCI_TRANSPORT',transport)!=transport:
        raise ValueError('conflicting native Bluetooth transport')
    environment['BT_HCI_TRANSPORT']=transport
    return plistlib.dumps(daemon,sort_keys=False)


def prepare_bluetooth_stack(reference, output):
    """Stage native launchd IPC plus the QCA backend; never install or run it."""
    from project.build import native as _component_native
    from project.firmware.macho import verify_macho
    from project.firmware.trustcache import development_module, module_info
    reference,output=Path(reference),Path(output)
    model=json.loads((build_state.FIRMWARE_DIR / 'models/bluetooth.json').read_text())['user_space']['files']
    # Fail closed on another OS revision or any modified native input.
    for source,record in model.items():
        if _component_native.digest(reference/Path(source).name) != record['sha256']:
            raise ValueError('unsupported Bluetooth system source '+source)
    daemon=prepare_bluetooth_daemon(reference/'com.apple.bluetoothd.plist')
    blue=plistlib.loads((reference/'com.apple.BlueTool.plist').read_bytes())
    if blue != model['/System/Library/LaunchDaemons/com.apple.BlueTool.plist']['plist']:
        raise ValueError('unsupported BlueTool launchd contract')
    blue['ProgramArguments']=['/usr/libexec/bluetoothbootstrap','-R']
    binary=build_state.BUILD/'output/bluetoothbootstrap'
    report=json.loads(Path(str(binary)+'.json').read_text())
    if _component_native.digest(binary) != report['sha256']:
        raise ValueError('modified Bluetooth backend')
    for path,sha in report['inputs'].items():
        if _component_native.digest(build_state.ROOT/path) != sha:raise ValueError('stale Bluetooth backend; rebuild '+path)
    signature=verify_macho(binary.read_bytes())
    if signature['identifier'] != 'org.darwin.development.bluetoothbootstrap' or \
            signature['entitlements'] != {'org.darwin.bluetooth.bootstrap':True}:
        raise ValueError('invalid Bluetooth backend signing policy')
    files={'root/usr/libexec/bluetoothbootstrap':binary.read_bytes(),
           'root/System/Library/LaunchDaemons/com.apple.bluetoothd.plist':daemon,
           'root/System/Library/LaunchDaemons/com.apple.BlueTool.plist':plistlib.dumps(blue,sort_keys=False)}
    # All native inputs, build provenance and signing policy passed before publication.
    output.mkdir(parents=True,exist_ok=True)
    for name,data in files.items():
        path=output/name;path.parent.mkdir(parents=True,exist_ok=True)
        pending=path.with_name(path.name+'.pending');pending.write_bytes(data);pending.replace(path)
        path.chmod(0o755 if name.endswith('/bluetoothbootstrap') else 0o644)
    trustcache,executables=development_module([output/'root/usr/libexec/bluetoothbootstrap'])
    files['bluetooth.trustcache']=trustcache
    (output/'bluetooth.trustcache').write_bytes(trustcache)
    manifest={'version':1,'status':'staged-offline','hardware_tested':False,'installed':False,
        'native_bluetoothd_modified':False,'native_bluetool_modified':False,
        'native_sources':{p:r['sha256'] for p,r in model.items()},
        'transport':'H4:POSIX:com.apple.uart.bluetooth,3200000','device_tree_transport_encoding':1,
        'service':'com.apple.BlueTool','scripts':['boot','init','deepsleep'],
        'backend_signature':signature,'build_inputs':report['inputs'],
        'development_trustcache':{**module_info(trustcache),'executables':executables},
        'files':{n:{'bytes':len(b),'sha256':hashlib.sha256(b).hexdigest()} for n,b in files.items()},
        'requirements':['validated firmware/board policy via prepare-bluetooth',
                        'explicit offline rootfs overlay and development trustcache integration'],
        'native_chipset_policy_ported':False,
        'remaining_native_policy':'bluetoothd LEHardwareModel factory and VSC dispatch contain no WCN3990 profile',
        'native_stack_runtime_verified':False}
    pending=output/'manifest.pending';pending.write_text(json.dumps(manifest,indent=2)+'\n');pending.replace(output/'manifest.json')
    return manifest


def verify_bluetooth_stack(directory):
    from project.build import native as _component_native
    from project.firmware.macho import verify_macho
    from project.firmware.trustcache import development_module, module_info
    directory=Path(directory)
    manifest=json.loads((directory/'manifest.json').read_text())
    expected={'root/usr/libexec/bluetoothbootstrap','bluetooth.trustcache',
              'root/System/Library/LaunchDaemons/com.apple.bluetoothd.plist',
              'root/System/Library/LaunchDaemons/com.apple.BlueTool.plist'}
    if manifest.get('version') != 1 or set(manifest.get('files',{})) != expected or \
            manifest.get('status') != 'staged-offline' or any(manifest.get(key) is not False for key in
                ('hardware_tested','installed','native_bluetoothd_modified','native_bluetool_modified',
                 'native_chipset_policy_ported','native_stack_runtime_verified')):
        raise ValueError('invalid offline Bluetooth stack manifest')
    for name in expected:
        path=directory/name;record=manifest['files'][name]
        if path.is_symlink() or not path.is_file() or record['bytes'] > 0x1000000 or \
                path.stat().st_size != record['bytes'] or _component_native.digest(path) != record['sha256']:
            raise ValueError('modified Bluetooth stack file '+name)
    model=json.loads((build_state.FIRMWARE_DIR / 'models/bluetooth.json').read_text())['user_space']['files']
    if manifest['native_sources'] != {p:r['sha256'] for p,r in model.items()}:
        raise ValueError('unsupported native Bluetooth stack revision')
    daemon=plistlib.loads((directory/'root/System/Library/LaunchDaemons/com.apple.bluetoothd.plist').read_bytes())
    native=model['/System/Library/LaunchDaemons/com.apple.bluetoothd.plist']['plist']
    import copy
    wanted=copy.deepcopy(native)
    wanted.setdefault('EnvironmentVariables',{})['BT_HCI_TRANSPORT']='H4:POSIX:com.apple.uart.bluetooth,3200000'
    blue=plistlib.loads((directory/'root/System/Library/LaunchDaemons/com.apple.BlueTool.plist').read_bytes())
    wanted_blue=copy.deepcopy(model['/System/Library/LaunchDaemons/com.apple.BlueTool.plist']['plist'])
    wanted_blue['ProgramArguments']=['/usr/libexec/bluetoothbootstrap','-R']
    if daemon != wanted or blue != wanted_blue:raise ValueError('Bluetooth launchd IPC/configuration changed')
    binary=(directory/'root/usr/libexec/bluetoothbootstrap').read_bytes()
    signature=verify_macho(binary)
    if signature != manifest['backend_signature'] or signature['identifier'] != 'org.darwin.development.bluetoothbootstrap' or \
            signature['entitlements'] != {'org.darwin.bluetooth.bootstrap':True}:
        raise ValueError('Bluetooth backend signature/policy mismatch')
    for path,sha in manifest['build_inputs'].items():
        if Path(path).is_absolute() or '..' in Path(path).parts or _component_native.digest(build_state.ROOT/path) != sha:
            raise ValueError('stale Bluetooth stack source '+path)
    module=(directory/'bluetooth.trustcache').read_bytes()
    # Regeneration proves the exact cdhash set and catches a stale/unrelated cache.
    expected_module,_=development_module([directory/'root/usr/libexec/bluetoothbootstrap'])
    if module != expected_module or module_info(module) != {k:v for k,v in manifest['development_trustcache'].items() if k != 'executables'}:
        raise ValueError('Bluetooth development trustcache mismatch')
    return {'status':'passed','files':len(expected),'signature':signature,
            'native_bluetoothd_modified':False,'native_chipset_policy_ported':False,
            'native_stack_runtime_verified':False,'hardware_tested':False}


def prepare_bluetooth_firmware(archive, profile_path, output, bluetoothd_plist=None):
    """Prepare reviewed assets for the bridge; no downloads and no device access."""
    from project.build import native as _component_native
    archive, profile_path, output = Path(archive), Path(profile_path), Path(output)
    profile = json.loads(profile_path.read_text())
    if _component_native.digest(archive) != profile['archive_sha256']:
        raise ValueError('Bluetooth archive hash mismatch')
    with zipfile.ZipFile(archive) as z:
        blobs = {}
        for key, limit in [('patch', 0x100000), ('nvm', 0x10000)]:
            name = profile[key]['file']
            if not isinstance(name,str) or name.startswith('/') or '..' in Path(name).parts or z.namelist().count(name) != 1:
                raise ValueError('invalid or duplicate Bluetooth archive member')
            if z.getinfo(name).file_size > limit:
                raise ValueError('Bluetooth archive member exceeds limit')
            blobs[key] = z.read(name)
            if hashlib.sha256(blobs[key]).hexdigest()!=profile[key]['sha256']:
                raise ValueError('Bluetooth source asset hash mismatch')
    source_nvm_hash=profile['nvm']['sha256']
    blobs['nvm'],changes=prepare_bluetooth_transport_nvm(blobs['nvm'])
    # Record the original immutable asset and every transport-only byte edit.
    # The bridge verifies the prepared hash, never the original hash by mistake.
    profile['nvm']['source_sha256']=source_nvm_hash
    profile['nvm']['sha256']=hashlib.sha256(blobs['nvm']).hexdigest()
    configuration = bluetooth_bundle_configuration(profile,blobs['patch'],blobs['nvm'])
    daemon=prepare_bluetooth_daemon(bluetoothd_plist) if bluetoothd_plist else None
    # Publish only after the complete policy and both payloads passed validation.
    output.mkdir(parents=True,exist_ok=True)
    files = {'patch.tlv':blobs['patch'],'nvm.bin':blobs['nvm'],'configuration.bin':configuration,
             'profile.json':(json.dumps(profile,indent=2)+'\n').encode()}
    if daemon is not None:files['com.apple.bluetoothd.plist']=daemon
    for name,data in files.items():
        pending=output/(name+'.pending');pending.write_bytes(data);pending.replace(output/name)
    manifest={'version':1,'archive_sha256':_component_native.digest(archive),'nvm_transport_changes':changes,
              'files':{n:{'sha256':hashlib.sha256(b).hexdigest(),'bytes':len(b)} for n,b in files.items()}}
    pending=output/'manifest.pending';pending.write_text(json.dumps(manifest,indent=2)+'\n');pending.replace(output/'manifest.json')
    return manifest


def configure_bluetooth_firmware(tree, directory):
    """Add one complete Bluetooth policy to an offline FDT tree atomically."""
    from project.build import native as _component_native
    import copy
    directory=Path(directory)
    manifest=json.loads((directory/'manifest.json').read_text())
    expected={'patch.tlv','nvm.bin','configuration.bin','profile.json'}
    if 'com.apple.bluetoothd.plist' in manifest.get('files',{}):expected.add('com.apple.bluetoothd.plist')
    if manifest.get('version') != 1 or set(manifest['files']) != expected:
        raise ValueError('invalid prepared Bluetooth bundle')
    files={}
    for name in expected:
        path=directory/name;record=manifest['files'][name]
        if path.stat().st_size != record['bytes'] or record['bytes'] > 0x100000 or _component_native.digest(path) != record['sha256']:
            raise ValueError('modified prepared Bluetooth bundle')
        files[name]=path.read_bytes()
    profile=json.loads(files['profile.json'])
    if profile['archive_sha256'] != manifest['archive_sha256'] or bluetooth_bundle_configuration(profile,files['patch.tlv'],files['nvm.bin']) != files['configuration.bin']:
        raise ValueError('Bluetooth configuration/profile mismatch')
    draft=copy.deepcopy(tree)
    def walk(node):
        yield node
        for child in node['children']:yield from walk(child)
    nodes=list(walk(draft))
    def one(compatible):
        found=[n for n in nodes if compatible.encode() in n['props'].get('compatible',b'').split(b'\0')]
        if len(found)!=1:raise ValueError('ambiguous Bluetooth resource provider '+compatible)
        return found[0]['props']
    bt=one('qcom,wcn3990-bt');gcc=one('qcom,gcc-msm8998');gpio=one('qcom,msm8998-pinctrl');rpm=one('qcom,glink-rpm')
    def merge(props,key,stride,additions):
        raw=props.get(key,b'')
        if len(raw)%(4*stride):raise ValueError('malformed Bluetooth resource property '+key)
        records={}
        for row in struct.iter_unpack('>'+str(stride)+'I',raw):
            if row[0] in records:raise ValueError('duplicate existing Bluetooth resource')
            records[row[0]]=row[1:]
        for row in additions:
            if row[0] in records and records[row[0]] != tuple(row[1:]):
                raise ValueError('conflicting Bluetooth resource policy '+key)
            records[row[0]]=tuple(row[1:])
        props[key]=b''.join(struct.pack('>'+str(stride)+'I',k,*v) for k,v in sorted(records.items()))
    if gcc.get('qcom,gcc-handoff-version',struct.pack('>I',1)) != struct.pack('>I',1):
        raise ValueError('unsupported GCC ownership version')
    gcc['qcom,gcc-handoff-version']=struct.pack('>I',1)
    merge(gcc,'qcom,owned-clocks',2,[(37,0),(53,0)])
    merge(gcc,'qcom,owned-roots',2,[(14,51200000)])
    merge(gpio,'qcom,owned-gpios',1,[(p,) for p in range(45,49)])
    merge(gpio,'qcom,owned-pin-functions',2,[(p,4) for p in range(45,49)])
    merge(rpm,'qcom,regulator-limits',4,[(r['id'],r['minimum'],r['maximum'],r['boot_floor']) for r in profile['rails']])
    bt['qcom,bluetooth-config']=files['configuration.bin'] # versioned opaque little-endian ABI
    bt['bluetooth-patch']=files['patch.tlv'];bt['bluetooth-nvm']=files['nvm.bin'];bt['status']=b'okay\0'
    address=bluetooth_local_address(profile,files['nvm.bin'])
    if bt.get('local-mac-address',address)!=address:
        raise ValueError('conflicting Bluetooth local address')
    bt['local-mac-address']=address
    # bluetoothd reads this four-byte DeviceTree property independently of
    # BT_HCI_TRANSPORT. apple_dt converts the FDT cell to native little endian.
    if bt.get('transport-encoding',struct.pack('>I',1)) != struct.pack('>I',1):
        raise ValueError('conflicting native Bluetooth transport encoding')
    bt['transport-encoding']=struct.pack('>I',1)
    tree.clear();tree.update(draft)
    return {'manifest_sha256':_component_native.digest(directory/'manifest.json'),'controller':profile['controller'],
            'native_launchd_prepared':'com.apple.bluetoothd.plist' in files,
            'native_launchd_installed':False,'hardware_tested':False}


def select_wifi_board(data,identity):
    """Select one exact board-2 entry; reject ambiguity and malformed tails."""
    magic=b'QCA-ATH10K-BOARD\0'
    wanted=identity.encode('ascii')
    if not wanted or len(wanted)>200 or not data.startswith(magic) or len(data)>16*1024*1024:
        raise ValueError('invalid board-2 container/identity')
    def entries(value,start):
        while start<len(value):
            if len(value)-start<8:raise ValueError('truncated board-2 entry')
            kind,count=struct.unpack_from('<II',value,start);start+=8
            padded=(count+3)&~3
            if padded>len(value)-start:raise ValueError('board-2 entry exceeds container')
            yield kind,value[start:start+count]
            start+=padded
    selected=None
    for kind,value in entries(data,(len(magic)+3)&~3):
        if kind!=0:continue
        name=None;board=None
        for tag,record in entries(value,0):
            if tag==0:
                if name is not None or board is not None:raise ValueError('duplicate/out-of-order board name')
                name=record
            elif tag==1:
                if name is None or board is not None or not record or len(record)>1024*1024:
                    raise ValueError('invalid board data record')
                board=record
        if name is None or board is None:raise ValueError('incomplete board entry')
        if name==wanted:
            if selected is not None:raise ValueError('ambiguous board identity')
            selected=board
    if selected is None:raise ValueError('requested board identity is absent')
    return selected


def prepare_wifi_firmware(archive,profile_path,output):
    """Stage pinned BDF input only; modem boot and MSA reservation are external."""
    from project.build import native as _component_native
    archive,profile_path,output=Path(archive),Path(profile_path),Path(output)
    profile=json.loads(profile_path.read_text())
    if not isinstance(profile,dict) or profile.get('schema')!=1 or profile.get('controller')!='WCN3990' or _component_native.digest(archive)!=profile.get('archive_sha256'):
        raise ValueError('unsupported controller or firmware archive hash')
    for field in ('chip_id','board_id'):
        if type(profile.get(field)) is not int or not 0<=profile[field]<=0xffffffff:
            raise ValueError('explicit QMI '+field+' is required')
    board=profile.get('board')
    if not isinstance(board,dict) or board.get('format') not in ('raw','board-2') or not isinstance(board.get('member'),str):
        raise ValueError('unsupported board asset format')
    with zipfile.ZipFile(archive) as source:
        matches=[i for i in source.infolist() if i.filename==board['member']]
        if len(matches)!=1 or not 0<matches[0].file_size<=16*1024*1024:
            raise ValueError('missing/ambiguous/oversized board asset')
        data=source.read(matches[0])
    if hashlib.sha256(data).hexdigest()!=board.get('sha256'):raise ValueError('board asset hash mismatch')
    if board['format']=='board-2':
        identity=board.get('identity')
        prefix=f"bus=snoc,qmi-board-id={profile['board_id']:x},qmi-chip-id={profile['chip_id']:x}"
        if not isinstance(identity,str) or (identity!=prefix and not identity.startswith(prefix+',variant=')):
            raise ValueError('board identity contradicts pinned QMI IDs')
        data=select_wifi_board(data,identity)
    if not 0<len(data)<=1024*1024:raise ValueError('invalid raw board data size')
    configuration=struct.pack('<6I',1,24,profile['chip_id'],profile['board_id'],len(data),0)
    files={'board.bin':data,'configuration.bin':configuration,
           'profile.json':(json.dumps(profile,indent=2)+'\n').encode()}
    manifest={'schema':1,'status':'board-assets-validated-offline','controller':'WCN3990',
              'chip_id':profile['chip_id'],'board_id':profile['board_id'],
              'archive_sha256':profile['archive_sha256'],'hardware_tested':False,'installed':False,
              'remote_firmware_included':False,'msa_reserved':False,
              'files':{name:{'sha256':hashlib.sha256(value).hexdigest(),'bytes':len(value)} for name,value in files.items()}}
    output.mkdir(parents=True,exist_ok=True)
    for name,value in files.items():
        pending=output/(name+'.pending');pending.write_bytes(value);pending.replace(output/name)
    pending=output/'manifest.pending';pending.write_text(json.dumps(manifest,indent=2)+'\n');pending.replace(output/'manifest.json')
    return manifest


def prepare_wifi_driverkit():
    """Recover only local DriverKit reference libraries; never mount/install/run."""
    from project.build import drivers as _component_drivers, native as _component_native
    import tempfile
    contract=json.loads((build_state.ABI_ROOT / 'driverkit-abi.json').read_text())
    reference=build_state.BUILD/'obj/wifi/reference'
    reference.mkdir(parents=True,exist_ok=True)
    headers=build_state.TOOLS/'driverkit/include'
    for name,record in json.loads((headers/'sources.json').read_text()).items():
        if _component_native.digest(headers/name)!=record['sha256']:
            raise ValueError('modified public DriverKit header: '+name)
    cache=build_state.ROOT/contract['cache']
    caches={cache:contract['cache_sha256'],Path(str(cache)+'.symbols'):contract['cache_symbols_sha256']}
    missing=[]
    for path,sha in caches.items():
        if path.exists():
            if _component_native.digest(path)!=sha:raise ValueError('DriverKit reference cache changed: '+str(path))
        else:missing.append(path)
    if missing:
        source=build_state.ROOT / 'tools/src/go-apfs'
        reader=contract['sealed_reader']
        patch=build_state.ROOT/reader['patch']
        if _component_native.digest(patch)!=reader['patch_sha256']:raise ValueError('sealed reader patch changed')
        for name,sha in reader['source_files'].items():
            if _component_native.digest(source/name)!=sha:raise ValueError('sealed reader source changed: '+name)
        image=build_state.ROOT/contract['source_cryptex']
        if not image.is_file():raise ValueError('prepare-wifi requires the local system cryptex: '+str(image))
        # A disposable source checkout keeps APFS writer behavior unchanged.
        # Output hashes below authenticate extraction against the pinned cache;
        # this narrow reader does not claim to authenticate the APFS seal.
        with tempfile.TemporaryDirectory(prefix='.reader-',dir=build_state.BUILD/'obj/wifi') as temp:
            directory=Path(temp)
            shutil.copytree(source,directory/'source',ignore=shutil.ignore_patterns('.git','vendor'))
            _component_native.run([_component_native.host('patch'),'-p1','--batch','-i',patch],cwd=directory/'source',log=reference/'sealed-reader.log')
            executable=directory/'apfs'
            _component_native.run([build_state.PREFIX/'libexec/go/bin/go','build','-o',executable,'./cmd/apfs'],cwd=directory/'source',log=reference/'sealed-reader.log')
            cache.parent.mkdir(parents=True,exist_ok=True)
            for path in missing:
                _component_native.run([executable,'extract',image,'/System/DriverKit/System/Library/dyld/'+path.name,
                     '-C',cache.parent],log=reference/'sealed-reader.log')
                if _component_native.digest(path)!=caches[path]:
                    path.unlink()
                    raise ValueError('extracted DriverKit cache hash mismatch')
    ipsw=build_state.BIN/'ipsw'
    if _component_native.digest(ipsw)!=contract['extractor_sha256']:raise ValueError('DriverKit extraction tool changed')
    for name,record in contract['frameworks'].items():
        path=build_state.ROOT/record['path']
        if path.exists() and _component_native.digest(path)!=record['sha256']:
            raise ValueError('DriverKit framework reference changed: '+name)
        if not path.exists():
            _component_native.run([ipsw,'dyld','extract',cache,record['install_name'],'--slide','--output',path.parent],
                log=reference/'framework-extract.log')
            if _component_native.digest(path)!=record['sha256']:raise ValueError('DriverKit framework extraction differs: '+name)
    for symbol,binding in contract['function_bindings'].items():
        path=build_state.ROOT/contract['frameworks'][binding['framework']]['path']
        expected=bytes.fromhex(binding['bytes'])
        if _component_drivers.provider_function_bytes(path,int(binding['address'],0),len(expected))!=expected:
            raise ValueError('native WLAN function binding changed: '+symbol)
    binding=contract['public_vtable_binding']
    path=build_state.ROOT/contract['frameworks'][binding['framework']]['path']
    expected=bytes.fromhex(binding['bytes'])
    if _component_drivers.provider_function_bytes(path,int(binding['address'],0),len(expected),executable=False)!=expected:
        raise ValueError('DriverKit public IOService virtual ABI changed')
    for name,value in contract['public_rpc_ids'].items():
        if struct.pack('<Q',int(value,0)) not in path.read_bytes():raise ValueError('DriverKit lifecycle RPC ID changed: '+name)
    return {'status':'local-reference-verified','platform':'DriverKit','version':'24.6',
            'frameworks':list(contract['frameworks']),'installed':False,'hardware_tested':False}


def driverkit_macho(path):
    """Validate a genuine DriverKit executable and the public class-load format."""
    data=path.read_bytes()
    if len(data)<32 or struct.unpack_from('<4I',data)!=(0xfeedfacf,0x100000c,0,2):
        raise ValueError('expected thin arm64 DriverKit MH_EXECUTE')
    count,command_bytes,flags=struct.unpack_from('<3I',data,16)
    end=32+command_bytes
    if end>len(data) or not flags&0x200000 or not flags&1:raise ValueError('DriverKit executable is not PIE/NoUndefs')
    pos=32;platform=None;libraries=[];segments=[];classes=[];fixups=None;regions=[];signature=None
    for _ in range(count):
        if pos+8>end:raise ValueError('truncated DriverKit command')
        command,size=struct.unpack_from('<2I',data,pos)
        if size<8 or pos+size>end:raise ValueError('invalid DriverKit command')
        if command in (5,0x80000028):raise ValueError('DriverKit must use class loading without a main/thread entry')
        if command==0x32:
            if size<24 or platform is not None:raise ValueError('invalid/duplicate DriverKit platform command')
            platform=struct.unpack_from('<3I',data,pos+8)
        if command==0x80000034:
            if size<16 or fixups is not None:raise ValueError('invalid/duplicate chained fixup command')
            fixups=struct.unpack_from('<2I',data,pos+8)
        if command==0x1d:
            if size<16 or signature is not None:raise ValueError('invalid/duplicate signature command')
            signature=struct.unpack_from('<2I',data,pos+8)
        if command==0xc:
            if size<24:raise ValueError('short DriverKit dependency command')
            offset,=struct.unpack_from('<I',data,pos+8)
            if offset>=size:raise ValueError('invalid DriverKit dependency')
            finish=data.find(b'\0',pos+offset,pos+size)
            if finish<0:raise ValueError('unterminated DriverKit dependency')
            libraries.append(data[pos+offset:finish].decode())
        if command==0x19:
            if size<72:raise ValueError('short DriverKit segment')
            vm,vm_size,offset,file_size=struct.unpack_from('<4Q',data,pos+24)
            protection,=struct.unpack_from('<I',data,pos+60)
            sections,=struct.unpack_from('<I',data,pos+64)
            if offset+file_size>len(data) or 72+80*sections>size:raise ValueError('invalid DriverKit segment range')
            segments.append((vm,offset,file_size))
            regions.append((vm,vm_size,protection))
            for i in range(sections):
                section=pos+72+i*80
                name=data[section:section+16].rstrip(b'\0')
                if name==b'__osclassinfo':
                    address,length,offset=struct.unpack_from('<QQI',data,section+32)
                    if length%8 or offset+length>len(data):raise ValueError('invalid DriverKit class section')
                    classes.extend((address+n,struct.unpack_from('<Q',data,offset+n)[0]) for n in range(0,length,8))
        pos+=size
    if pos!=end or platform!=(10,0x180600,0x180600) or len(classes)!=1:
        raise ValueError('DriverKit platform or class registration mismatch')
    def vm_bytes(address,length):
        for vm,offset,span in segments:
            if vm<=address and address+length<=vm+span:return data[offset+address-vm:offset+address-vm+length]
        raise ValueError('DriverKit class metadata outside file')
    formats={}
    base=min(vm for vm,offset,span in segments if span and offset==0)
    if fixups:
        offset,length=fixups
        if length<28 or offset+length>len(data):raise ValueError('invalid DriverKit chained fixups')
        starts=offset+struct.unpack_from('<I',data,offset+4)[0]
        if starts+4>offset+length:raise ValueError('invalid DriverKit fixup starts')
        seg_count,=struct.unpack_from('<I',data,starts)
        if seg_count>len(segments) or starts+4+4*seg_count>offset+length:raise ValueError('invalid fixup segment count')
        for i in range(seg_count):
            relative,=struct.unpack_from('<I',data,starts+4+4*i)
            if not relative:continue
            if starts+relative+8>offset+length:raise ValueError('truncated fixup segment')
            formats[i]=struct.unpack_from('<H',data,starts+relative+6)[0]
    def pointer(raw,storage):
        for i,(vm,offset,span) in enumerate(segments):
            if vm<=storage<vm+span:
                kind=formats.get(i)
                if kind is None:return raw
                if kind not in (2,6) or raw>>63:raise ValueError('unsupported class metadata fixup')
                address=(raw&((1<<36)-1))|(((raw>>36)&255)<<56)
                return address+base if kind==6 else address
        raise ValueError('class pointer storage outside file')
    address=pointer(classes[0][1],classes[0][0])
    load=vm_bytes(address,160)
    description,meta,version,instance_size=struct.unpack_from('<QQII',load)
    description=pointer(description,address);meta=pointer(meta,address+8)
    constructor=pointer(struct.unpack_from('<Q',load,88)[0],address+88)
    if not any(vm<=constructor<vm+span and prot&4 for vm,span,prot in regions):
        raise ValueError('DriverKit class constructor is not executable')
    if not any(vm<=meta and meta+8<=vm+span and prot&2 for vm,span,prot in regions):
        raise ValueError('DriverKit metaclass pointer is not writable')
    desc=vm_bytes(description,196)
    if version!=1 or not meta or instance_size<48 or desc[4:100].rstrip(b'\0')!=b'QcomWLANDriver' or desc[100:196].rstrip(b'\0')!=b'IOService':
        raise ValueError('invalid DriverKit OSClassLoadInformation')
    from project.firmware.macho import signature_blobs
    if signature is None:raise ValueError('DriverKit development signature missing')
    offset,length=signature
    if offset+length!=len(data):raise ValueError('invalid DriverKit signature extent')
    blobs,_=signature_blobs(data[offset:])
    if set(blobs)!={0}:raise ValueError('unexpected DriverKit signing blobs/entitlements')
    directory=blobs[0]
    if len(directory)<44:raise ValueError('short DriverKit CodeDirectory')
    magic,size,version,flags,hashes,ident,special,slots,limit=struct.unpack_from('>9I',directory)
    hash_size,hash_type,code_platform,page_shift=struct.unpack_from('4B',directory,36)
    if magic!=0xfade0c02 or size!=len(directory) or not flags&2 or special or limit!=offset or \
            (hash_size,hash_type,code_platform,page_shift)!=(32,2,0,12) or slots!=(limit+4095)//4096 or \
            ident<44 or ident>=hashes or hashes+32*slots>len(directory):
        raise ValueError('invalid DriverKit ad-hoc signing policy')
    finish=directory.find(b'\0',ident,hashes)
    if finish<0 or directory[ident:finish]!=b'org.darwin.driver.QcomWLANDriver':
        raise ValueError('DriverKit signature identifier differs from bundle')
    for index in range(slots):
        digest_bytes=hashlib.sha256(data[index*4096:min((index+1)*4096,limit)]).digest()
        if directory[hashes+32*index:hashes+32*(index+1)]!=digest_bytes:
            raise ValueError('DriverKit signed code page changed')
    return {'architecture':'arm64','file_type':'MH_EXECUTE','platform':'DriverKit','minimum_version':'24.6',
            'sdk_version':'24.6','pie':True,'entry':'__osclassinfo','class':'QcomWLANDriver',
            'superclass':'IOService','instance_size':instance_size,'dependencies':libraries,
            'signature':{'identifier':'org.darwin.driver.QcomWLANDriver','ad_hoc':True,'code_pages_verified':slots,
                         'entitlements':False,'cms':False}}


def wifi_driverkit_inputs():
    from project.build import native as _component_native
    source=build_state.ROOT/'drivers/wifi/QcomWLANDriver'
    headers=build_state.TOOLS/'driverkit/include'
    contract=json.loads((build_state.ABI_ROOT / 'driverkit-abi.json').read_text())
    linker=build_state.PREFIX/'libexec/native/arm-apple-darwin11-ld'
    paths=[build_state.SELF,build_state.ROOT/'tools/project/firmware/macho.py',build_state.ABI_ROOT / 'driverkit-abi.json',headers/'sources.json',build_state.PREFIX/'compiler.json',
           Path(_component_native.compiler()),linker if linker.exists() else build_state.BIN/'arm-apple-darwin11-ld',
           *source.glob('*'),*(headers/name for name in json.loads((headers/'sources.json').read_text())),
           build_state.DRIVERS_ROOT/'include/WLANProtocol.h',build_state.DRIVERS_ROOT/'include/WLANDriverPolicy.h',
           *(build_state.ROOT/contract['frameworks'][name]['path'] for name in ('DriverKit','NetworkingDriverKit','libc++.dylib'))]
    # Include the actual Darwin C/framework headers found by Clang, including
    # system headers. -MD is intentional; -MMD would omit these inputs.
    for name in ('QcomWLANDriver.cpp','QcomWLANDriverRPC.cpp','NativeWLAN.cpp'):
        dependency=build_state.BUILD/'obj/wifi/obj'/(name+'.d')
        if not dependency.exists():raise ValueError('missing DriverKit dependency record; rebuild wifi-driverkit')
        value=dependency.read_text().replace('\\\n',' ')
        if ':' not in value:raise ValueError('malformed DriverKit dependency record')
        for item in shlex.split(value.split(':',1)[1]):
            path=Path(item).resolve()
            if not path.is_relative_to(build_state.ROOT):raise ValueError('DriverKit used a header outside the project: '+str(path))
            paths.append(path)
    return {str(path.relative_to(build_state.ROOT)):_component_native.digest(path) for path in paths if path.is_file()}


def build_wifi_driverkit():
    from project.build import native as _component_native
    import tempfile
    _component_native.aliases();prepare_wifi_driverkit()
    source=build_state.ROOT/'drivers/wifi/QcomWLANDriver'
    contract=json.loads((build_state.ABI_ROOT / 'driverkit-abi.json').read_text())
    sdk=build_state.ROOT / 'tools/sdk/iPhoneOS18.6.sdk'
    headers=build_state.TOOLS/'driverkit/include'
    objdir=build_state.BUILD/'obj/wifi/obj';objdir.mkdir(parents=True,exist_ok=True)
    includes=['-isystem',build_state.PREFIX/'lib/clang'/json.loads((build_state.PREFIX/'compiler.json').read_text())['resource_version']/'include',
              '-isystem',sdk/'usr/include','-isystem',headers,'-F',sdk/'System/Library/Frameworks',
              '-I',build_state.DRIVERS_ROOT/'include']
    objects=[]
    for name in ('QcomWLANDriver.cpp','QcomWLANDriverRPC.cpp','NativeWLAN.cpp'):
        obj=objdir/(name+'.o');log=objdir/(name+'.log');log.unlink(missing_ok=True)
        _component_native.run([build_state.BIN/'clang-real','--driver-mode=g++','--target=arm64-apple-driverkit24.6',*_component_native.compiler_resource(),
             '-nostdinc','-nostdinc++',*includes,'-std=c++17','-fno-exceptions','-fno-rtti','-fblocks',
             '-Wall','-Wextra','-Werror','-O2','-fvisibility=hidden','-Xclang','-fdump-vtable-layouts',
             '-MD','-MF',objdir/(name+'.d'),'-c',source/name,'-o',obj],log=log)
        objects.append(obj)
    libraries=[build_state.ROOT/contract['frameworks'][name]['path'] for name in ('DriverKit','NetworkingDriverKit','libc++.dylib')]
    output=build_state.BUILD/'output/QcomWLANDriver.dext';output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.driverkit-',dir=build_state.BUILD/'obj/wifi') as directory:
        pending=Path(directory)/'QcomWLANDriver'
        _component_native.run([build_state.BIN/'arm-apple-darwin11-ld','-arch','arm64','-platform_version','driverkit','24.6','24.6',
             '-undefined','error','-adhoc_codesign','-final_output','org.darwin.driver.QcomWLANDriver',
             '-o',pending,*objects,*libraries],log=objdir/'link.log')
        metadata=driverkit_macho(pending)
        expected={contract['frameworks'][name]['install_name'] for name in ('DriverKit','NetworkingDriverKit','libc++.dylib')}
        if set(metadata['dependencies'])!=expected:raise ValueError('unexpected DriverKit dependencies')
        info=plistlib.loads((source/'Info.plist').read_bytes())
        if info['CFBundlePackageType']!='DEXT' or info['CFBundleSupportedPlatforms']!=['DriverKit']:
            raise ValueError('invalid DEXT bundle metadata')
        report={'status':'cross-linked-offline','hardware_tested':False,'installed':False,
                'apple_platform_identity_claimed':False,'apple_private_entitlements':False,
                'native_registration_bridge_compiled':True,'native_wlan_control_ready':False,
                'iig':'explicit public OSClassLoadInformation registration; inherited Apple Start/Stop RPCs only',
                'sha256':_component_native.digest(pending),'metadata':metadata,
                'inputs':wifi_driverkit_inputs()}
        output.mkdir(exist_ok=True)
        shutil.copy2(source/'Info.plist',output/'Info.plist')
        pending.replace(output/'QcomWLANDriver')
        (output/'build.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Cross-linked DriverKit 24.6: '+str(output.relative_to(build_state.ROOT)))


def verify_wifi_driverkit():
    from project.build import native as _component_native
    output=build_state.BUILD/'output/QcomWLANDriver.dext'
    report=json.loads((output/'build.json').read_text())
    if report.get('status')!='cross-linked-offline' or report.get('hardware_tested') is not False or report.get('installed') is not False:
        raise ValueError('invalid offline DriverKit build report')
    if report.get('inputs')!=wifi_driverkit_inputs():raise ValueError('stale/incomplete DriverKit inputs; rebuild wifi-driverkit')
    if report.get('apple_platform_identity_claimed') is not False or report.get('apple_private_entitlements') is not False or \
            report.get('native_registration_bridge_compiled') is not True or report.get('native_wlan_control_ready') is not False:
        raise ValueError('invalid DriverKit readiness/signing claims')
    if _component_native.digest(output/'QcomWLANDriver')!=report['sha256'] or driverkit_macho(output/'QcomWLANDriver')!=report['metadata']:
        raise ValueError('DriverKit executable changed')
    if (output/'Info.plist').read_bytes()!=(build_state.ROOT/'drivers/wifi/QcomWLANDriver/Info.plist').read_bytes():
        raise ValueError('stale DriverKit matching metadata')
    return {'status':'passed','bundle':str(output.relative_to(build_state.ROOT)),'sha256':report['sha256'],
            'platform':'DriverKit 24.6','native_registration_bridge_compiled':True,
            'native_wlan_control_ready':False,'hardware_tested':False,'installed':False}
