"""Offline input/evidence helpers for toolchain.py; never accesses a device."""
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import shutil
import struct
import zipfile


def sha(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def macho_evidence(data):
    if len(data) < 32 or struct.unpack_from('<I', data)[0] != 0xfeedfacf:
        raise ValueError('expected thin 64-bit Mach-O')
    _, cpu, subtype, kind, count, command_bytes, _, _ = struct.unpack_from('<8I', data)
    end, pos = 32 + command_bytes, 32
    if end > len(data) or count > command_bytes // 8:
        raise ValueError('invalid Mach-O commands')
    dylibs, symbols = [], None
    for _ in range(count):
        if pos + 8 > end:
            raise ValueError('short command')
        command, size = struct.unpack_from('<II', data, pos)
        if size < 8 or pos + size > end:
            raise ValueError('invalid command size')
        if command in (12, 24, 0x80000018, 0x8000001f):
            if size < 24:
                raise ValueError('short dylib command')
            offset, = struct.unpack_from('<I', data, pos + 8)
            if not 24 <= offset < size:
                raise ValueError('dylib name bounds')
            name = data[pos + offset:pos + size].split(b'\0', 1)
            if len(name) != 2:
                raise ValueError('unterminated dylib name')
            dylibs.append(name[0].decode())
        elif command == 2:
            if size != 24 or symbols:
                raise ValueError('invalid symbol table command')
            symbols = struct.unpack_from('<4I', data, pos + 8)
        pos += size
    if pos != end:
        raise ValueError('command count mismatch')
    imports = []
    if symbols:
        offset, count, strings, length = symbols
        if offset + 16 * count > len(data) or strings + length > len(data):
            raise ValueError('symbol table bounds')
        for i in range(count):
            index, flags, _, _, _ = struct.unpack_from('<IBBHQ', data, offset + i * 16)
            if flags & 0xe0 or flags & 0x0e or not flags & 1 or not index:
                continue
            if index >= length:
                raise ValueError('symbol string bounds')
            stop = data.find(b'\0', strings + index, strings + length)
            if stop < 0:
                raise ValueError('unterminated symbol')
            name = data[strings + index:stop].decode()
            if any(s in name for s in ('Baseband', 'Telephony', 'QMI', 'Transport', 'IOService')):
                imports.append(name)
    return {'cpu': hex(cpu), 'subtype': hex(subtype), 'filetype': kind,
            'dependencies': dylibs, 'telephony_imports': imports}


def prepare(root, output=None):
    root = Path(root)
    output = Path(output or root / 'build/baseband/firmware')
    manifest = json.loads((root / 'docs/baseband/firmware-sources.json').read_text())
    source = root / 'firmware/sagit-baseband'
    files = {}
    for name in ('mba.mbn', 'modem.mbn', 'modemr.jsn', 'modemuw.jsn'):
        record, path = manifest['files'][name], source / name
        if path.stat().st_size != record['bytes'] or sha(path) != record['sha256']:
            raise ValueError('baseband input differs from pinned extraction: ' + name)
        if name.endswith('.mbn'):
            with path.open('rb') as handle:
                header = handle.read(52)
            if header[:7] != b'\x7fELF\x01\x01\x01' or struct.unpack_from('<H', header, 18)[0] != 164:
                raise ValueError('expected ELF32 Hexagon firmware: ' + name)
        else:
            json.loads(path.read_bytes())
        files[name] = {'bytes': record['bytes'], 'sha256': record['sha256']}
    output.mkdir(parents=True, exist_ok=True)
    catalog = catalog_report = None
    if (root / 'docs/baseband/stock-firmware-source.json').exists():
        from baseband_handoff import stock_tftp_catalog
        catalog, _, stock = stock_tftp_catalog(root)
        catalog_report = {'bytes': len(catalog), 'sha256': hashlib.sha256(catalog).hexdigest(),
                          'source_archive_sha256': stock['archive_sha256']}
    report_path = output / 'manifest.json'
    report_path.unlink(missing_ok=True)
    for name in files:
        pending = output / (name + '.pending')
        shutil.copyfile(source / name, pending)
        if sha(pending) != files[name]['sha256']:
            pending.unlink()
            raise ValueError('firmware changed during staging')
        pending.replace(output / name)
    if catalog is not None:
        pending = output / 'tftp-files.bin.pending'; pending.write_bytes(catalog)
        pending.replace(output / 'tftp-files.bin')
    report = {'schema': 1, 'status': 'pinned-inputs-staged', 'files': files,
              'source_kind': manifest['source_kind'], 'original_xiaomi_package_verified': False,
              'device_authentication_verified': False, 'memory_reserved': False,
              'hardware_tested': False, 'installed': False}
    if catalog_report is not None: report['tftp_catalog'] = catalog_report
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    return report


def analyze(root, provider=None, output=None):
    root = Path(root)
    output = Path(output or root / 'docs/baseband/ios18-model.json')
    output.parent.mkdir(parents=True, exist_ok=True)
    provider = Path(provider or root / 'build/ios18-kext-analysis/kernelcache.release.ipad7c.macho')
    spec = importlib.util.spec_from_file_location('baseband_kext_analysis',
                                                root / 'scripts/analyze-kernelcache.py')
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    kernel, kexts = reader.inspect(provider)
    selected = [{k: v for k, v in item.items() if k in
                 ('CFBundleIdentifier', 'CFBundleVersion', 'DTSDKName', 'DTPlatformBuild',
                  'IOKitPersonalities', 'OSBundleLibraries')}
                for item in kexts if 'baseband' in item.get('CFBundleIdentifier', '').lower()]
    firmware = []
    for name in ('iPad_10.2_18.7.10_22H374_Restore.ipsw',
                 'iPhone14,6_18.6.2_22G100_Restore.ipsw'):
        archive = root / 'firmware' / name
        with zipfile.ZipFile(archive) as source:
            raw = source.read('BuildManifest.plist')
            manifest = plistlib.loads(raw)
            firmware.append({
                'path': str(archive.relative_to(root)),
                'role': 'primary' if name.startswith('iPad') else 'comparison',
                'manifest_sha256': hashlib.sha256(raw).hexdigest(),
                'ProductVersion': manifest['ProductVersion'],
                'ProductBuildVersion': manifest['ProductBuildVersion'],
                'SupportedProductTypes': manifest['SupportedProductTypes'],
                'baseband_archives': [n for n in source.namelist() if n.endswith('.bbfw')]
            })
    ref = root / 'build/baseband/reference'
    command, plist = ref / 'CommCenter', ref / 'com.apple.CommCenter.plist'
    native = macho_evidence(command.read_bytes())
    native.update(path='/System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter',
                  sha256=sha(command))
    sdk = root / 'tools/sdk/iPhoneOS18.6.sdk'
    stubs = {}
    for name in ('libTelephonyBasebandDynamic', 'libTelephonyIOKit', 'libTelephonyUtilDynamic',
                 'libBasebandCommandDriversQMI', 'libBasebandManager', 'libQMIParserDynamic'):
        path = sdk / 'usr/lib' / (name + '.tbd')
        if path.exists():
            stubs[name] = {'path': str(path.relative_to(root)), 'sha256': sha(path),
                          'kind': 'SDK export declaration; runtime ABI not established'}
    report = {'schema': 1, 'firmware': firmware, 'kernel': kernel, 'kernel_kexts': selected,
              'user_space': {'build': '22H374', 'source_rootfs': 'build/apfs/firmware/094-32804-038.dmg',
                             'CommCenter': native, 'launchd': plistlib.loads(plist.read_bytes()),
                             'launchd_sha256': sha(plist)},
              'sdk': {'version': '18.6', 'stubs': stubs},
              'findings': [
                  'Primary iPad kernel uses AppleBasebandD101 and ICE PCIe transport, not MSM8998.',
                  'CommCenter has a baseband DT gate; that property alone cannot implement its dependencies.',
                  'QMI SDK exports do not prove that this CommCenter selects a Qualcomm command driver.',
                  'MSM8998 needs integrated Q6/PBL/MBA, SMEM/SMP2P and GLINK; GPU PAS13 is inapplicable.',
                  'Apple private control ABI, cellular data plane and telephony integration remain required.'
              ], 'hardware_tested': False, 'installed': False}
    runtime_evidence = {}
    for name in ('ipad-runtime-libraries.json', 'ipad-apple-control.json'):
        path = root / 'docs/baseband' / name
        if path.exists():
            evidence = json.loads(path.read_text())
            if evidence.get('build') != firmware[0]['ProductBuildVersion']:
                raise ValueError('runtime evidence build differs from primary iPad')
            runtime_evidence[name] = {'path': str(path.relative_to(root)), 'sha256': sha(path),
                                      'build': evidence['build']}
    report['runtime_evidence'] = runtime_evidence
    pending = output.with_suffix(output.suffix + '.pending')
    pending.write_text(json.dumps(report, indent=2) + '\n')
    pending.replace(output)
    return {'status': 'offline-evidence-recorded', 'report': str(output),
            'baseband_kexts': len(selected), 'primary': firmware[0]['ProductBuildVersion']}
