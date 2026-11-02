"""XNU source preparation, compilation and verification."""
import hashlib
import json
import os
import shutil
import struct
import tarfile
from project.build import context as build_state


def make_args(jobs):
    from project.build import native as _component_native
    values = {
        'SYSCTL_HW_PHYSICALCPU': jobs, 'SYSCTL_HW_LOGICALCPU': jobs,
        'MEMORY_SIZE': 8589934592, 'MAKEJOBS': f'-j{jobs}',
        'SDKROOT': 'iphoneos', 'SDKROOT_RESOLVED': build_state.SDK, 'HOST_SDKROOT_RESOLVED': '/',
        'PLATFORM': 'iPhoneOS', 'PLATFORMPATH': build_state.BUILD / 'obj/platform/iPhoneOS.platform',
        'SDKVERSION': '18.6', 'RC_DARWIN_KERNEL_VERSION': '24.6.0', 'KDKROOT': '',
        'ARCH_CONFIGS': 'ARM64', 'MACHINE_CONFIGS': build_state.TARGET, 'KERNEL_CONFIGS': 'DEVELOPMENT',
        'ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG': 'arm64',
        'XCRUN': build_state.BIN / 'xcrun', 'CC': build_state.BIN / 'clang-arm64', 'CXX': build_state.BIN / 'clang++-arm64',
        'HOST_CC': build_state.BIN / 'host-cc', 'MIG': build_state.BIN / 'mig', 'MIGCOM': build_state.PREFIX / 'libexec/migcom',
        'MIGCC': build_state.BIN / 'clang-arm64', 'IIG': build_state.BIN / 'iig', 'PLUTIL': build_state.BIN / 'plutil',
        'UNIFDEF': build_state.BIN / 'unifdef', 'HOST_FLEX': _component_native.host('flex'), 'HOST_BISON': _component_native.host('bison'),
        'HOST_GM4': _component_native.host('m4'), 'HOST_OS_VERSION': '6.0', 'HOST_CODESIGN': _component_native.host('true'),
        'HOST_CODESIGN_ALLOCATE': _component_native.host('false'), 'EMBEDDED_DEVICE_MAP': _component_native.host('false'),
        'CTFCONVERT': _component_native.host('false'), 'CTFMERGE': _component_native.host('false'),
        'MAKEKDEBUGEVENTS': build_state.BIN / 'makekdebugevents',
        'OBJROOT': build_state.OBJ, 'DSTROOT': build_state.BUILD / 'obj/dst', 'SYMROOT': build_state.BUILD / 'obj/sym',
        'BUILD_WERROR': 0, 'BUILD_LTO': 0, 'BUILD_DSYM': 0, 'DO_CTFMERGE': 0,
        'USE_BINARY_PLIST': 0,
        'CFLAGS_EXTRA': f'-DOS_FIREHOSE_SPI=1 -DXNU_SOFTWARE_PANIC={os.environ.get("XNU_SOFTWARE_PANIC", "1" if build_state.TARGET in ("SDM835", "SM8150") else "0")} -I{build_state.TOOLS}/private-headers -I{build_state.TOOLS}/compat',
    }
    for name in ('strip', 'lipo', 'libtool', 'nm', 'nmedit', 'ctf_insert'):
        values[name.upper().replace('CTF_INSERT', 'CTFINSERT')] = build_state.BIN / ('arm-apple-darwin11-' + name)
    return [f'{key}={value}' for key, value in values.items()]


def make_xnu(target, jobs, log, extra=(), check=True):
    from project.build import native as _component_native
    env = dict(os.environ)
    env['XNU_MACHINE_CONFIG'] = build_state.TARGET
    env['PATH'] = str(build_state.BIN) + os.pathsep + env['PATH']
    env['LD_LIBRARY_PATH'] = str(build_state.PREFIX / 'lib') + os.pathsep + env.get('LD_LIBRARY_PATH', '')
    return _component_native.run(['make', '-C', build_state.SRC, f'-j{jobs}', target, *make_args(jobs), *extra],
               log=log, check=check, env=env)


def prepare_sdk(jobs):
    from project.build import native as _component_native
    marker = build_state.BUILD / '.workspace-root'
    workspace_paths = f'{build_state.ROOT}\n{build_state.SRC}'
    if marker.exists() and marker.read_text().strip() != workspace_paths:
        # Only our regenerable target build directories contain absolute paths.
        for name in ('obj', 'headers', 'sdk', 'runtime', 'sym', 'dst'):
            path = build_state.BUILD / name
            if path.exists():
                shutil.rmtree(path)
        for pattern in ('.headers-installed*', '.config-signature*'):
            for stamp in build_state.BUILD.glob(pattern):
                stamp.unlink()
    build_state.BUILD.mkdir(parents=True, exist_ok=True)
    marker.write_text(workspace_paths + '\n')
    original = build_state.ROOT / 'tools/sdk/iPhoneOS18.6.sdk'
    if not (build_state.SDK / 'SDKSettings.plist').exists():
        build_state.SDK.parent.mkdir(parents=True, exist_ok=True)
        _component_native.run(['cp', '-a', '--reflink=auto', str(original) + '/.', build_state.SDK])
        _component_native.run(['chmod', '-R', 'u+w', build_state.SDK])
    (build_state.SDK / 'usr/local/include/kernel/os').mkdir(parents=True, exist_ok=True)
    (build_state.SDK / 'usr/local/lib/kernel').mkdir(parents=True, exist_ok=True)
    (build_state.BUILD / 'obj/platform/iPhoneOS.platform').mkdir(parents=True, exist_ok=True)
    for sub in ('include/os', 'private/os'):
        shutil.copytree(build_state.ROOT / 'tools/src/libplatform' / sub, build_state.SDK / 'usr/local/include/os', dirs_exist_ok=True)
    shutil.copytree(build_state.ROOT / 'tools/src/libdispatch/os', build_state.SDK / 'usr/local/include/os', dirs_exist_ok=True)
    shutil.copy2(build_state.ROOT / 'tools/src/libdispatch/os/firehose_buffer_private.h',
                 build_state.SDK / 'usr/local/include/kernel/os/firehose_buffer_private.h')
    availability = build_state.ROOT / 'tools/src/AvailabilityVersions/availability.pl'
    (build_state.SDK / 'usr/local/libexec').mkdir(parents=True, exist_ok=True)
    if availability.exists() and not availability.read_text().startswith('#!/bin/sh'):
        shutil.copy2(availability, build_state.SDK / 'usr/local/libexec/availability.pl')
    else:
        source = availability.with_name('availability')
        _component_native.run([_component_native.host('python3'), source, '--av_version', '151', '--preprocess', source,
             build_state.SDK / 'usr/local/libexec/availability.pl'])
        (build_state.SDK / 'usr/local/libexec/availability.pl').chmod(0o755)
    other_source_patches = set()
    for dependency in ('apfs-tools.json', 'usb-storage.json'):
        specification = build_state.TOOLS / dependency
        if specification.exists():
            other_source_patches.update(json.loads(specification.read_text()).get('patches', []))
    other_source_patches.update({'apfsprogs-ios18-native-metadata.patch',
                                'go-apfs-sealed-readonly.patch'})
    # Later patches intentionally modify earlier patches. Reconstruct the whole
    # recipe in isolation and recognize the current prefix before any writes.
    from project.build.kernel_patches import ensure_kernel_patch_stack, order_kernel_patches
    patches = order_kernel_patches([patch for patch in (build_state.TOOLS / 'patches').glob('*.patch')
                                   if patch.name not in other_source_patches])
    specification = json.loads(build_state.LOCK.read_text())['xnu']
    ensure_kernel_patch_stack(build_state.SRC, patches, build_state.ROOT / 'tools/cache' / specification['archive'],
                              specification['sha256'], build_state.BUILD / 'logs/patches.log')
    # Upstream DOCONFDEPS omits platform MASTER files. Explicitly invalidate
    # generated configuration makefiles when that input changes.
    signature = _component_native.digest(build_state.SRC / 'config/MASTER.arm64.iPhoneOS')
    stamp = build_state.BUILD / ('.config-signature-' + build_state.TARGET.lower())
    if not stamp.exists() or stamp.read_text().strip() != signature:
        for path in build_state.CONFIG.glob('*/DEVELOPMENT/Makefile'):
            path.unlink()
        stamp.write_text(signature + '\n')
    headers_stamp = build_state.BUILD / ('.headers-installed-' + build_state.TARGET.lower())
    if not headers_stamp.exists():
        make_xnu('installhdrs', jobs, build_state.BUILD / 'logs/headers.log', [f'DSTROOT={build_state.BUILD}/headers'])
        _component_native.run(['cp', '-a', '--remove-destination', '--reflink=auto',
             str(build_state.BUILD / 'headers') + '/.', build_state.SDK], log=build_state.BUILD / 'logs/headers.log')
        headers_stamp.touch()


def build_kernel(jobs):
    from project.build import native as _component_native
    _component_native.aliases()
    _component_native.fetch_mirrors()
    prepare_sdk(jobs)
    build_runtime()
    panic_mode = os.environ.get('XNU_SOFTWARE_PANIC', '1' if build_state.TARGET in ('SDM835', 'SM8150') else '0')
    if panic_mode not in ('0', '1'):
        raise RuntimeError('XNU_SOFTWARE_PANIC must be 0 or 1')
    panic_stamp = build_state.BUILD / ('.software-panic-' + build_state.TARGET.lower())
    if not panic_stamp.exists() or panic_stamp.read_text().strip() != panic_mode:
        # Only the panic character sink changes. Invalidate it when toggling the macro.
        (build_state.CONFIG / 'osfmk/DEVELOPMENT/printf.o').unlink(missing_ok=True)
        panic_stamp.write_text(panic_mode + '\n')
    # IIG products are needed both by header export and the configured IOKit
    # Makefile, whose generated-file paths are relative to DEVELOPMENT/.
    for definition in sorted((build_state.SRC / 'iokit/DriverKit').glob('*.iig')):
        for directory in (build_state.CONFIG / 'iokit/DriverKit', build_state.CONFIG / 'iokit/DEVELOPMENT/DriverKit'):
            _component_native.iig(['--def', str(definition), '--header', str(directory / (definition.stem + '.h')),
                 '--impl', str(directory / (definition.stem + '.iig.cpp'))])
    log = build_state.BUILD / ('kernel-' + build_state.TARGET.lower() + '.log')
    log.write_text('')
    make_xnu('build', jobs, log)
    output = build_state.BUILD / 'output'
    output.mkdir(parents=True, exist_ok=True)
    for suffix in ('', '.unstripped'):
        source = build_state.CONFIG / (build_state.KERNEL_FILE + suffix)
        if not source.exists():
            raise RuntimeError(f'expected kernel missing: {source}')
        shutil.copy2(source, output / source.name)
    verify_kernel()


def verify_kernel():
    from project.build import native as _component_native
    results = []
    for suffix in ('', '.unstripped'):
        path = build_state.BUILD / 'output' / (build_state.KERNEL_FILE + suffix)
        data = path.read_bytes()
        magic, cpu, subtype, filetype, ncmds, size, flags, reserved = struct.unpack_from('<8I', data)
        if (magic, cpu, subtype & 0xffffff, filetype) != (0xfeedfacf, 0x100000c, 0, 2):
            raise RuntimeError(f'{path.name}: expected thin arm64 MH_EXECUTE')
        offset, platform, version, entry, symbols = 32, None, None, None, None
        segments, dylibs = [], []
        for _ in range(ncmds):
            cmd, length = struct.unpack_from('<II', data, offset)
            if length < 8 or offset + length > 32 + size:
                raise RuntimeError('malformed Mach-O load commands')
            if cmd == 0x19:
                name, vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<16s4Q', data, offset + 8)
                segments.append({'name': name.rstrip(b'\0').decode(), 'address': hex(vmaddr),
                                 'size': vmsize, 'file_offset': fileoff, 'file_size': filesize})
            elif cmd == 0x32:
                platform, version = struct.unpack_from('<II', data, offset + 8)
            elif cmd == 0x25:
                platform, version = 2, struct.unpack_from('<I', data, offset + 8)[0]
            elif cmd == 5:
                entry = struct.unpack_from('<Q', data, offset + 16 + 32 * 8)[0]
            elif cmd == 2:
                symbols = struct.unpack_from('<4I', data, offset + 8)
            elif cmd in (0xc, 0x18, 0x80000018, 0x8000001f):
                dylibs.append(cmd)
            offset += length
        if platform != 2 or version != (18 << 16 | 6 << 8):
            raise RuntimeError(f'{path.name}: expected iPhoneOS 18.6 deployment command')
        if not entry or dylibs or not flags & 1:
            raise RuntimeError(f'{path.name}: invalid static kernel entry/flags/dependencies')
        if not any(int(s['address'], 16) <= entry < int(s['address'], 16) + s['size'] for s in segments):
            raise RuntimeError('entry lies outside mapped segments')
        undefined, defined_names = [], set()
        if symbols:
            symoff, count, stroff, strsize = symbols
            for index in range(count):
                string, kind, section, desc, value = struct.unpack_from('<IBBHQ', data, symoff + 16 * index)
                if string and kind & 0xe0 == 0 and kind & 0x0e == 0x0e:
                    end = data.index(0, stroff + string, stroff + strsize)
                    defined_names.add(data[stroff + string:end].decode())
                if kind & 0xe0 == 0 and kind & 0x0e == 0 and kind & 1:
                    end = data.index(0, stroff + string, stroff + strsize)
                    undefined.append(data[stroff + string:end].decode())
        if undefined:
            raise RuntimeError(f'unresolved symbols: {undefined}')
        required = {'_pe_sdm835_init_interrupts', '_pe_sdm835_handle_irq',
                    '_PE_dispatch_irq',
                    '_pe_sdm835_cpu_on', '_pe_sdm835_cpu_off', '_pe_sdm835_send_ipi',
                    '_qcom_uartdm_init', '_qcom_uartdm_transmit_data',
                    '_qcom_uartdm_receive_ready', '_qcom_uartdm_receive_data'}
        if build_state.TARGET == 'SM8150':
            required -= {'_qcom_uartdm_init', '_qcom_uartdm_transmit_data',
                         '_qcom_uartdm_receive_ready', '_qcom_uartdm_receive_data'}
            required |= {'_ml_usb_console_read_tx', '_ml_usb_console_write_rx',
                         '_ml_usb_console_set_enabled'}
        if build_state.TARGET in ('SDM835', 'SM8150') and suffix == '.unstripped' and not required <= defined_names:
            raise RuntimeError(f'{build_state.TARGET} platform symbols absent: {sorted(required - defined_names)}')
        results.append({'file': path.name, 'sha256': _component_native.digest(path), 'bytes': len(data),
                        'architecture': 'arm64', 'platform': 'iPhoneOS', 'sdk': '18.6',
                        'entry': hex(entry), 'undefined_symbols': undefined, 'segments': segments,
                        'platform_symbols_verified': sorted(required) if build_state.TARGET in ('SDM835', 'SM8150') and suffix == '.unstripped' else []})
    report = {'xnu': '11417.140.69', 'config': 'DEVELOPMENT_ARM64_' + build_state.TARGET, 'kernels': results,
              'hardware_boot_verified': False}
    (build_state.BUILD / ('output/verification-' + build_state.TARGET.lower() + '.json')).write_text(json.dumps(report, indent=2) + '\n')
    (build_state.BUILD / 'output/verification.json').write_text(json.dumps(report, indent=2) + '\n')
    for r in results:
        print(f'{r["file"]}: arm64 / iPhoneOS 18.6 / {r["bytes"]} bytes / no undefined symbols')
        print(f'SHA256 {r["sha256"]}')


def build_runtime():
    from project.build import native as _component_native
    work = build_state.BUILD / 'obj/runtime' / build_state.TARGET.lower()
    work.mkdir(parents=True, exist_ok=True)
    library = build_state.SDK / 'usr/local/lib/kernel' / build_state.TARGET.lower()
    library.mkdir(parents=True, exist_ok=True)
    arch_flag = '-march=armv8-a+crc+crypto' if build_state.TARGET in ('SDM835', 'SM8150') else '-march=armv8.5a+sme2'
    builtins = build_state.ROOT / 'tools/src/compiler-rt/lib/builtins'
    objects = []
    names = 'udivti3 divti3 umodti3 modti3 udivmodti4 multi3 ashlti3 ashrti3 lshrti3 clzti2 clzdi2 ctzti2 ctzdi2 ucmpti2 cmpti2 muloti4'.split()
    for name in names:
        source, output = builtins / (name + '.c'), work / (name + '.o')
        if not output.exists() or source.stat().st_mtime > output.stat().st_mtime:
            _component_native.run([build_state.BIN / 'clang-arm64', '-c', source, '-o', output, '-O2', '-mkernel',
                 '-fno-builtin', '-fno-stack-protector', arch_flag, '-isysroot', build_state.SDK], log=build_state.BUILD / 'logs/runtime.log')
        objects.append(output)
    source, output = build_state.TOOLS / 'compat/kernel-runtime.c', work / 'kernel-runtime.o'
    _component_native.run([build_state.BIN / 'clang-arm64', '-c', source, '-o', output, '-O2', '-mkernel',
         '-fno-builtin', '-fno-stack-protector', arch_flag, '-isysroot', build_state.SDK], log=build_state.BUILD / 'logs/runtime.log')
    _component_native.run([build_state.BIN / 'arm-apple-darwin11-libtool', '-static', '-o', library / 'libcc_kext.a',
         *objects, output], log=build_state.BUILD / 'logs/runtime.log')
    source, output = build_state.ROOT / 'tools/src/libdispatch/src/firehose/firehose_buffer.c', work / 'firehose.o'
    flags = ['-c', source, '-o', output, '-O2', '-g', '-mkernel', '-nostdlibinc',
             '-fno-builtin', '-fno-common', '-fno-strict-aliasing', arch_flag,
             '-DKERNEL', '-DPRIVATE', '-DKERNEL_PRIVATE', '-DXNU_KERNEL_PRIVATE',
             '-D__MACHO__=1', '-D__API__=v4', '-D__MACH30__', '-DARM64_BOARD_CONFIG_' + build_state.TARGET,
             '-DARM64', '-D__ARM64__', '-DLP64', '-D__ARM_ARCH__=8', '-DDISPATCH_USE_DTRACE=0',
             '-DOS_ATOMIC_CONFIG_MEMORY_ORDER_DEPENDENCY=1', '-DOS_ATOMIC_CONFIG_STARVATION_FREE_ONLY=0',
             f'-I{build_state.TOOLS}/src/libdispatch', f'-I{build_state.TOOLS}/src/libdispatch/src/firehose',
             f'-I{build_state.SRC}/EXTERNAL_HEADERS', f'-I{build_state.SDK}/usr/local/include/kernel']
    for name in ('bsd', 'osfmk', 'libkern', 'libkern/firehose', 'iokit', 'pexpert', 'libsa', 'security', 'san'):
        flags.append(f'-I{build_state.OBJ}/EXPORT_HDRS/{name}')
    _component_native.run([build_state.BIN / 'clang-arm64', *flags], log=build_state.BUILD / 'logs/runtime.log')
    _component_native.run([build_state.BIN / 'arm-apple-darwin11-libtool', '-static', '-o', library / 'libfirehose_kernel.a', output],
        log=build_state.BUILD / 'logs/runtime.log')


def collect_sdm835():
    """Restore pinned reference data, verifying every source against its manifest."""
    from project.build import native as _component_native
    manifest = json.loads((build_state.ROOT / 'platforms/qcom/msm8998/sources.json').read_text())
    for item in manifest['sources']:
        path = build_state.ROOT / 'local/reference/qcom/msm8998/sdm835' / item['file']
        if not path.exists():
            # Reuse local snapshots; this command downloads missing data.
            import urllib.request
            import base64
            raw = urllib.request.urlopen(item['url'], timeout=60).read()
            if '?format=TEXT' in item['url']:
                raw = base64.b64decode(raw)
            elif 'api.github.com' in item['url']:
                raw = base64.b64decode(json.loads(raw)['content'])
            if hashlib.sha256(raw).hexdigest() != item['sha256']:
                raise RuntimeError('reference download hash mismatch: ' + item['file'])
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(raw)
        if _component_native.digest(path) != item['sha256']:
            raise RuntimeError('reference hash mismatch: ' + item['file'])
        print('Verified reference: ' + item['file'])


def package():
    verify_kernel()
    output = build_state.BUILD / 'output' / f'xnu-toolchain-linux-{os.uname().machine}.tar.gz'
    def include(info):
        if '/.git/' in info.name or info.name.endswith('/.git') or '/__pycache__' in info.name:
            return None
        if info.name.startswith('xnu-toolchain/tools/cache/'):
            return None
        if info.name.endswith(('.orig', '.rej', '.pyc')):
            return None
        return info
    with tarfile.open(output, 'w:gz', compresslevel=3) as archive:
        for name in ('project.sh', 'pyproject.toml', 'README.md', '.gitignore', 'tools', 'kernel', 'drivers',
                     'boot/bridge', 'boot/storage', 'boards', 'platforms', 'firmware/ios', 'docs'):
            path = build_state.ROOT / name
            if path.exists():
                archive.add(path, arcname='xnu-toolchain/' + name, filter=include)
        # Ordered patch verification requires the original, checksum-pinned
        # XNU input even when the portable toolchain is used offline.
        xnu_archive = build_state.ROOT / 'tools/cache' / json.loads(build_state.LOCK.read_text())['xnu']['archive']
        archive.add(xnu_archive, arcname='xnu-toolchain/tools/cache/' + xnu_archive.name)
        for path in (build_state.BUILD / 'output').glob('mach*'):
            archive.add(path, arcname='xnu-toolchain/build/output/' + path.name)
        archive.add(build_state.BUILD / 'output/verification.json', arcname='xnu-toolchain/build/output/verification.json')
        for path in (build_state.BUILD / 'output').glob('verification-*.json'):
            archive.add(path, arcname='xnu-toolchain/build/output/' + path.name)
        extensions = build_state.BUILD / 'output/Extensions'
        if extensions.exists():
            archive.add(extensions, arcname='xnu-toolchain/build/output/Extensions')
    print(f'Portable archive: {output} ({output.stat().st_size} bytes)')
