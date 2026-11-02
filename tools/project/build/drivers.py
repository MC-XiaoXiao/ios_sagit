"""Kext builds, private ABI checks and verified collections."""
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import struct
import tempfile
from project.build import context as build_state


def driver_configuration():
    path = build_state.DRIVERS_ROOT / 'Build.json'
    config = json.loads(path.read_text()) if path.exists() else {}
    if build_state.FIRMWARE_DIR and build_state.DRIVERS_ROOT == build_state.ROOT / 'drivers':
        adapter = json.loads((build_state.FIRMWARE_DIR / 'drivers.json').read_text())
        probe_contracts = adapter.pop('probe_contracts', {})
        source_manifests = adapter.pop('source_manifests', [])

        def paths(value):
            if isinstance(value, str) and value.startswith(('abi/', 'include/')):
                return os.path.relpath(build_state.FIRMWARE_DIR / value, build_state.DRIVERS_ROOT)
            if isinstance(value, list):
                return [paths(item) for item in value]
            if isinstance(value, dict):
                return {key: paths(item) for key, item in value.items()}
            return value

        def merge(target, additions):
            for key, value in additions.items():
                if isinstance(value, dict) and isinstance(target.get(key), dict):
                    merge(target[key], value)
                else:
                    target[key] = value

        merge(config, paths(adapter))
        config['source_manifests'] += paths(source_manifests)
        for check in config.get('abi_checks', []):
            check.update(paths(probe_contracts.get(check['name'], {})))
        config['include_dirs'].insert(0, os.path.relpath(build_state.FIRMWARE_INCLUDE, build_state.DRIVERS_ROOT))
        config['provider_image'] = str(build_state.PROVIDER_IMAGE)
    definitions = config.get('target_collections', {}).get(build_state.TARGET, config.get('collections', {}))
    resolved = {}
    def collection(name, visiting=()):
        if name in visiting:
            raise RuntimeError('collection inheritance cycle: ' + ' -> '.join((*visiting, name)))
        if name not in definitions:
            raise RuntimeError('unknown inherited collection: ' + str(name))
        if name in resolved:
            return resolved[name]
        value = dict(definitions[name]); parents = value.get('inherits', [])
        if not isinstance(parents, list) or any(not isinstance(p, str) for p in parents):
            raise RuntimeError('invalid collection inheritance: ' + name)
        roots = []
        for parent in parents:
            inherited = collection(parent, (*visiting, name))
            roots += inherited.get('roots', [])
        # Paths and output names belong to the child; dependency roots inherit.
        value['roots'] = list(dict.fromkeys([*roots, *value.get('roots', [])]))
        resolved[name] = value
        return value
    for name in definitions:
        collection(name)
    config['collections'] = resolved
    if build_state.BUILD != build_state.ROOT / 'build':
        for name, value in resolved.items():
            value['output'] = str(build_state.BUILD / 'output' / ('kernelcache.' + build_state.TARGET.lower() + '.' + name))
    return config


def driver_sources():
    result, identifiers, executables = [], set(), set()
    for path in sorted(build_state.DRIVERS_ROOT.rglob('Info.plist')):
        info = plistlib.loads(path.read_bytes())
        # Categories also contain user-space bundles and DriverKit extensions.
        if info.get('CFBundlePackageType') != 'KEXT':
            continue
        identifier, executable, version = (info.get(k) for k in
            ('CFBundleIdentifier', 'CFBundleExecutable', 'CFBundleVersion'))
        for value in (identifier, executable, version):
            if not isinstance(value, str) or not re.fullmatch(r'[A-Za-z0-9_.-]+', value) or value in ('.', '..'):
                raise RuntimeError(f'invalid Kext identity/version: {path}')
        if identifier in identifiers or executable in executables:
            raise RuntimeError(f'duplicate Kext identifier/executable: {path}')
        identifiers.add(identifier)
        executables.add(executable)
        result.append((path.parent, info))
    if not result:
        raise RuntimeError(f'no Kext sources found: {build_state.DRIVERS_ROOT}')
    if build_state.DRIVER_FILTERS:
        matched = {value for source, info in result for value in build_state.DRIVER_FILTERS
                   if value in (source.name, info['CFBundleExecutable'], info['CFBundleIdentifier'])}
        if matched != set(build_state.DRIVER_FILTERS):
            raise RuntimeError(f'unknown driver selection: {sorted(set(build_state.DRIVER_FILTERS) - matched)}')
        result = [(source, info) for source, info in result if any(value in
                  (source.name, info['CFBundleExecutable'], info['CFBundleIdentifier']) for value in build_state.DRIVER_FILTERS)]
    return result


def prepare_driver_sdk(jobs):
    # Once the matching SDK is installed, a standalone Kext build should not
    # replay kernel-source patches (the kernel may be under active editing).
    from project.build import kernel as _component_kernel, native as _component_native
    _component_native.aliases()
    marker = build_state.BUILD / '.workspace-root'
    if (not marker.exists() or marker.read_text().strip() != f'{build_state.ROOT}\n{build_state.SRC}' or
            not (build_state.BUILD / ('.headers-installed-' + build_state.TARGET.lower())).exists() or
            not (build_state.SDK / 'System/Library/Frameworks/Kernel.framework/Headers/IOKit/IOInterruptController.h').exists()):
        _component_kernel.prepare_sdk(jobs)


def driver_compile_flags():
    """Shared SDK, architecture and diagnostics for Kexts and compile checks."""
    framework = build_state.SDK / 'System/Library/Frameworks/Kernel.framework'
    common = ['-isysroot', build_state.SDK, '-nostdinc',
              '-isystem', framework / 'PrivateHeaders', '-isystem', framework / 'Headers',
              '-isystem', build_state.SDK / 'usr/include',
              '-I' + str(build_state.SDK / 'usr/local/include/kernel'), '-I' + str(build_state.SRC / 'EXTERNAL_HEADERS'),
              '-mkernel', '-fno-builtin', '-fno-common', '-mgeneral-regs-only', '-march=armv8-a+crc+crypto',
              '-DKERNEL', '-DKERNEL_PRIVATE', '-DDRIVER_PRIVATE', '-DAPPLE', '-DNeXT',
              '-D__APPLE_CC__=6000', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
              '-Wno-unused-parameter', '-Wno-macro-redefined', '-Wno-c99-designator',
              '-Wno-reorder-init-list', '-Wno-missing-field-initializers',
              '-Wno-missing-designated-field-initializers']
    common += ['-I' + str(build_state.DRIVERS_ROOT / relative)
               for relative in driver_configuration().get('include_dirs', [])]
    return common


def driver_language_flags(cpp):
    return ['-std=gnu++17', '-fapple-kext', '-fno-exceptions', '-fno-rtti',
            '-fc++-static-destructors=none'] if cpp else ['-std=gnu11']


def build_drivers(jobs, sources=None):
    """Build real MH_KEXT_BUNDLEs with XNU headers and Apple's kmod stubs."""
    from project.build import gpu as _component_gpu, native as _component_native
    prepare_driver_sdk(jobs)
    common = driver_compile_flags()
    config = driver_configuration()
    sources = driver_sources() if sources is None else sources
    verify_driver_abi(sources, config)
    gpu_generated = _component_gpu.build_gpu_shaders() if any(info['CFBundleExecutable'] == 'QcomGPU' for _,info in sources) else None
    for source, info in sources:
        name = info['CFBundleExecutable']
        units = sorted(p for p in source.iterdir() if p.suffix in ('.c', '.cpp', '.cc', '.cxx'))
        if not units:
            raise RuntimeError(f'no compilation units: {source}')
        has_cpp = any(p.suffix in ('.cpp', '.cc', '.cxx') for p in units)
        resident = config.get('resident', has_cpp)
        if has_cpp and not resident:
            raise RuntimeError('this compiler configuration supports resident C++ Kexts; set resident=true')
        objdir = build_state.BUILD / 'obj/drivers/obj' / name
        objdir.mkdir(parents=True, exist_ok=True)
        generated = objdir / 'kmod_info.c'
        stop_result = 'KERN_FAILURE' if resident else 'KERN_SUCCESS'
        generated.write_text(
            '#include <mach/mach_types.h>\n'
            'extern kern_return_t _start(kmod_info_t *, void *);\n'
            'extern kern_return_t _stop(kmod_info_t *, void *);\n'
            f'static kern_return_t module_stop(kmod_info_t *info, void *data) {{ return {stop_result}; }}\n'
            f'KMOD_EXPLICIT_DECL({info["CFBundleIdentifier"]}, "{info["CFBundleVersion"]}", _start, _stop)\n'
            '__private_extern__ kmod_start_func_t *_realmain = 0;\n'
            '__private_extern__ kmod_stop_func_t *_antimain = module_stop;\n'
            '__private_extern__ int _kext_apple_cc = __APPLE_CC__;\n')
        units += [generated, build_state.SRC / 'libkern/kmod/c_start.c', build_state.SRC / 'libkern/kmod/c_stop.c']
        objects = []
        for unit in units:
            obj = objdir / (unit.name + '.o')
            cpp = unit.suffix in ('.cpp', '.cc', '.cxx')
            flags = driver_language_flags(cpp)
            layout = config.get('vtable_contracts', {}).get(name)
            layout_log = None
            layout_units = ({layout['unit']} | {v['unit'] for v in layout.get('additional_classes', {}).values()}) if layout else set()
            if cpp and unit.name in layout_units:
                flags += ['-Xclang', '-fdump-vtable-layouts']
                layout_log = objdir / (unit.name + '.vtable.txt')
                layout_log.unlink(missing_ok=True)
            _component_native.run([build_state.BIN / ('clang++-arm64' if cpp else 'clang-arm64'), *common, '-I' + str(source),
                 *(['-I'+str(gpu_generated)] if name == 'QcomGPU' else []),
                 *flags, '-c', unit, '-o', obj], log=layout_log)
            objects.append(obj)
        bundle = build_state.DRIVER_OUTPUT / (name + '.kext') / 'Contents'
        (bundle / 'MacOS').mkdir(parents=True, exist_ok=True)
        # Classic Apple ld64 supports -kext directly. No Mach-O header patching.
        _component_native.run([build_state.BIN / 'arm-apple-darwin11-ld', '-arch', 'arm64', '-kext', '-syslibroot', build_state.SDK,
             '-platform_version', 'ios', '18.6', '18.6', '-undefined', 'dynamic_lookup',
             '-o', bundle / 'MacOS' / name, *objects])
        shutil.copy2(source / 'Info.plist', bundle / 'Info.plist')
        if name == 'QcomGPU':
            (bundle / 'Resources').mkdir(exist_ok=True)
            shutil.copyfile(build_state.TOOLS / 'gpu-compiler/NOTICE.txt', bundle / 'Resources/NOTICE.Mesa.txt')
        if (source / 'NOTICE.ADSP.txt').exists():
            (bundle / 'Resources').mkdir(exist_ok=True)
            shutil.copyfile(source / 'NOTICE.ADSP.txt', bundle / 'Resources/NOTICE.ADSP.txt')
            shutil.copyfile(source / 'LICENSE.ADSP', bundle / 'Resources/LICENSE.ADSP')
        if (source / 'NOTICE.SMMU.txt').exists():
            (bundle / 'Resources').mkdir(exist_ok=True)
            shutil.copyfile(source / 'NOTICE.SMMU.txt', bundle / 'Resources/NOTICE.SMMU.txt')
        if (source / 'NOTICE.RMTFS').exists():
            (bundle / 'Resources').mkdir(exist_ok=True)
            shutil.copyfile(source / 'NOTICE.RMTFS', bundle / 'Resources/NOTICE.RMTFS')
            shutil.copyfile(source / 'LICENSE', bundle / 'Resources/LICENSE')
        if (source / 'NOTICE.TFTP').exists():
            (bundle / 'Resources').mkdir(exist_ok=True)
            shutil.copyfile(source / 'NOTICE.TFTP', bundle / 'Resources/NOTICE.TFTP')
        if name == 'QcomSCM':
            (bundle / 'Resources').mkdir(exist_ok=True)
            shutil.copyfile(source / 'NOTICE.txt', bundle / 'Resources/NOTICE.txt')
    verify_drivers(sources)


def driver_macho(path):
    """Strict thin arm64 Kext metadata reader for offline verification."""
    from project.build import native as _component_native
    data = path.read_bytes()
    if len(data) < 32:
        raise RuntimeError(f'truncated Kext: {path}')
    h = struct.unpack_from('<8I', data)
    if h[:4] != (0xfeedfacf, 0x100000c, 0, 0xb):
        raise RuntimeError(f'expected arm64/ALL MH_KEXT_BUNDLE: {path}')
    pos, end = 32, 32 + h[5]
    sections, symbols, platform = [], None, None
    if end > len(data):
        raise RuntimeError('Kext load commands outside file')
    for _ in range(h[4]):
        if pos + 8 > end:
            raise RuntimeError('short Kext load command')
        cmd, size = struct.unpack_from('<II', data, pos)
        if size < 8 or pos + size > end:
            raise RuntimeError('invalid Kext load command size')
        if cmd == 0x19:
            if size < 72:
                raise RuntimeError('short Kext segment')
            s = struct.unpack_from('<II16s4Q4I', data, pos)
            if s[5] + s[6] > len(data) or 72 + s[9] * 80 > size:
                raise RuntimeError('invalid Kext segment range')
            for i in range(s[9]):
                sec = struct.unpack_from('<16s16sQQIIIIIIII', data, pos + 72 + i * 80)
                if sec[6] and sec[6] + 8 * sec[7] > len(data):
                    raise RuntimeError('Kext relocations outside file')
                sections.append(sec[0].rstrip(b'\0').decode())
        elif cmd == 2:
            symbols = struct.unpack_from('<4I', data, pos + 8)
        elif cmd == 0x32:
            platform = struct.unpack_from('<3I', data, pos + 8)
        elif cmd in (0xc, 0x18, 0x80000018, 0x8000001f, 0x80000034):
            raise RuntimeError('Kext contains userspace dylib dependencies or chained fixups')
        pos += size
    # Apple's classic -kext link omits userspace deployment load commands.
    # If a newer linker emits one, it must agree with our compiler target.
    if pos != end or (platform is not None and platform != (2, 0x120600, 0x120600)) or not symbols:
        raise RuntimeError('Kext platform/load-command/symbol metadata mismatch')
    symoff, count, stroff, strsize = symbols
    if symoff + count * 16 > len(data) or stroff + strsize > len(data):
        raise RuntimeError('Kext symbol table outside file')
    defined, undefined = set(), set()
    for i in range(count):
        string, kind, section, desc, value = struct.unpack_from('<IBBHQ', data, symoff + 16 * i)
        if not string or kind & 0xe0:
            continue
        if string >= strsize:
            raise RuntimeError('invalid Kext symbol string index')
        finish = data.find(b'\0', stroff + string, stroff + strsize)
        if finish < 0:
            raise RuntimeError('unterminated Kext symbol name')
        name = data[stroff + string:finish].decode()
        if kind & 0x0e == 0 and kind & 1:
            undefined.add(name)
        elif kind & 0x0e == 0x0e:
            defined.add(name)
    if '_kmod_info' not in defined:
        raise RuntimeError('Kext lacks kmod_info')
    if any(s.endswith('10gMetaClassE') for s in defined) and '__mod_init_func' not in sections:
        raise RuntimeError('C++ Kext lacks metaclass initializers')
    return {'sha256': _component_native.digest(path), 'bytes': len(data), 'architecture': 'arm64',
            'filetype': 'MH_KEXT_BUNDLE', 'defined': sorted(defined), 'imports': sorted(undefined)}


def driver_kpi_patterns(libraries):
    # com.apple.kpi.bsd uses XNU's BSDKernel export table, which includes the
    # control-socket and mbuf KPI. There is no BSD.exports in this source tree.
    families = {'iokit': 'IOKit', 'libkern': 'Libkern', 'mach': 'Mach',
                'bsd': 'BSDKernel', 'private': 'Private', 'unsupported': 'Unsupported'}
    patterns=[]
    for key,family in families.items():
        if 'com.apple.kpi.'+key not in libraries:
            continue
        for suffix in ('','.arm','.arm64','.arm64.iPhoneOS'):
            exports=build_state.SRC/'config'/(family+suffix+'.exports')
            if exports.exists():
                patterns.extend(line.split(':')[0].strip() for line in exports.read_text().splitlines()
                                if line.startswith('_'))
    return patterns


def verify_drivers(sources=None):
    from project.build import native as _component_native
    for relative in driver_configuration().get('source_manifests', []):
        manifest = build_state.DRIVERS_ROOT / relative
        for name, source in json.loads(manifest.read_text()).items():
            path = manifest.parent / name
            if _component_native.digest(path) != source['sha256']:
                raise RuntimeError(f'driver reference/header checksum mismatch: {path}')
    root = build_state.DRIVER_OUTPUT
    reports = []
    for source, source_info in (driver_sources() if sources is None else sources):
        name = source_info['CFBundleExecutable']
        contents = root / (name + '.kext') / 'Contents'
        info = plistlib.loads((contents / 'Info.plist').read_bytes())
        if info != source_info:
            raise RuntimeError(f'stale driver plist: {name}')
        identifier = info['CFBundleIdentifier']
        if info.get('CFBundlePackageType') != 'KEXT':
            raise RuntimeError(f'invalid Kext identity: {name}')
        report = driver_macho(contents / 'MacOS' / name)
        layout = driver_configuration().get('vtable_contracts', {}).get(name)
        if layout:
            log = build_state.BUILD / 'obj/drivers/obj' / name / (layout['unit'] + '.vtable.txt')
            report['native_abi_evidence'] = [verify_abi_contract(json.loads((build_state.DRIVERS_ROOT / p).read_text())) for p in layout.get('native_abi_contracts', [])]
            report['compiler_vtable'] = verify_compiler_vtable(log.read_text(), layout.get('class', name), layout)
            if layout.get('additional_classes'):
                report['additional_compiler_vtables'] = {}
                for cls, contract in layout['additional_classes'].items():
                    log = build_state.BUILD / 'obj/drivers/obj' / name / (contract['unit'] + '.vtable.txt')
                    report['additional_compiler_vtables'][cls] = verify_compiler_vtable(log.read_text(), cls, contract)
        report.update({'bundle': name + '.kext', 'identifier': identifier, 'libraries': info.get('OSBundleLibraries', {}),
                       'personalities': info.get('IOKitPersonalities', {})})
        reports.append(report)
    nm = subprocess.check_output([build_state.BIN / 'arm-apple-darwin11-nm', '-j', '-U',
                                 build_state.BUILD / 'output' / (build_state.KERNEL_FILE + '.unstripped')], text=True)
    kernel_symbols = set(nm.splitlines())
    by_id = {r['identifier']: r for r in reports}
    needed = {dependency for report in reports for dependency in report['libraries']}
    contracts = [c for p in driver_configuration().get('abi_contracts', [])
                 if (c := json.loads((build_state.DRIVERS_ROOT / p).read_text()))['bundle_identifier'] in needed]
    evidence = [verify_abi_contract(contract) for contract in contracts]
    external = {c['bundle_identifier']: set(c['required_symbols']) for c in contracts}
    provider_classes = {c['bundle_identifier']: set(c.get('classes', {})) for c in contracts}
    provider_symbols = {**external, **{identifier: set(r['defined']) for identifier, r in by_id.items()}}
    for report in reports:
        report['matching_personalities'] = {
            name: verify_matching_personality(personality, report['identifier'], report['libraries'],
                                               set(report['defined']), provider_symbols, provider_classes)
            for name, personality in report.pop('personalities').items()}
        available = set(kernel_symbols)
        for dependency in report['libraries']:
            if dependency in by_id:
                available.update(by_id[dependency]['defined'])
            available.update(external.get(dependency, set()))
        missing = set(report['imports']) - available
        if missing:
            raise RuntimeError(f'unaccounted driver imports: {report["bundle"]}: {sorted(missing)}')
        # A defined kernel symbol is not necessarily exported by a declared
        # KPI family. Check this for all platform bundles, including inherited
        # vtable references and the machine-layer CPU registration interfaces.
        import fnmatch
        patterns = driver_kpi_patterns(report['libraries'])
        unavailable = [s for s in report['imports'] if s in kernel_symbols and
                       not any(fnmatch.fnmatchcase(s, p) for p in patterns)]
        if unavailable:
            raise RuntimeError(f'imports outside declared KPI exports: {report["bundle"]}: {unavailable}')
        report['unavailable_kpis'] = unavailable
        report['external_provider_imports'] = sorted(set(report['imports']) &
            {s for dependency in report['libraries'] for s in external.get(dependency, set())})
    for report in reports:
        report.pop('defined')
    result = {'drivers': reports, 'provider_contracts': contracts, 'provider_evidence': evidence,
              'status': 'offline-build-verified',
              'kernel_collection_link_verified': False, 'hardware_boot_verified': False}
    path = root.parent / 'verification-drivers.json'
    path.write_text(json.dumps(result, indent=2) + '\n')
    print(f'Verified {len(reports)} arm64 Kext bundles: {root}; external providers: {sorted(external)}')


def verify_matching_personality(personality, identifier, libraries, local_symbols, providers, provider_classes):
    """Apple supports personalities instantiating a class in a declared dependency.

    Check the class against an actual local binary or a hash/layout-verified
    native provider contract, rather than accepting an arbitrary IOClass name.
    """
    cls = personality.get('IOClass')
    provider = personality.get('IOProviderClass')
    target = personality.get('CFBundleIdentifier', identifier)
    if not isinstance(cls, str) or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', cls) or not isinstance(provider, str) or not provider:
        raise RuntimeError('invalid Apple matching personality')
    if target != identifier and target not in libraries:
        raise RuntimeError('personality class provider is not a declared dependency')
    symbols = local_symbols if target == identifier else providers.get(target, set())
    if f'__ZN{len(cls)}{cls}10gMetaClassE' not in symbols:
        raise RuntimeError(f'personality class missing from binary/provider: {cls}')
    if target in provider_classes and cls not in provider_classes[target]:
        raise RuntimeError(f'personality class lacks a native layout contract: {cls}')
    return {'class': cls, 'bundle_identifier': target, 'provider_class': provider}


def verify_compiler_vtable(text, name, contract):
    match = re.search(r"Vtable for '" + re.escape(name) + r"' \((\d+) entries\)\.\n(.*?)(?=\n\n|\Z)",
                      text, re.S)
    if not match:
        raise RuntimeError(f'compiler vtable missing: {name}')
    entries = {int(i): value for i, value in re.findall(r'^\s*(\d+) \| (.*)$', match[2], re.M)}
    # Apple Kext codegen appends one vcall_offset after the virtual entries.
    if entries.get(int(match[1]) - 1) != 'vcall_offset (0)':
        raise RuntimeError(f'unexpected Kext compiler vtable terminator: {name}')
    slots = int(match[1]) - 3
    if slots != contract['slots'] or any('[pure]' in v for v in entries.values()):
        raise RuntimeError(f'compiled subclass vtable is incompatible/abstract: {name}')
    bindings = {}
    for slot, expected in contract.get('bindings', {}).items():
        actual = entries.get(int(slot) + 2, '')
        if expected not in actual:
            raise RuntimeError(f'compiled virtual hook moved: {name}[{slot}]: {actual}')
        bindings[slot] = actual
    return {'slots': slots, 'bindings': bindings}


def provider_function_bytes(image, address, length, executable=True):
    """Read audited code/data through Mach-O segments, never a raw file offset."""
    data = image.read_bytes()
    if len(data) < 32 or struct.unpack_from('<I', data)[0] != 0xfeedfacf:
        raise RuntimeError('provider is not a 64-bit Mach-O')
    count, command_bytes = struct.unpack_from('<II', data, 16)
    end = 32 + command_bytes
    if end > len(data) or address & 3 or length < 4 or length > 256:
        raise RuntimeError('invalid provider function binding')
    pos = 32
    for _ in range(count):
        if pos + 8 > end:
            raise RuntimeError('truncated provider load command')
        cmd, size = struct.unpack_from('<II', data, pos)
        if size < 8 or pos + size > end:
            raise RuntimeError('invalid provider load command')
        if cmd == 0x19:
            if size < 72:
                raise RuntimeError('truncated provider segment')
            vm, _, offset, file_size, _, prot = struct.unpack_from('<QQQQii', data, pos + 24)
            if vm <= address and address + length <= vm + file_size and (not executable or prot & 4):
                start = offset + address - vm
                if start + length > len(data):
                    raise RuntimeError('provider function outside file')
                return data[start:start + length]
        pos += size
    raise RuntimeError('provider function is not executable/file-backed')


def verify_abi_contract(contract):
    from project.build import native as _component_native
    import tempfile
    image = build_state.NATIVE_KERNELCACHE
    if image is None:
        configured = contract.get('image', driver_configuration().get('provider_image'))
        if not configured:
            raise RuntimeError('ABI contract needs --provider-image or a configured image path')
        image = (build_state.DRIVERS_ROOT / configured).resolve()
    if _component_native.digest(image) != contract['image_sha256']:
        raise RuntimeError('provider image hash changed; re-audit the ABI contract')
    patcher = build_state.BUILD / 'obj/kernelcache_patcher/kernelcache_patcher'
    project = build_state.TOOLS / 'kernelcache_patcher'
    inputs = [*project.glob('*.cpp'), *project.glob('include/*.h'), *project.glob('include/*.inc'), project / 'CMakeLists.txt']
    path = build_state.BUILD / 'obj/drivers' / (contract['image_sha256'] + '.abi.json')
    path.parent.mkdir(parents=True, exist_ok=True)
    # Several independent driver builds can inspect the same provider. Parse
    # this invocation's private output; publishing a shared filename directly
    # lets one process read another process's partially written JSON.
    # One analysis recovers every provider class. Share that immutable result
    # within this invocation; each contract still gets its own alias bindings
    # and byte/layout checks. Include exact kernel/tool hashes in the identity.
    with build_state.ABI_ANALYSIS_LOCK:
        if not patcher.exists() or max(p.stat().st_mtime for p in inputs) > patcher.stat().st_mtime:
            _component_native.run(['cmake', '-S', project, '-B', patcher.parent, '-DCMAKE_BUILD_TYPE=Release'])
            _component_native.run(['cmake', '--build', patcher.parent, '-j', '3'])
        kernel = build_state.BUILD / 'output' / (build_state.KERNEL_FILE + '.unstripped')
        key = (str(image.resolve()), contract['image_sha256'], str(kernel.resolve()), _component_native.digest(kernel), _component_native.digest(patcher))
        if key not in build_state.ABI_ANALYSES:
            with tempfile.TemporaryDirectory(prefix='.abi-', dir=path.parent) as directory:
                pending = Path(directory) / 'report.json'
                _component_native.run([patcher, 'inspect-abi', kernel, image, pending], log=Path(directory) / 'analysis.log')
                raw = pending.read_text()
                json.loads(raw)
                if _component_native.digest(kernel) != key[3] or _component_native.digest(patcher) != key[4]:
                    raise RuntimeError('ABI analysis inputs changed during inspection')
                pending.replace(path)
                build_state.ABI_ANALYSES[key] = raw
        evidence = json.loads(build_state.ABI_ANALYSES[key])
    classes = {c['name']: c for c in evidence['classes']}
    for name, expected in contract.get('classes', {}).items():
        cls = classes.get(name, {})
        if any(cls.get(key) != value for key, value in expected.items()):
            raise RuntimeError(f'provider class ABI differs from contract: {name}')
    for symbol, binding in contract.get('virtual_symbols', {}).items():
        slots = classes.get(binding['class'], {}).get('slots', [])
        index = binding['slot']
        if not isinstance(index, int) or index < 0 or index >= len(slots) or not slots[index]:
            raise RuntimeError(f'contract virtual slot unavailable: {symbol}')
        if symbol in evidence['symbols'] and evidence['symbols'][symbol] != slots[index]:
            raise RuntimeError(f'contract virtual slot contradicts provider symbol: {symbol}')
        evidence['symbols'][symbol] = slots[index]
    for symbol, binding in contract.get('function_bindings', {}).items():
        address = int(binding['address'], 0)
        expected = bytes.fromhex(binding['bytes'])
        if provider_function_bytes(image, address, len(expected)) != expected:
            raise RuntimeError(f'provider function bytes differ from contract: {symbol}')
        if symbol in evidence['symbols'] and evidence['symbols'][symbol] != address:
            raise RuntimeError(f'provider function contradicts existing symbol: {symbol}')
        evidence['symbols'][symbol] = address
    missing = set(contract.get('required_symbols', [])) - evidence['symbols'].keys()
    if missing:
        raise RuntimeError(f'provider ABI symbols could not be recovered: {sorted(missing)}')
    return {'image_sha256': contract['image_sha256'],
            'classes': {name: {k: v for k, v in classes[name].items() if k != 'slots'}
                        for name in contract.get('classes', {})},
            'symbols': {name: evidence['symbols'][name] for name in contract.get('required_symbols', [])}}


def verify_driver_abi(sources, config=None):
    """Compile private ABI probes only for providers used by this driver build."""
    from project.build import native as _component_native
    config = driver_configuration() if config is None else config
    libraries = {name for _, info in sources for name in info.get('OSBundleLibraries', {})}
    consumers = {info['CFBundleExecutable'] for _, info in sources}
    for check in config.get('abi_checks', []):
        if not (libraries.intersection(check.get('libraries', [])) or
                consumers.intersection(check.get('consumers', []))):
            continue
        if not re.fullmatch(r'[A-Za-z0-9_-]+', check['name']):
            raise RuntimeError('invalid ABI check name')
        if not check.get('abi_contracts'):
            raise RuntimeError('missing firmware ABI contracts for ' + check['name'])
        includes = ['-I' + str(build_state.DRIVERS_ROOT / p) for p in check.get('include_dirs', [])]
        for index, relative in enumerate(check.get('kernel_compile', [])):
            source = build_state.DRIVERS_ROOT / relative
            cpp = source.suffix in ('.cpp', '.cc', '.cxx')
            if not cpp and source.suffix != '.c':
                raise RuntimeError(f'unsupported kernel compile check: {source}')
            objdir = build_state.BUILD / 'obj/drivers/abi' / check['name']
            objdir.mkdir(parents=True, exist_ok=True)
            object_path = objdir / (str(index) + '-' + source.name + '.o')
            layouts = check.get('kernel_vtables', {}).get(relative, {})
            layout_flags = ['-Xclang', '-fdump-vtable-layouts'] if layouts else []
            layout_log = objdir / (str(index) + '-' + source.name + '.vtable.txt') if layouts else None
            if layout_log:
                layout_log.unlink(missing_ok=True)
            _component_native.run([build_state.BIN / ('clang++-arm64' if cpp else 'clang-arm64'),
                 *driver_compile_flags(), *includes, *driver_language_flags(cpp), *layout_flags,
                 '-c', source, '-o', object_path], log=layout_log)
            for name, contract in layouts.items():
                verify_compiler_vtable(layout_log.read_text(), name, contract)
            # A positional ABI check alone does not prove that signatures in
            # a probe still match its audited imports. New/changed provider
            # references must be added to the contract before this can pass.
            provider_contracts = check.get('kernel_provider_imports', {}).get(relative, [])
            if provider_contracts:
                nm = subprocess.check_output([build_state.BIN / 'arm-apple-darwin11-nm', '-u', object_path], text=True)
                imported = {line.strip() for line in nm.splitlines()}
                for relative_contract in provider_contracts:
                    contract = json.loads((build_state.DRIVERS_ROOT / relative_contract).read_text())
                    # Itanium member/static/constructor/destructor names for
                    # classes owned by this provider; KPI references remain
                    # covered by the ordinary Kext verifier once linked.
                    prefixes = tuple('__ZN' + qualifier + str(len(name)) + name
                                     for name in contract.get('classes', {}) for qualifier in ('', 'K'))
                    selected = {name for name in imported if name.startswith(prefixes)}
                    missing = selected - set(contract.get('required_symbols', []))
                    if missing:
                        raise RuntimeError(f'unaudited kernel probe provider imports: {sorted(missing)}')
        for relative in check.get('abi_contracts', []):
            verify_abi_contract(json.loads((build_state.DRIVERS_ROOT / relative).read_text()))


def collection_bundle_order(metadata, roots, library_reader=None):
    """Resolve every declared provider without dropping unavailable libraries."""
    order, active, done, missing = [], set(), set(), {}
    def visit(name, parent):
        if name in done:
            return
        if name in active:
            raise RuntimeError(f'cyclic bundle dependency: {name}')
        if name not in metadata:
            missing.setdefault(name, set()).add(parent)
            return
        active.add(name)
        libraries = library_reader(metadata[name]) if library_reader else metadata[name].get('OSBundleLibraries', {})
        for dependency in sorted(libraries):
            visit(dependency, name)
        active.remove(name)
        done.add(name)
        order.append(name)
    for name in roots:
        visit(name, 'collection root')
    if missing:
        details = '; '.join(f'{name} (required by {", ".join(sorted(parents))})'
                            for name, parents in sorted(missing.items()))
        raise RuntimeError(f'missing collection dependencies: {details}')
    if not order:
        raise RuntimeError('empty collection')
    return order


def collection_inputs(collection, base_manifest=None):
    """Use the same dependency closure for component builds, verification and packing."""
    if not collection or not re.fullmatch(r'[A-Za-z0-9_-]+', collection):
        raise RuntimeError('invalid or missing collection name')
    config = driver_configuration()
    definition = config.get('collections', {}).get(collection)
    if not definition:
        raise RuntimeError('select a configured collection with --collection')
    image = build_state.NATIVE_KERNELCACHE
    if image is None:
        configured = config.get('provider_image')
        if not configured:
            raise RuntimeError('collection needs --provider-image or a configured provider image')
        image = (build_state.DRIVERS_ROOT / configured).resolve()
    from project.analysis import kernelcache as inventory
    _, native = inventory.inspect(image)
    native_by_id = {k['CFBundleIdentifier']: k for k in native}
    sources = driver_sources()
    local = {info['CFBundleIdentifier']: (source, info) for source, info in sources}
    collision = local.keys() & native_by_id.keys()
    if collision:
        raise RuntimeError(f'local bundle IDs shadow provider bundles: {sorted(collision)}')
    metadata = dict(native_by_id)
    metadata.update({name: item[1] for name, item in local.items()})
    roots = list(definition.get('roots', []))
    if base_manifest:
        roots += [line.strip() for line in base_manifest.read_text().splitlines()
                  if line.strip() and not line.lstrip().startswith('#')]
    order = collection_bundle_order(metadata, roots, inventory.architecture_libraries)
    return config, definition, image, order, local


def collection_sources(collection):
    _, _, _, order, local = collection_inputs(collection)
    return [local[name] for name in order if name in local]


def pack_drivers(jobs, collection, output=None, base_manifest=None, local_kernel=None):
    """Resolve a configured collection, build local bundles, then use the generic KC linker."""
    from project.build import native as _component_native
    config, definition, image, order, local = collection_inputs(collection, base_manifest)
    selected = [local[name] for name in order if name in local]
    if selected:
        build_drivers(jobs, selected)
    (build_state.BUILD / 'obj/collections').mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=collection + '-', dir=build_state.BUILD / 'obj/collections'))
    inputs = stage / 'inputs'
    inputs.mkdir()
    kernel = snapshot_input(local_kernel or build_state.BUILD / 'output' / (build_state.KERNEL_FILE + '.unstripped'), inputs / 'kernel.macho')
    image = snapshot_input(image, inputs / 'provider.macho')
    binaries = {}
    for name in order:
        if name not in local:
            continue
        info = local[name][1]
        bundle = build_state.DRIVER_OUTPUT / (info['CFBundleExecutable'] + '.kext')
        link = stage / (name + '.kext')
        (link / 'Contents/MacOS').mkdir(parents=True)
        binary = snapshot_input(bundle / 'Contents/MacOS' / info['CFBundleExecutable'],
                                link / 'Contents/MacOS' / info['CFBundleExecutable'])
        (link / 'Contents/Info.plist').write_bytes(plistlib.dumps(info, sort_keys=False))
        binaries[name] = _component_native.digest(binary)
    (stage / 'kexts.txt').write_text('\n'.join(order) + '\n')
    patcher = build_state.BUILD / 'obj/kernelcache_patcher/kernelcache_patcher'
    _component_native.run(['cmake', '-S', build_state.TOOLS / 'kernelcache_patcher', '-B', patcher.parent,
                           '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=ON'])
    _component_native.run(['cmake', '--build', patcher.parent, '-j', str(jobs)])
    _component_native.run(['ctest', '--test-dir', patcher.parent, '--output-on-failure', '--no-tests=error'])
    if output is None:
        configured = definition.get('output')
        if not configured:
            raise RuntimeError('collection needs --output or a configured output path')
        output = build_state.DRIVERS_ROOT / configured
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    log = stage / 'link.log'
    # The same hash/layout-checked provider contracts used for import
    # accounting must also reach the linker. Otherwise a stripped provider
    # can pass offline accounting but leave subclass virtual imports unresolved.
    provider_symbols = {}
    for relative in config.get('abi_contracts', []):
        contract = json.loads((build_state.DRIVERS_ROOT / relative).read_text())
        if contract['bundle_identifier'] not in order:
            continue
        verified = verify_abi_contract(contract)
        for name, address in verified['symbols'].items():
            if name in provider_symbols and provider_symbols[name] != address:
                raise RuntimeError(f'conflicting provider ABI symbol: {name}')
            provider_symbols[name] = address
    symbol_argument = '-'
    if provider_symbols:
        symbol_path = stage / 'provider-symbols.txt'
        symbol_path.write_text(''.join(f'{address:x},{name}\n'
                                     for name, address in sorted(provider_symbols.items())))
        symbol_argument = '@' + str(symbol_path)
    _component_native.run([patcher, kernel, image, symbol_argument, stage, output], log=log)
    report = json.loads(Path(str(output) + '.verification.json').read_text())
    if report.get('status') != 'passed' or report.get('kext_count') != len(order):
        raise RuntimeError('collection verifier did not confirm the expected bundle count')
    manifest = {'bundle_order': order, 'local_bundle_sha256': binaries, 'kernel_sha256': _component_native.digest(kernel),
                'provider_sha256': _component_native.digest(image), 'output_sha256': _component_native.digest(output),
                'inputs': {'kernel': os.path.relpath(kernel, output.parent),
                           'provider': os.path.relpath(image, output.parent)},
                'link_log': os.path.relpath(log, output.parent),
                'hardware_boot_verified': False}
    Path(str(output) + '.manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Collection published: {output}; {len(order)} Kexts; SHA256 {manifest["output_sha256"]}')


def snapshot_input(source, destination):
    """Freeze mutable build inputs before a collection consumes them."""
    from project.build import native as _component_native
    for _ in range(3):
        before = _component_native.digest(source)
        _component_native.run(['cp', '--reflink=auto', source, destination])
        if _component_native.digest(destination) == before == _component_native.digest(source):
            return destination
    raise RuntimeError(f'input changed repeatedly while snapshotting: {source}')


def verify_collection(collection=None, output=None):
    from project.build import native as _component_native
    if output is None:
        configured = driver_configuration().get('collections', {}).get(collection, {}).get('output')
        if not configured:
            raise RuntimeError('select --output or a configured --collection')
        output = build_state.DRIVERS_ROOT / configured
    output = output.resolve()
    manifest = json.loads(Path(str(output) + '.manifest.json').read_text())
    kernel = (output.parent / manifest['inputs']['kernel']).resolve()
    provider = (output.parent / manifest['inputs']['provider']).resolve()
    for path, key in ((output, 'output_sha256'), (kernel, 'kernel_sha256'), (provider, 'provider_sha256')):
        if _component_native.digest(path) != manifest[key]:
            raise RuntimeError(f'collection input/output differs from manifest: {path}')
    expected = [arg for name in manifest['bundle_order'] for arg in ('--expected-id', name)]
    from project.execution import invoke
    invoke('project.analysis.verify_kernelcache', [output,
         '--report', str(output) + '.verification.json', '--source-kernelcache', provider,
         '--local-kernel', kernel, *expected])
