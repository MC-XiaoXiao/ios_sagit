#!/usr/bin/env python3
"""Portable Linux -> iPhoneOS/ARM64 XNU toolchain; also serves generated tool aliases."""
import argparse
import concurrent.futures
import hashlib
import html
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import shlex
import subprocess
import struct
import sys
import tarfile
import tempfile
import threading
import zipfile

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / 'kernel'
TOOLS = ROOT / 'tools'
PREFIX = TOOLS / 'prefix'
BIN = PREFIX / 'bin'
BUILD = ROOT / 'build'
SDK = BUILD / 'sdk/iPhoneOS18.6.kernel.sdk'
OBJ = BUILD / 'obj'
TARGET = os.environ.get('XNU_MACHINE_CONFIG', 'SDM835')
CONFIG = OBJ / ('DEVELOPMENT_ARM64_' + TARGET)
KERNEL_FILE = 'mach.development.' + TARGET.lower()
NATIVE_KERNELCACHE = None
DRIVERS_ROOT = ROOT / 'drivers'
DRIVER_FILTERS = []
DRIVER_OUTPUT = BUILD / 'output/Extensions'
SELF = ROOT / 'scripts/toolchain.py'
LOCK = ROOT / 'tools/dependencies.json'
JOBS = max(1, min(4, os.cpu_count() or 1))
ABI_ANALYSIS_LOCK = threading.Lock()
ABI_ANALYSES = {}


def run(args, cwd=None, log=None, check=True, env=None):
    args = list(map(str, args))
    if log:
        Path(log).parent.mkdir(parents=True, exist_ok=True)
        with open(log, 'a') as out:
            result = subprocess.run(args, cwd=cwd, stdout=out, stderr=out, env=env)
    else:
        result = subprocess.run(args, cwd=cwd, env=env)
    if check and result.returncode:
        if log:
            print('\n'.join(Path(log).read_text(errors='replace').splitlines()[-60:]), file=sys.stderr)
        raise RuntimeError(f'command failed ({result.returncode}): {args[0]}; log={log}')
    return result.returncode


def exec_tool(tool, args):
    loader = PREFIX / 'libexec/ld.so'
    path = Path(tool)
    with path.open('rb') as stream:
        is_elf = stream.read(4) == b'\x7fELF'
    if loader.exists() and is_elf and path.is_relative_to(PREFIX):
        libraries = str(PREFIX / 'lib/glibc') + os.pathsep + str(PREFIX / 'lib')
        os.execv(str(loader), [str(loader), '--library-path', libraries,
                              '--argv0', str(BIN / path.name), str(path), *map(str, args)])
    os.execv(str(tool), [str(tool), *map(str, args)])


def host(name):
    # Avoid recursively resolving our own multicall aliases.
    search = os.pathsep.join(p for p in os.environ.get('PATH', '').split(os.pathsep)
                             if Path(p).resolve() != BIN.resolve())
    found = shutil.which(name, path=search)
    if not found:
        raise RuntimeError(f'missing host dependency: {name}')
    return found


def digest(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def compiler(cxx=False):
    path = PREFIX / 'libexec/native/clang-real'
    if not path.exists():
        path = BIN / 'clang-real'
    return str(path) if path.exists() else host('clang++' if cxx else 'clang')


def compiler_resource():
    meta = PREFIX / 'compiler.json'
    if meta.exists():
        return ['-resource-dir', str(PREFIX / 'lib/clang' / json.loads(meta.read_text())['resource_version'])]
    return []


def bundle_host_runtime():
    native = PREFIX / 'libexec/native'
    glibc = PREFIX / 'lib/glibc'
    native.mkdir(parents=True, exist_ok=True)
    glibc.mkdir(parents=True, exist_ok=True)
    result = subprocess.check_output(['ldd', compiler()], text=True)
    for name, path in re.findall(r'\s+(\S+) => (/\S+) \(', result):
        if name in ('libc.so.6', 'libm.so.6'):
            shutil.copy2(Path(path).resolve(), glibc / name)
    match = re.search(r'(/\S*ld-linux[^\s]*) \(', result)
    if not match:
        raise RuntimeError('cannot locate the host ELF loader')
    shutil.copy2(Path(match[1]).resolve(), PREFIX / 'libexec/ld.so')
    # Every native executable uses the same multicall dispatcher and bundled
    # loader. Keep glibc out of global LD_LIBRARY_PATH for host shell tools.
    for path in list(BIN.glob('*')) + [PREFIX / 'libexec/migcom']:
        if path.is_symlink() or not path.is_file():
            continue
        with path.open('rb') as stream:
            is_elf = stream.read(4) == b'\x7fELF'
        if is_elf:
            path.rename(native / path.name)
            path.symlink_to(os.path.relpath(SELF, path.parent))


def bundle_compiler():
    """Freeze stage-0 Clang and non-libc ELF dependencies in the prefix."""
    clang = Path(host('clang')).resolve()
    shutil.copy2(clang, BIN / 'clang-real')
    resource = Path(subprocess.check_output([clang, '-print-resource-dir'], text=True).strip())
    target = PREFIX / 'lib/clang' / resource.name
    shutil.copytree(resource / 'include', target / 'include', dirs_exist_ok=True)
    exclusions = {'libc.so.6', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2', 'librt.so.1'}
    dependencies = {}
    for executable in [clang, BIN / 'arm-apple-darwin11-ld', BIN / 'tcsh']:
        result = subprocess.check_output(['ldd', str(executable)], text=True)
        for name, path in re.findall(r'\s+(\S+) => (/\S+) \(', result):
            if name not in exclusions and not (PREFIX / 'lib' / name).exists():
                source = Path(path).resolve()
                shutil.copy2(source, PREFIX / 'lib' / name)
                dependencies[name] = {'origin': str(source), 'sha256': digest(source)}
    metadata = {'compiler_version': subprocess.check_output([clang, '--version'], text=True),
                'compiler_sha256': digest(clang), 'resource_version': resource.name,
                'host': os.uname().machine, 'libraries': dependencies}
    (PREFIX / 'compiler.json').write_text(json.dumps(metadata, indent=2) + '\n')
    for name in ('clang', 'llvm-libs', 'libbsd', 'gcc-libs', 'gcc', 'glibc', 'libedit',
                 'libffi', 'icu', 'ncurses', 'zlib', 'zstd', 'libxml2', 'util-linux-libs'):
        license_dir = Path('/usr/share/licenses') / name
        if license_dir.exists():
            shutil.copytree(license_dir, PREFIX / 'licenses' / name, dirs_exist_ok=True)


def fetch(name, spec):
    cache = TOOLS / 'cache' / spec['archive']
    cache.parent.mkdir(parents=True, exist_ok=True)
    if not cache.exists():
        print(f'Download {name}', flush=True)
        part = cache.with_suffix(cache.suffix + '.part')
        run(['curl', '-fLsS', '--retry', '3', '--connect-timeout', '30', spec['url'], '-o', part])
        part.rename(cache)
    expected = spec.get('sha256')
    actual = digest(cache)
    if expected and actual != expected:
        raise RuntimeError(f'checksum mismatch: {cache}')
    return cache, actual


def unpack(archive, destination):
    if (destination / '.unpacked').exists():
        return
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as f:
        members = f.getmembers()
        for m in members:
            m.name = m.name.split('/', 1)[-1] if '/' in m.name else ''
        f.extractall(destination, members=[m for m in members if m.name], filter='data')
    (destination / '.unpacked').touch()


def aliases():
    BIN.mkdir(parents=True, exist_ok=True)
    for name in ['clang-arm64', 'clang++-arm64', 'xcrun', 'host-cc', 'plutil',
                 'sed', 'install', 'makekdebugevents', 'iig', 'mig']:
        p = BIN / name
        if not p.exists():
            p.symlink_to(os.path.relpath(SELF, BIN))


def mirror_source(url):
    raw = subprocess.check_output(['curl', '-fLsS', '--retry', '3', url], text=True)
    table = raw.split('<table class="code">', 1)[-1].split('</table>', 1)[0]
    rows = re.findall(r'<tr\b[^>]*>.*?<td\b[^>]*>(.*?)</td>\s*</tr>', table, re.S)
    lines = []
    for row in rows:
        row = re.sub(r'<span[^>]*inlayHint[^>]*>.*?</span>', '', row, flags=re.S)
        lines.append(html.unescape(re.sub(r'<[^>]+>', '', row)).replace('\xa0', ' '))
    if not lines:
        raise RuntimeError(f'empty source mirror: {url}')
    return '\n'.join(lines) + '\n'


def fetch_mirrors():
    catalog = TOOLS / 'mirrors.json'
    if not catalog.exists():
        raise RuntimeError('missing source mirror lockfile')
    def fetch_one(item):
        relative, info = item
        path = TOOLS / relative
        if not path.exists():
            text = mirror_source(info['url'])
            if relative.startswith('iig/') and relative.endswith('.cpp'):
                text = text.replace('#include <DriverKit/IOReturn.h>',
                                    '#include <DriverKit/IOReturn.h>\n#include <DriverKit/IORPC.h>')
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        if digest(path) != info['sha256']:
            raise RuntimeError(f'source mirror checksum mismatch: {path}')
    list(concurrent.futures.ThreadPoolExecutor(4).map(fetch_one, json.loads(catalog.read_text()).items()))


def bootstrap(jobs):
    manifest = json.loads(LOCK.read_text())
    def download(item):
        name, spec = item
        return name, fetch(name, spec)
    results = dict(concurrent.futures.ThreadPoolExecutor(4).map(download, manifest.items()))
    for name, spec in manifest.items():
        archive, checksum = results[name]
        spec['sha256'] = checksum
        if name == 'sdk':
            dest = TOOLS / 'sdk'
            if not (dest / 'iPhoneOS18.6.sdk/SDKSettings.plist').exists():
                dest.mkdir(parents=True, exist_ok=True)
                with zipfile.ZipFile(archive) as z:
                    members = [i for i in z.infolist() if i.filename.startswith('iPhoneOS18.6.sdk/')]
                    z.extractall(dest, members=members)
                    for info in members:
                        p = dest / info.filename
                        mode = info.external_attr >> 16
                        if mode & 0o170000 == 0o120000:
                            target = p.read_text()
                            p.unlink()
                            p.symlink_to(target)
        elif name == 'xnu':
            if not (SRC / 'Makefile').exists():
                unpack(archive, SRC)
        else:
            unpack(archive, TOOLS / 'src' / name)
    LOCK.write_text(json.dumps(manifest, indent=2) + '\n')
    fetch_mirrors()
    aliases()
    env = dict(os.environ, CC=host('clang'), CXX=host('clang++'))
    env['PATH'] = str(BIN) + os.pathsep + env['PATH']
    env['LD_LIBRARY_PATH'] = str(PREFIX / 'lib') + os.pathsep + env.get('LD_LIBRARY_PATH', '')
    log = BUILD / 'bootstrap.log'
    for name in ('unifdef', 'tcsh'):
        if (BIN / name).exists():
            continue
        source = TOOLS / 'src' / name
        print(f'Build {name}', flush=True)
        if name == 'unifdef':
            (source / 'version.sh').write_text('V="unifdef-2.12"\nD="2020-02-14"\n')
            run(['make', f'-j{jobs}', f'prefix={PREFIX}', 'install'], cwd=source, log=log, env=env)
        else:
            if not (source / 'configure').exists():
                run(['autoreconf', '-fi'], cwd=source, log=log, env=env)
            work = BUILD / 'host/tcsh'
            work.mkdir(parents=True, exist_ok=True)
            run([source / 'configure', f'--prefix={PREFIX}'], cwd=work, log=log, env=env)
            run(['make', f'-j{jobs}', 'install'], cwd=work, log=log, env=env)
    if not (PREFIX / 'lib/libdispatch.so').exists():
        print('Build host libdispatch', flush=True)
        work = BUILD / 'host/libdispatch'
        run(['cmake', '-S', TOOLS / 'src/host-dispatch', '-B', work, '-G', 'Ninja',
             f'-DCMAKE_INSTALL_PREFIX={PREFIX}', '-DCMAKE_BUILD_TYPE=Release',
             '-DCMAKE_C_FLAGS=-Wno-error=unused-but-set-global',
             '-DBUILD_SHARED_LIBS=ON', '-DENABLE_TESTING=OFF', '-DBUILD_TESTING=OFF',
             '-DENABLE_SWIFT=OFF', '-DUSE_DTRACE=OFF'], log=log, env=env)
        run(['cmake', '--build', work, '-j', jobs], log=log, env=env)
        run(['cmake', '--install', work], log=log, env=env)
    if not (BIN / 'arm-apple-darwin11-ld').exists():
        print('Build cctools/ld64', flush=True)
        source = TOOLS / 'src/cctools-port/cctools'
        work = BUILD / 'host/cctools'
        work.mkdir(parents=True, exist_ok=True)
        run([source / 'configure', '--target=arm-apple-darwin11', f'--prefix={PREFIX}',
             f'--with-llvm-config={host("llvm-config")}', f'--with-libdispatch={PREFIX}',
             f'--with-libblocksruntime={PREFIX}'], cwd=work, log=log, env=env)
        run(['make', f'-j{jobs}'], cwd=work, log=log, env=env)
        run(['make', 'install'], cwd=work, log=log, env=env)
    migcom = PREFIX / 'libexec/migcom'
    if not migcom.exists():
        print('Build MIG', flush=True)
        source = TOOLS / 'src/bootstrap_cmds/migcom.tproj'
        work = BUILD / 'host/mig'
        work.mkdir(parents=True, exist_ok=True)
        migcom.parent.mkdir(parents=True, exist_ok=True)
        run(['bison', '-y', '-d', '-o', work / 'y.tab.c', source / 'parser.y'], log=log)
        run(['flex', '-o', work / 'lex.yy.c', source / 'lexxer.l'], log=log)
        names = 'error global header mig routine server statement string type user utils'.split()
        run([host('clang'), '-std=gnu11', '-O2', '-D_GNU_SOURCE', '-Ulinux',
             '-DMIG_VERSION="mig-138"', f'-I{source}', f'-I{work}', '-idirafter',
             TOOLS / 'src/cctools-port/cctools/include/foreign',
             *[source / (n + '.c') for n in names], work / 'y.tab.c', work / 'lex.yy.c',
             '-o', migcom], log=log)
        driver = (source / 'mig.sh').read_text().replace('`/usr/bin/arch`', '`uname -m`')
        (PREFIX / 'libexec/mig-driver').write_text(driver)
        (PREFIX / 'libexec/mig-driver').chmod(0o755)
    for name in ('libmacho', 'libstuff'):
        source = BUILD / f'host/cctools/{name}/.libs/{name}.a'
        if source.exists():
            shutil.copy2(source, PREFIX / f'lib/{name}.a')
        elif not (PREFIX / f'lib/{name}.a').exists():
            raise RuntimeError(f'missing host archive: {name}')
    if not (PREFIX / 'compiler.json').exists():
        bundle_compiler()
    relocate_elf()
    if not (PREFIX / 'libexec/ld.so').exists():
        bundle_host_runtime()
    print('Toolchain ready', flush=True)


def relocate_elf():
    """Replace install-prefix RUNPATH strings in-place; no patchelf dependency."""
    for path in list(BIN.glob('*')) + list((PREFIX / 'lib').glob('*.so*')):
        if path.is_symlink() or not path.is_file():
            continue
        data = bytearray(path.read_bytes())
        if data[:6] != b'\x7fELF\x02\x01':
            continue
        section_offset = struct.unpack_from('<Q', data, 40)[0]
        section_size, count = struct.unpack_from('<HH', data, 58)
        sections = [struct.unpack_from('<IIQQQQIIQQ', data, section_offset + i * section_size)
                    for i in range(count)]
        for sec in sections:
            if sec[1] != 6:  # SHT_DYNAMIC
                continue
            strings = sections[sec[6]][4]
            for offset in range(sec[4], sec[4] + sec[5], 16):
                tag, value_offset = struct.unpack_from('<QQ', data, offset)
                if tag not in (15, 29):  # DT_RPATH / DT_RUNPATH
                    continue
                pos = strings + value_offset
                end = data.index(0, pos)
                value = bytes(data[pos:end])
                if str(PREFIX).encode() in value:
                    new = b'$ORIGIN/../lib:$ORIGIN:/usr/lib'
                    if len(new) <= len(value):
                        data[pos:end] = new + b'\0' * (len(value) - len(new))
        path.write_bytes(data)


def make_args(jobs):
    values = {
        'SYSCTL_HW_PHYSICALCPU': jobs, 'SYSCTL_HW_LOGICALCPU': jobs,
        'MEMORY_SIZE': 8589934592, 'MAKEJOBS': f'-j{jobs}',
        'SDKROOT': 'iphoneos', 'SDKROOT_RESOLVED': SDK, 'HOST_SDKROOT_RESOLVED': '/',
        'PLATFORM': 'iPhoneOS', 'PLATFORMPATH': BUILD / 'platform/iPhoneOS.platform',
        'SDKVERSION': '18.6', 'RC_DARWIN_KERNEL_VERSION': '24.6.0', 'KDKROOT': '',
        'ARCH_CONFIGS': 'ARM64', 'MACHINE_CONFIGS': TARGET, 'KERNEL_CONFIGS': 'DEVELOPMENT',
        'ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG': 'arm64',
        'XCRUN': BIN / 'xcrun', 'CC': BIN / 'clang-arm64', 'CXX': BIN / 'clang++-arm64',
        'HOST_CC': BIN / 'host-cc', 'MIG': BIN / 'mig', 'MIGCOM': PREFIX / 'libexec/migcom',
        'MIGCC': BIN / 'clang-arm64', 'IIG': BIN / 'iig', 'PLUTIL': BIN / 'plutil',
        'UNIFDEF': BIN / 'unifdef', 'HOST_FLEX': host('flex'), 'HOST_BISON': host('bison'),
        'HOST_GM4': host('m4'), 'HOST_OS_VERSION': '6.0', 'HOST_CODESIGN': host('true'),
        'HOST_CODESIGN_ALLOCATE': host('false'), 'EMBEDDED_DEVICE_MAP': host('false'),
        'CTFCONVERT': host('false'), 'CTFMERGE': host('false'),
        'MAKEKDEBUGEVENTS': BIN / 'makekdebugevents',
        'OBJROOT': OBJ, 'DSTROOT': BUILD / 'dst', 'SYMROOT': BUILD / 'sym',
        'BUILD_WERROR': 0, 'BUILD_LTO': 0, 'BUILD_DSYM': 0, 'DO_CTFMERGE': 0,
        'USE_BINARY_PLIST': 0,
        'CFLAGS_EXTRA': f'-DOS_FIREHOSE_SPI=1 -DXNU_SOFTWARE_PANIC={os.environ.get("XNU_SOFTWARE_PANIC", "1" if TARGET in ("SDM835", "SM8150") else "0")} -I{TOOLS}/private-headers -I{TOOLS}/compat',
    }
    for name in ('strip', 'lipo', 'libtool', 'nm', 'nmedit', 'ctf_insert'):
        values[name.upper().replace('CTF_INSERT', 'CTFINSERT')] = BIN / ('arm-apple-darwin11-' + name)
    return [f'{key}={value}' for key, value in values.items()]


def make_xnu(target, jobs, log, extra=(), check=True):
    env = dict(os.environ)
    env['XNU_MACHINE_CONFIG'] = TARGET
    env['PATH'] = str(BIN) + os.pathsep + env['PATH']
    env['LD_LIBRARY_PATH'] = str(PREFIX / 'lib') + os.pathsep + env.get('LD_LIBRARY_PATH', '')
    return run(['make', '-C', SRC, f'-j{jobs}', target, *make_args(jobs), *extra],
               log=log, check=check, env=env)


def prepare_sdk(jobs):
    marker = BUILD / '.workspace-root'
    workspace_paths = f'{ROOT}\n{SRC}'
    if marker.exists() and marker.read_text().strip() != workspace_paths:
        # Only our regenerable target build directories contain absolute paths.
        for name in ('obj', 'headers', 'sdk', 'runtime', 'sym', 'dst'):
            path = BUILD / name
            if path.exists():
                shutil.rmtree(path)
        for pattern in ('.headers-installed*', '.config-signature*'):
            for stamp in BUILD.glob(pattern):
                stamp.unlink()
    BUILD.mkdir(parents=True, exist_ok=True)
    marker.write_text(workspace_paths + '\n')
    original = TOOLS / 'sdk/iPhoneOS18.6.sdk'
    if not (SDK / 'SDKSettings.plist').exists():
        SDK.parent.mkdir(parents=True, exist_ok=True)
        run(['cp', '-a', '--reflink=auto', str(original) + '/.', SDK])
        run(['chmod', '-R', 'u+w', SDK])
    (SDK / 'usr/local/include/kernel/os').mkdir(parents=True, exist_ok=True)
    (SDK / 'usr/local/lib/kernel').mkdir(parents=True, exist_ok=True)
    (BUILD / 'platform/iPhoneOS.platform').mkdir(parents=True, exist_ok=True)
    for sub in ('include/os', 'private/os'):
        shutil.copytree(TOOLS / 'src/libplatform' / sub, SDK / 'usr/local/include/os', dirs_exist_ok=True)
    shutil.copytree(TOOLS / 'src/libdispatch/os', SDK / 'usr/local/include/os', dirs_exist_ok=True)
    shutil.copy2(TOOLS / 'src/libdispatch/os/firehose_buffer_private.h',
                 SDK / 'usr/local/include/kernel/os/firehose_buffer_private.h')
    availability = TOOLS / 'src/AvailabilityVersions/availability.pl'
    (SDK / 'usr/local/libexec').mkdir(parents=True, exist_ok=True)
    if availability.exists() and not availability.read_text().startswith('#!/bin/sh'):
        shutil.copy2(availability, SDK / 'usr/local/libexec/availability.pl')
    else:
        source = availability.with_name('availability')
        run([host('python3'), source, '--av_version', '151', '--preprocess', source,
             SDK / 'usr/local/libexec/availability.pl'])
        (SDK / 'usr/local/libexec/availability.pl').chmod(0o755)
    other_source_patches = set()
    for dependency in ('apfs-tools.json', 'usb-storage.json'):
        specification = TOOLS / dependency
        if specification.exists():
            other_source_patches.update(json.loads(specification.read_text()).get('patches', []))
    other_source_patches.update({'apfsprogs-ios18-native-metadata.patch',
                                'go-apfs-sealed-readonly.patch'})
    # Later patches intentionally modify earlier patches. Reconstruct the whole
    # recipe in isolation and recognize the current prefix before any writes.
    from kernel_patch_stack import ensure_kernel_patch_stack, order_kernel_patches
    patches = order_kernel_patches([patch for patch in (TOOLS / 'patches').glob('*.patch')
                                   if patch.name not in other_source_patches])
    specification = json.loads(LOCK.read_text())['xnu']
    ensure_kernel_patch_stack(SRC, patches, TOOLS / 'cache' / specification['archive'],
                              specification['sha256'], BUILD / 'patches.log')
    # Upstream DOCONFDEPS omits platform MASTER files. Explicitly invalidate
    # generated configuration makefiles when that input changes.
    signature = digest(SRC / 'config/MASTER.arm64.iPhoneOS')
    stamp = BUILD / ('.config-signature-' + TARGET.lower())
    if not stamp.exists() or stamp.read_text().strip() != signature:
        for path in CONFIG.glob('*/DEVELOPMENT/Makefile'):
            path.unlink()
        stamp.write_text(signature + '\n')
    headers_stamp = BUILD / ('.headers-installed-' + TARGET.lower())
    if not headers_stamp.exists():
        make_xnu('installhdrs', jobs, BUILD / 'headers.log', [f'DSTROOT={BUILD}/headers'])
        run(['cp', '-a', '--remove-destination', '--reflink=auto',
             str(BUILD / 'headers') + '/.', SDK], log=BUILD / 'headers.log')
        headers_stamp.touch()


def build_kernel(jobs):
    aliases()
    fetch_mirrors()
    prepare_sdk(jobs)
    build_runtime()
    panic_mode = os.environ.get('XNU_SOFTWARE_PANIC', '1' if TARGET in ('SDM835', 'SM8150') else '0')
    if panic_mode not in ('0', '1'):
        raise RuntimeError('XNU_SOFTWARE_PANIC must be 0 or 1')
    panic_stamp = BUILD / ('.software-panic-' + TARGET.lower())
    if not panic_stamp.exists() or panic_stamp.read_text().strip() != panic_mode:
        # Only the panic character sink changes. Invalidate it when toggling the macro.
        (CONFIG / 'osfmk/DEVELOPMENT/printf.o').unlink(missing_ok=True)
        panic_stamp.write_text(panic_mode + '\n')
    # IIG products are needed both by header export and the configured IOKit
    # Makefile, whose generated-file paths are relative to DEVELOPMENT/.
    for definition in sorted((SRC / 'iokit/DriverKit').glob('*.iig')):
        for directory in (CONFIG / 'iokit/DriverKit', CONFIG / 'iokit/DEVELOPMENT/DriverKit'):
            iig(['--def', str(definition), '--header', str(directory / (definition.stem + '.h')),
                 '--impl', str(directory / (definition.stem + '.iig.cpp'))])
    log = BUILD / ('kernel-' + TARGET.lower() + '.log')
    log.write_text('')
    make_xnu('build', jobs, log)
    output = BUILD / 'output'
    output.mkdir(parents=True, exist_ok=True)
    for suffix in ('', '.unstripped'):
        source = CONFIG / (KERNEL_FILE + suffix)
        if not source.exists():
            raise RuntimeError(f'expected kernel missing: {source}')
        shutil.copy2(source, output / source.name)
    verify_kernel()


def verify_kernel():
    results = []
    for suffix in ('', '.unstripped'):
        path = BUILD / 'output' / (KERNEL_FILE + suffix)
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
        if TARGET == 'SM8150':
            required -= {'_qcom_uartdm_init', '_qcom_uartdm_transmit_data',
                         '_qcom_uartdm_receive_ready', '_qcom_uartdm_receive_data'}
            required |= {'_ml_usb_console_read_tx', '_ml_usb_console_write_rx',
                         '_ml_usb_console_set_enabled'}
        if TARGET in ('SDM835', 'SM8150') and suffix == '.unstripped' and not required <= defined_names:
            raise RuntimeError(f'{TARGET} platform symbols absent: {sorted(required - defined_names)}')
        results.append({'file': path.name, 'sha256': digest(path), 'bytes': len(data),
                        'architecture': 'arm64', 'platform': 'iPhoneOS', 'sdk': '18.6',
                        'entry': hex(entry), 'undefined_symbols': undefined, 'segments': segments,
                        'platform_symbols_verified': sorted(required) if TARGET in ('SDM835', 'SM8150') and suffix == '.unstripped' else []})
    report = {'xnu': '11417.140.69', 'config': 'DEVELOPMENT_ARM64_' + TARGET, 'kernels': results,
              'hardware_boot_verified': False}
    (BUILD / ('output/verification-' + TARGET.lower() + '.json')).write_text(json.dumps(report, indent=2) + '\n')
    (BUILD / 'output/verification.json').write_text(json.dumps(report, indent=2) + '\n')
    for r in results:
        print(f'{r["file"]}: arm64 / iPhoneOS 18.6 / {r["bytes"]} bytes / no undefined symbols')
        print(f'SHA256 {r["sha256"]}')


def build_runtime():
    work = BUILD / 'runtime' / TARGET.lower()
    work.mkdir(parents=True, exist_ok=True)
    library = SDK / 'usr/local/lib/kernel' / TARGET.lower()
    library.mkdir(parents=True, exist_ok=True)
    arch_flag = '-march=armv8-a+crc+crypto' if TARGET in ('SDM835', 'SM8150') else '-march=armv8.5a+sme2'
    builtins = TOOLS / 'src/compiler-rt/lib/builtins'
    objects = []
    names = 'udivti3 divti3 umodti3 modti3 udivmodti4 multi3 ashlti3 ashrti3 lshrti3 clzti2 clzdi2 ctzti2 ctzdi2 ucmpti2 cmpti2 muloti4'.split()
    for name in names:
        source, output = builtins / (name + '.c'), work / (name + '.o')
        if not output.exists() or source.stat().st_mtime > output.stat().st_mtime:
            run([BIN / 'clang-arm64', '-c', source, '-o', output, '-O2', '-mkernel',
                 '-fno-builtin', '-fno-stack-protector', arch_flag, '-isysroot', SDK], log=BUILD / 'runtime.log')
        objects.append(output)
    source, output = TOOLS / 'compat/kernel-runtime.c', work / 'kernel-runtime.o'
    run([BIN / 'clang-arm64', '-c', source, '-o', output, '-O2', '-mkernel',
         '-fno-builtin', '-fno-stack-protector', arch_flag, '-isysroot', SDK], log=BUILD / 'runtime.log')
    run([BIN / 'arm-apple-darwin11-libtool', '-static', '-o', library / 'libcc_kext.a',
         *objects, output], log=BUILD / 'runtime.log')
    source, output = TOOLS / 'src/libdispatch/src/firehose/firehose_buffer.c', work / 'firehose.o'
    flags = ['-c', source, '-o', output, '-O2', '-g', '-mkernel', '-nostdlibinc',
             '-fno-builtin', '-fno-common', '-fno-strict-aliasing', arch_flag,
             '-DKERNEL', '-DPRIVATE', '-DKERNEL_PRIVATE', '-DXNU_KERNEL_PRIVATE',
             '-D__MACHO__=1', '-D__API__=v4', '-D__MACH30__', '-DARM64_BOARD_CONFIG_' + TARGET,
             '-DARM64', '-D__ARM64__', '-DLP64', '-D__ARM_ARCH__=8', '-DDISPATCH_USE_DTRACE=0',
             '-DOS_ATOMIC_CONFIG_MEMORY_ORDER_DEPENDENCY=1', '-DOS_ATOMIC_CONFIG_STARVATION_FREE_ONLY=0',
             f'-I{TOOLS}/src/libdispatch', f'-I{TOOLS}/src/libdispatch/src/firehose',
             f'-I{SRC}/EXTERNAL_HEADERS', f'-I{SDK}/usr/local/include/kernel']
    for name in ('bsd', 'osfmk', 'libkern', 'libkern/firehose', 'iokit', 'pexpert', 'libsa', 'security', 'san'):
        flags.append(f'-I{OBJ}/EXPORT_HDRS/{name}')
    run([BIN / 'clang-arm64', *flags], log=BUILD / 'runtime.log')
    run([BIN / 'arm-apple-darwin11-libtool', '-static', '-o', library / 'libfirehose_kernel.a', output],
        log=BUILD / 'runtime.log')


def iig_kernel_call_content(text):
    """Adapt cached IIG callers to the current kernel's local RPC content ABI."""
    def initialize(match):
        declaration = match.group(0)
        if '.kernelContent' in declaration:
            return declaration
        # These are the two pinned cache layouts: synchronous and one-way.
        # Use the generated content directly, without reinterpreting Mach ports.
        if not re.fullmatch(
                r'    IORPC rpc = \{ \.message = &buf\.msg\.mach, '
                r'\.reply = (?:&buf\.rpl\.rpl\.mach|NULL), '
                r'\.sendSize = (?:sizeof\(buf\.msg\)|sizeof\(\*msg\)), '
                r'\.replySize = (?:sizeof\(buf\.rpl\)|0) \};', declaration):
            raise RuntimeError('unrecognized cached IIG caller layout')
        return (declaration + '\n#if KERNEL\n'
                '    rpc.kernelContent = &buf.msg.content.__hdr;\n#endif')
    return re.sub(r'^    IORPC rpc = \{[^;]+\};'
                  r'(?!\n#if KERNEL\n    rpc\.kernelContent = &buf\.msg\.content\.__hdr;\n#endif)',
                  initialize, text, flags=re.MULTILINE)


def iig(args):
    def option(key):
        return Path(args[args.index(key) + 1]) if key in args else None
    definition, header, impl = option('--def'), option('--header'), option('--impl')
    if not definition or not header:
        raise RuntimeError('iig requires --def and --header')
    cache = TOOLS / 'iig'
    cache.mkdir(parents=True, exist_ok=True)
    for output, suffix in ((header, '.h'), (impl, '.iig.cpp')):
        if output is None:
            continue
        filename = definition.stem + suffix
        saved = cache / filename
        if not saved.exists():
            url = 'https://fergofrog.com/code/codebrowser/xnu-build/RELEASE_ARM64_VMAPPLE/iokit/RELEASE/DriverKit/' + filename + '.html'
            text = mirror_source(url)
            if 'IIG' not in text and 'iig' not in text:
                raise RuntimeError(f'invalid IIG cache: {url}')
            if suffix == '.iig.cpp':
                text = text.replace('#include <DriverKit/IOReturn.h>', '#include <DriverKit/IOReturn.h>\n#include <DriverKit/IORPC.h>')
            saved.write_text(text)
        info = json.loads((TOOLS / 'mirrors.json').read_text()).get('iig/' + filename)
        if info and (digest(saved) != info['sha256'] or digest(definition) != info['input_sha256']):
            raise RuntimeError(f'IIG cache/input changed; regenerate matching output: {definition}')
        output.parent.mkdir(parents=True, exist_ok=True)
        text = saved.read_text()
        if definition.stem == 'IOMemoryDescriptor' and suffix == '.h':
            text = text.replace('kIOMemoryMapCacheModeWriteThrough = 0x00000400,',
                                'kIOMemoryMapCacheModeWriteThrough = 0x00000400,\n'
                                '\tkIOMemoryMapCacheModeRealTime = 0x00000800,')
        if definition.stem == 'IOService' and suffix == '.h':
            text = text.replace('#define IOService_KernelMethods \\\n',
                                '#define IOService_KernelMethods \\\n'
                                'public: kern_return_t SetPowerOverride_Impl(bool enable); \\\n')
        if suffix == '.iig.cpp':
            text = iig_kernel_call_content(text)
            text = text.replace('#include <DriverKit/IORPC.h>',
                                '#include <DriverKit/IORPC.h>\n'
                                '#if KERNEL\n#include <iig-kernel-rpc.h>\n#endif')
            text = text.replace('IORPCMessageFromMach(rpc.message, false);',
                                'xnu_iig_message_from_mach(rpc.message, false, rpc.sendSize);\n'
                                '    if (!msg) return kIOReturnIPCError;')
            for rpc in ('rpc', 'rpc.rpc'):
                text = text.replace(f'IORPCMessageFromMach({rpc}.reply, false);',
                                    f'xnu_iig_message_from_mach({rpc}.reply, false, {rpc}.replySize);\n'
                                    '    if (!message) return kIOReturnIPCError;')
        if saved.resolve() != output.resolve():
            if not output.exists() or output.read_text() != text:
                output.write_text(text)


def shim(name, args):
    native = PREFIX / 'libexec/native' / name
    if native.is_file():
        exec_tool(native, args)
    if name == 'iig':
        iig(args)
        return
    if name in ('clang-arm64', 'clang++-arm64'):
        new = []
        for arg in args:
            if arg == '-fno-branch-target-identification':
                new.append('-mbranch-protection=none')
            elif arg in ('-lcc_kext', '-lfirehose_kernel'):
                new.append(str(SDK / 'usr/local/lib/kernel' / TARGET.lower() / ('lib' + arg[2:] + '.a')))
            else:
                new.append(arg)
        os.environ['LD_LIBRARY_PATH'] = str(PREFIX / 'lib') + os.pathsep + os.environ.get('LD_LIBRARY_PATH', '')
        modes = ['--driver-mode=g++'] if '++' in name else []
        exec_tool(compiler('++' in name),
                  [*modes, '-no-canonical-prefixes', '--target=arm64-apple-ios18.6', f'-fuse-ld={BIN}/arm-apple-darwin11-ld',
                   *compiler_resource(),
                   *new, '-Wno-unknown-warning-option', '-Wno-unused-command-line-argument',
                   f'-I{TOOLS}/compat', f'-I{TOOLS}/private-headers',
                   '-Wno-shadow-header', '-Wno-unsafe-buffer-usage', '-Wno-reserved-identifier',
                   '-Wno-pre-c11-compat', '-Wno-deprecated-non-prototype', '-fheinous-gnu-extensions'])
    if name == 'mig':
        os.environ['MIGCOM'] = str(PREFIX / 'libexec/migcom')
        os.environ['MIGCC'] = str(BIN / 'clang-arm64')
        exec_tool(PREFIX / 'libexec/mig-driver', args)
    if name == 'host-cc':
        base = TOOLS / 'src/cctools-port/cctools'
        flags = ['-D__DARWIN_UNIX03', '-DHAVE_BCMP', '-DHAVE_BZERO', '-DHAVE_BCOPY',
                 '-DHAVE_INDEX', '-DHAVE_RINDEX', '-D__private_extern__=extern', '-DO_SYMLINK=0',
                 '-include', 'bsd/stdlib.h', '-include', 'bsd/string.h', '-include', 'bsd/unistd.h',
                 f'-I{TOOLS}/compat', f'-I{base}/include', f'-I{base}/include/foreign']
        if '-c' not in args:
            args += [str(PREFIX / 'lib/libmacho.a'), str(PREFIX / 'lib/libstuff.a'),
                     '-lbsd', '-luuid', '-lz', '-ldl']
        os.environ['LD_LIBRARY_PATH'] = str(PREFIX / 'lib') + os.pathsep + os.environ.get('LD_LIBRARY_PATH', '')
        exec_tool(compiler(), ['-no-canonical-prefixes', *compiler_resource(), *flags, *args])
    if name == 'install':
        exec_tool(host('install'), [a for a in args if a != '-S'])
    if name == 'sed':
        out = subprocess.check_output([host('sed'), *args])
        if any('genassym.o' in a for a in args):
            out = re.sub(rb'\s*DEFINITION__define__(\w+):\s*\.byte\s+(\d+)\s*\n',
                         lambda m: b'\n#define ' + m[1] + b' ' + bytes([int(m[2])]) + b'\n', out)
        sys.stdout.buffer.write(out)
        return
    if name == 'plutil':
        with open(args[-1], 'rb') as f:
            value = plistlib.load(f)
        if args[0] == '-extract':
            for key in args[1].split('.'):
                value = value[key]
            print(value)
        elif args[0] == '-convert':
            output = args[args.index('-o') + 1] if '-o' in args else args[-1]
            with open(output, 'wb') as f:
                plistlib.dump(value, f, fmt=plistlib.FMT_BINARY)
        elif args[0] != '-lint':
            raise RuntimeError(f'unsupported plutil: {args}')
        return
    if name == 'makekdebugevents':
        events = re.findall(r'^0x([0-9a-fA-F]+)\s+(\S+)', Path(args[0]).read_text(), re.M)
        print('typedef struct { uint32_t id; const char *name; } kd_event_t;\nkd_event_t kd_events[] = {')
        for number, label in sorted(events, key=lambda e: int(e[0], 16)):
            print(f'{{0x{number}, "{label}"}},')
        print('};')
        return
    if name == 'xcrun':
        if args[:1] == ['-sdk']:
            args = args[2:]
        if args == ['-show-sdk-path']:
            print(SDK)
        elif args == ['-show-sdk-version']:
            print('18.6')
        elif args == ['-show-sdk-platform-path']:
            print(BUILD / 'platform/iPhoneOS.platform')
        elif args[:1] == ['-find']:
            tool = args[1]
            mapped = {'clang': 'clang-arm64', 'cc': 'clang-arm64', 'clang++': 'clang++-arm64',
                      'c++': 'clang++-arm64', 'gm4': 'm4'}
            tool = mapped.get(tool, tool)
            if tool in ('strip', 'lipo', 'libtool', 'nm', 'nmedit', 'otool', 'ctf_insert', 'ranlib'):
                print(BIN / ('arm-apple-darwin11-' + tool))
            elif tool == 'migcom':
                print(PREFIX / 'libexec/migcom')
            elif (BIN / tool).exists():
                print(BIN / tool)
            elif tool in ('embedded_device_map', 'ctfconvert', 'ctfmerge', 'ctfdump', 'codesign_allocate', 'dsymutil', 'docc'):
                print(host('false'))
            else:
                print(host(tool))
        else:
            raise RuntimeError(f'unsupported xcrun: {args}')
        return
    raise RuntimeError(f'unknown tool alias: {name}')


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
        (ROOT/'drivers/power/QcomRPM/RPMRails.h').read_text())}
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
    source=Path(source)
    model=json.loads((ROOT/'docs/bluetooth/ios18-model.json').read_text())
    record=model['user_space']['files']['/System/Library/LaunchDaemons/com.apple.bluetoothd.plist']
    if digest(source)!=record['sha256']:
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
    from macho_development_sign import verify_macho
    from bridge_trustcache import development_module, module_info
    reference,output=Path(reference),Path(output)
    model=json.loads((ROOT/'docs/bluetooth/ios18-model.json').read_text())['user_space']['files']
    # Fail closed on another OS revision or any modified native input.
    for source,record in model.items():
        if digest(reference/Path(source).name) != record['sha256']:
            raise ValueError('unsupported Bluetooth system source '+source)
    daemon=prepare_bluetooth_daemon(reference/'com.apple.bluetoothd.plist')
    blue=plistlib.loads((reference/'com.apple.BlueTool.plist').read_bytes())
    if blue != model['/System/Library/LaunchDaemons/com.apple.BlueTool.plist']['plist']:
        raise ValueError('unsupported BlueTool launchd contract')
    blue['ProgramArguments']=['/usr/libexec/bluetoothbootstrap','-R']
    binary=BUILD/'output/bluetoothbootstrap'
    report=json.loads(Path(str(binary)+'.json').read_text())
    if digest(binary) != report['sha256']:
        raise ValueError('modified Bluetooth backend')
    for path,sha in report['inputs'].items():
        if digest(ROOT/path) != sha:raise ValueError('stale Bluetooth backend; rebuild '+path)
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
    from macho_development_sign import verify_macho
    from bridge_trustcache import development_module, module_info
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
                path.stat().st_size != record['bytes'] or digest(path) != record['sha256']:
            raise ValueError('modified Bluetooth stack file '+name)
    model=json.loads((ROOT/'docs/bluetooth/ios18-model.json').read_text())['user_space']['files']
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
        if Path(path).is_absolute() or '..' in Path(path).parts or digest(ROOT/path) != sha:
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
    archive, profile_path, output = Path(archive), Path(profile_path), Path(output)
    profile = json.loads(profile_path.read_text())
    if digest(archive) != profile['archive_sha256']:
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
    manifest={'version':1,'archive_sha256':digest(archive),'nvm_transport_changes':changes,
              'files':{n:{'sha256':hashlib.sha256(b).hexdigest(),'bytes':len(b)} for n,b in files.items()}}
    pending=output/'manifest.pending';pending.write_text(json.dumps(manifest,indent=2)+'\n');pending.replace(output/'manifest.json')
    return manifest


def configure_bluetooth_firmware(tree, directory):
    """Add one complete Bluetooth policy to an offline FDT tree atomically."""
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
        if path.stat().st_size != record['bytes'] or record['bytes'] > 0x100000 or digest(path) != record['sha256']:
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
    return {'manifest_sha256':digest(directory/'manifest.json'),'controller':profile['controller'],
            'native_launchd_prepared':'com.apple.bluetoothd.plist' in files,
            'native_launchd_installed':False,'hardware_tested':False}


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
    reference = ROOT / 'docs/usb/reference'
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
        sources.append({'file': str(path.relative_to(ROOT)), 'url': manifest[name]['url'], 'sha256': actual})
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


def main():
    global TARGET, CONFIG, KERNEL_FILE, NATIVE_KERNELCACHE, DRIVERS_ROOT, DRIVER_FILTERS, DRIVER_OUTPUT
    name = Path(sys.argv[0]).name
    if name != 'toolchain.py':
        shim(name, sys.argv[1:])
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['analyze-baseband', 'prepare-baseband', 'bootstrap', 'build', 'verify', 'doctor', 'package', 'test', 'test-platform', 'collect-sdm835',
                                          'build-drivers', 'verify-drivers', 'test-drivers', 'pack-drivers', 'verify-collection', 'collect-power', 'build-power-client', 'build-usb-client', 'build-battery-client', 'build-haptics-client', 'build-input-client', 'build-wifi-client', 'prepare-wifi', 'build-wifi-driverkit', 'verify-wifi-driverkit', 'build-bluetooth-client', 'prepare-bluetooth', 'prepare-bluetooth-stack', 'verify-bluetooth-stack', 'build-display-client', 'build-wallpaper-client', 'build-gyro-client', 'prepare-display-services', 'prepare-keybag-policy', 'verify-display-readback', 'build-gpu-client', 'build-gpu-metal', 'build-gpu-shaders', 'prepare-input', 'analyze-usb-resources'])
    parser.add_argument('-j', '--jobs', type=int, default=JOBS)
    parser.add_argument('--target', choices=['SDM835', 'SM8150', 'VMAPPLE'], default=TARGET)
    parser.add_argument('--kernelcache', '--provider-image', type=Path,
                        help='external provider image for the configured ABI contracts')
    parser.add_argument('--drivers-root', type=Path, default=DRIVERS_ROOT,
                        help='Kext source root; discovers child directories containing Info.plist')
    parser.add_argument('--driver', action='append', default=[],
                        help='select a source directory, executable name or bundle identifier; repeatable')
    parser.add_argument('--driver-output', type=Path, default=DRIVER_OUTPUT,
                        help='destination directory for compiled Kext bundles')
    parser.add_argument('--test', action='append', default=None,
                        help='select a configured host test by name; repeatable')
    parser.add_argument('--sanitize', action='store_true',
                        help='enable host ASan/UBSan for test-drivers (kernel compile checks stay native)')
    parser.add_argument('--collection', help='collection name declared in the driver build configuration')
    parser.add_argument('--output', type=Path, help='override the configured collection output path')
    parser.add_argument('--local-kernel', type=Path,
                        help='unstripped local kernel for pack-drivers; snapshot this exact build')
    parser.add_argument('--capture-prefix', type=Path, help='host prefix of exported displayctl validate-scanout files')
    parser.add_argument('--migration-service', type=Path, help='original pinned iOS18 datamigrator executable')
    parser.add_argument('--system-app-migrator', type=Path, help='original pinned iOS18 system-app migration bundle executable')
    parser.add_argument('--springboard-migrator', type=Path, help='original pinned iOS18 SpringBoard migration bundle executable')
    parser.add_argument('--service-resource-fork', '--migration-resource-fork', dest='migration_resource_fork', type=Path,
                        help='optional original zlib resource fork for an allocation-preserving service update')
    parser.add_argument('--service-decmpfs', '--migration-decmpfs', dest='migration_decmpfs', type=Path,
                        help='original compression attribute accompanying --service-resource-fork')
    parser.add_argument('--development-skip-migration-unlock', action='store_true',
                        help='prepare a reversible native-service derivative that skips migration unlock on negative MKB query results')
    parser.add_argument('--development-fail-fast-system-app-installs', action='store_true',
                        help='fail missing-app placeholders and use an immediate install-group deadline; retain native failure paths')
    parser.add_argument('--development-defer-launch-images', action='store_true',
                        help='defer native launch-image discovery and capture through the original post-migration request')
    parser.add_argument('--fairplay-launchd-plist', type=Path, help='original pinned iOS18 FairPlay launchd plist')
    parser.add_argument('--development-no-sep-service-readiness', action='store_true',
                        help='reset failed FairPlay Mach endpoints at close on development devices without SEP')
    parser.add_argument('--system-cryptex', type=Path, help='original raw SystemOS Cryptex for the pinned public MKB policy')
    parser.add_argument('--development-no-sep', action='store_true',
                        help='explicitly prepare public MKB data availability for unprotected Data without SEP')
    parser.add_argument('--launchd-cache', type=Path, help='current iOS18 cached launchd configuration')
    parser.add_argument('--launchd-cache-signature', type=Path, help='matching engineering detached launchd cache signature')
    parser.add_argument('--base-manifest', type=Path, help='additional bundle IDs from a kexts.txt manifest')
    parser.add_argument('--firmware-archive', type=Path, help='pinned input firmware archive for prepare-input')
    parser.add_argument('--firmware-directory', type=Path,
                        help='local stock firmware directory for offline USB resource extraction')
    parser.add_argument('--firmware-profile', type=Path, help='explicit controller identity, asset hashes and board policy')
    parser.add_argument('--bluetoothd-plist', type=Path, help='optional hash-pinned native launchd plist; prepare an offline H4/POSIX transport configuration')
    parser.add_argument('--bluetooth-reference',type=Path,default=BUILD/'bluetooth/reference',help='hash-pinned native BlueTool/bluetoothd and launchd inputs')
    args = parser.parse_args()
    TARGET = args.target
    CONFIG = OBJ / ('DEVELOPMENT_ARM64_' + TARGET)
    KERNEL_FILE = 'mach.development.' + TARGET.lower()
    NATIVE_KERNELCACHE = args.kernelcache.resolve() if args.kernelcache else None
    DRIVERS_ROOT = args.drivers_root.resolve()
    DRIVER_FILTERS = args.driver
    DRIVER_OUTPUT = args.driver_output.resolve()
    if args.jobs < 1:
        parser.error('jobs must be positive')
    if args.sanitize and args.command != 'test-drivers':
        parser.error('--sanitize is only supported with test-drivers')
    if args.command == 'bootstrap':
        bootstrap(args.jobs)
    elif args.command in ('analyze-baseband', 'prepare-baseband'):
        import baseband
        report = (baseband.analyze(ROOT, NATIVE_KERNELCACHE, args.output)
                  if args.command == 'analyze-baseband' else baseband.prepare(ROOT, args.output))
        print(json.dumps(report, indent=2))
    elif args.command == 'doctor':
        print(f'Compiler: {compiler()}')
        print(f'Host: {os.uname().machine}; SDK: {SDK}')
        for name in ('make', 'bison', 'flex', 'm4', 'curl', 'patch', 'perl', 'bash', 'sed', 'awk', 'install'):
            print(f'{name}: {host(name)}')
    elif args.command == 'build':
        build_kernel(args.jobs)
    elif args.command == 'package':
        package()
    elif args.command == 'test':
        test_platform()
        test_drivers()
    elif args.command == 'test-platform':
        test_platform()
    elif args.command == 'collect-sdm835':
        collect_sdm835()
    elif args.command == 'collect-power':
        collect_power_clocks()
    elif args.command == 'analyze-usb-resources':
        if not args.firmware_directory or not args.output:
            parser.error('analyze-usb-resources requires --firmware-directory and --output')
        result = analyze_usb_resources(args.firmware_directory, args.output)
        print(json.dumps({'output': str(args.output.resolve()),
            'SMMU_devices': len(result['hyp_static_namespace']['devices']),
            'USB_assignment_matches': len(result['hyp_static_namespace']['reference_usb_matches']),
            'initial_AP_proxy_votes': sum(v['ee'] == 0 for v in result['rpm_initial_proxy_votes']['votes']),
            'complete_AP_baseline': False,
            'production_profile_created': False, 'hardware_tested': False}))
    elif args.command == 'prepare-input':
        from input_firmware import prepare_input_firmware
        prepared = prepare_input_firmware(args.output or BUILD/'input/firmware', args.firmware_archive)
        print(json.dumps(prepared, indent=2))
    elif args.command == 'prepare-bluetooth':
        if not args.firmware_archive or not args.firmware_profile:
            parser.error('prepare-bluetooth requires --firmware-archive and --firmware-profile')
        print(json.dumps(prepare_bluetooth_firmware(args.firmware_archive,args.firmware_profile,
            args.output or BUILD / 'bluetooth/firmware',args.bluetoothd_plist),indent=2))
    elif args.command == 'build-usb-client':
        build_iokit_client('usbctl', ['usbctl/usbctl.cpp'],
            headers=['include/IOKitUser.h'], adhoc_sign=True,
            extra_exports=['IORegistryEntryFromPath', 'IORegistryEntryGetRegistryEntryID',
                           'IORegistryEntryCreateCFProperty', 'IOObjectConformsTo',
                           'IORegistryCreateIterator', 'IORegistryEntryGetName',
                           'IORegistryEntrySetCFProperties'])
    elif args.command == 'build-power-client':
        build_power_client()
    elif args.command == 'build-input-client':
        build_iokit_client('inputctl', ['inputctl/inputctl.mm'],
            headers=['include/IOKitUser.h'],
            extra_exports=['IORegistryEntryGetRegistryEntryID','IORegistryEntryCreateCFProperty',
                'IOHIDEventSystemClientCreateWithType','IOHIDEventSystemClientRegisterEventCallback',
                'IOHIDEventSystemClientScheduleWithRunLoop','IOHIDEventSystemClientUnscheduleWithRunLoop',
                'IOHIDEventSystemClientCopyServices','IOHIDEventSystemClientCopyProperty','IOHIDServiceClientCopyProperty',
                'IOHIDServiceClientGetRegistryID','IOHIDEventGetType','IOHIDEventGetSenderID',
                'IOHIDEventGetTimeStamp','IOHIDEventGetIntegerValue','IOHIDEventGetFloatValue','IOHIDEventGetChildren',
                'IOHIDEventCreateDigitizerEvent','IOHIDEventCreateDigitizerFingerEvent',
                'IOHIDEventSetIntegerValue','IOHIDEventSetFloatValue','IOHIDEventSetSenderID',
                'IOHIDEventAppendEvent','IOHIDEventSystemClientDispatchEvent'],
            frameworks=['Foundation'], entitlements={'com.apple.private.hid.client.event-monitor':True,
                'com.apple.private.hid.client.admin':True,'com.apple.private.hid.client.debug-tool':True,
                'com.apple.private.hid.client.event-dispatch':True,
                'com.apple.hid.system.user-access-service':True},
            signing_identifier='org.darwin.development.inputctl')
    elif args.command == 'build-battery-client':
        build_iokit_client('batteryctl', ['batteryctl/batteryctl.cpp'],
            extra_exports=['IORegistryEntryCreateCFProperties', 'IOPSCopyPowerSourcesInfo',
                           'IOPSCopyPowerSourcesList', 'IOPSGetPowerSourceDescription'],
            headers=['include/IOKitUser.h'], adhoc_sign=True)
    elif args.command == 'build-haptics-client':
        build_iokit_client('hapticsctl', ['hapticsctl/hapticsctl.cpp'],
            headers=['include/IOKitUser.h'], protocol=['HapticsProtocol.h'], adhoc_sign=True)
    elif args.command == 'build-wifi-client':
        build_iokit_client('wifictl', ['wifictl/wifictl.cpp'],
            headers=['include/IOKitUser.h'], protocol=['WLANProtocol.h'], adhoc_sign=True)
    elif args.command == 'prepare-wifi':
        if bool(args.firmware_archive)!=bool(args.firmware_profile):
            parser.error('Wi-Fi firmware preparation requires both --firmware-archive and --firmware-profile')
        prepared={'driverkit':prepare_wifi_driverkit(),'firmware_assets':'not-provided'}
        if args.firmware_profile:
            prepared['firmware_assets']=prepare_wifi_firmware(args.firmware_archive,args.firmware_profile,
                args.output or BUILD/'wifi/firmware')
        print(json.dumps(prepared,indent=2))
    elif args.command == 'build-wifi-driverkit':
        build_wifi_driverkit()
    elif args.command == 'verify-wifi-driverkit':
        print(json.dumps(verify_wifi_driverkit(),indent=2))
    elif args.command == 'build-bluetooth-client':
        build_iokit_client('bluetoothctl', ['bluetoothctl/bluetoothctl.cpp', '../drivers/bluetooth/userspace/IOKitBluetoothTransport.cpp'],
            headers=['include/IOKitUser.h', '../drivers/bluetooth/userspace/IOKitBluetoothTransport.h'],
            include_dirs=[ROOT / 'drivers/bluetooth/userspace'], protocol=['BluetoothProtocol.h'], adhoc_sign=True)
        xpc=(TOOLS/'sdk/iPhoneOS18.6.sdk/usr/lib/system/libxpc.tbd').read_text()
        if not re.search(r'\b_xpc_connection_copy_entitlement_value\b',xpc):
            raise RuntimeError('SDK lacks audited XPC peer entitlement SPI')
        build_iokit_client('bluetoothbootstrap',['../drivers/bluetooth/userspace/BlueToolService.cpp',
            '../drivers/bluetooth/userspace/IOKitBluetoothTransport.cpp'],
            headers=['include/IOKitUser.h','../drivers/bluetooth/userspace/IOKitBluetoothTransport.h',
                     '../drivers/bluetooth/userspace/BlueToolBootstrap.h','../drivers/bluetooth/QcomBluetooth/BluetoothControl.h',
                     '../drivers/bluetooth/QcomBluetooth/BluetoothFirmware.h','../drivers/bluetooth/QcomBluetooth/BluetoothH4.h'],
            protocol=['BluetoothProtocol.h'], include_dirs=[ROOT/'drivers/bluetooth/userspace',DRIVERS_ROOT/'bluetooth/QcomBluetooth'],
            blocks=True,entitlements={'org.darwin.bluetooth.bootstrap':True},
            signing_identifier='org.darwin.development.bluetoothbootstrap')
    elif args.command == 'prepare-bluetooth-stack':
        print(json.dumps(prepare_bluetooth_stack(args.bluetooth_reference,args.output or BUILD/'output/Bluetooth'),indent=2))
    elif args.command == 'verify-bluetooth-stack':
        print(json.dumps(verify_bluetooth_stack(args.output or BUILD/'output/Bluetooth'),indent=2))
    elif args.command == 'verify-display-readback':
        if not args.capture_prefix:
            parser.error('verify-display-readback requires --capture-prefix')
        print(json.dumps(verify_display_readback(args.capture_prefix), indent=2))
    elif args.command == 'prepare-keybag-policy':
        if not args.system_cryptex or not args.output or not args.development_no_sep:
            parser.error('prepare-keybag-policy requires --system-cryptex, --output and --development-no-sep')
        from keybag_policy import prepare
        print(json.dumps(prepare(args.system_cryptex, args.output), indent=2))
    elif args.command == 'prepare-display-services':
        policies = [(args.migration_service, args.development_skip_migration_unlock, prepare_display_services),
                    (args.system_app_migrator, args.development_fail_fast_system_app_installs, prepare_system_app_migrator),
                    (args.springboard_migrator, args.development_defer_launch_images, prepare_springboard_migrator),
                    (args.fairplay_launchd_plist, args.development_no_sep_service_readiness, prepare_no_sep_service_readiness)]
        selected = [(source, prepare) for source, enabled, prepare in policies if source and enabled]
        if (not args.output or len(selected) != 1 or
                any(bool(source) != enabled for source, enabled, _ in policies)):
            parser.error('prepare-display-services requires --output and exactly one pinned source with its development policy flag')
        if bool(args.migration_resource_fork) != bool(args.migration_decmpfs):
            parser.error('--service-resource-fork and --service-decmpfs must be supplied together')
        source, prepare = selected[0]
        cache_options = {}
        if args.launchd_cache or args.launchd_cache_signature:
            if prepare != prepare_no_sep_service_readiness or not (args.launchd_cache and args.launchd_cache_signature):
                parser.error('launchd cache inputs require the no-SEP readiness policy and both cache/signature')
            cache_options = {'cache': args.launchd_cache, 'cache_signature': args.launchd_cache_signature}
        elif prepare == prepare_no_sep_service_readiness:
            parser.error('iOS18 readiness policy requires --launchd-cache and --launchd-cache-signature')
        print(json.dumps(prepare(source, args.output,
            args.migration_resource_fork, args.migration_decmpfs, **cache_options), indent=2))
    elif args.command == 'build-display-client':
        build_iokit_client('displayctl', ['displayctl/displayctl.mm'],
            extra_exports=['IORegistryEntryGetRegistryEntryID', 'IORegistryEntryCreateCFProperty', 'IORegistryEntrySetCFProperties'],
            headers=['include/IOKitUser.h'], frameworks=['Foundation', 'QuartzCore', 'CoreGraphics', 'ImageIO', 'IOSurface', 'Metal'],
            entitlements={'com.apple.QuartzCore.displayable-context': True,
                          'com.apple.QuartzCore.secure-mode': True,
                          'com.apple.QuartzCore.display-state': True,
                          'com.apple.QuartzCore.global-capture': True,
                          'com.apple.QuartzCore.secure-capture': True},
            signing_identifier='org.darwin.development.displayctl')
        build_iokit_client('displayctl', ['displayctl/displayctl.mm'],
            extra_exports=['IORegistryEntryGetRegistryEntryID', 'IORegistryEntryCreateCFProperty', 'IORegistryEntrySetCFProperties'],
            headers=['include/IOKitUser.h'], frameworks=['Foundation', 'QuartzCore', 'CoreGraphics', 'ImageIO', 'IOSurface', 'Metal'], adhoc_sign=True, loadable=True)
    elif args.command == 'build-wallpaper-client':
        build_iokit_client('wallpaperctl', ['wallpaperctl/wallpaperctl.mm'],
            frameworks=['Foundation', 'UIKit'], adhoc_sign=True)
    elif args.command == 'build-gyro-client':
        build_iokit_client('gyroctl', ['gyroctl/gyroctl.mm'],
            frameworks=['Foundation', 'CoreMotion'], adhoc_sign=True)
    elif args.command == 'build-gpu-client':
        build_gpu_client()
    elif args.command == 'build-gpu-metal':
        build_gpu_metal()
    elif args.command == 'build-gpu-shaders':
        build_gpu_shaders()
    elif args.command == 'build-drivers':
        build_drivers(args.jobs)
    elif args.command == 'verify-drivers':
        verify_drivers()
    elif args.command == 'test-drivers':
        test_drivers(args.test, sanitize=args.sanitize)
    elif args.command == 'pack-drivers':
        pack_drivers(args.jobs, args.collection, args.output, args.base_manifest, args.local_kernel)
    elif args.command == 'verify-collection':
        verify_collection(args.collection, args.output)
    else:
        verify_kernel()


def collect_sdm835():
    """Restore pinned reference data, verifying every source against its manifest."""
    manifest = json.loads((ROOT / 'docs/sdm835/sources.json').read_text())
    for item in manifest['sources']:
        path = ROOT / 'docs/sdm835/reference' / item['file']
        if not path.exists():
            # Prefer the checked-in snapshots; this command repairs missing data.
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
        if digest(path) != item['sha256']:
            raise RuntimeError('reference hash mismatch: ' + item['file'])
        print('Verified reference: ' + item['file'])


def test_platform():
    if TARGET == 'SM8150':
        BUILD.mkdir(exist_ok=True)
        run([BIN / 'clang-arm64', '-fsyntax-only', ROOT / 'tests/sm8150_config_test.c',
             '-mkernel', '-march=armv8-a+crc+crypto', '-DARM64_BOARD_CONFIG_SM8150',
             '-DKERNEL_PRIVATE', '-DXNU_KERNEL_PRIVATE', '-DKERNEL', '-DPRIVATE',
             '-I' + str(SRC / 'pexpert'), '-I' + str(SRC / 'osfmk'),
             '-I' + str(OBJ / 'EXPORT_HDRS/osfmk'), '-isysroot', SDK])
        for name, source, compiler_name, flags in (
                ('arm-smp-sm8150', ROOT / 'tests/arm_smp_test.cpp', 'c++',
                 ['-std=c++17', '-DARM64_BOARD_CONFIG_SM8150', '-I' + str(SRC / 'pexpert')]),
                ('sm8150-platform', DRIVERS_ROOT / 'platform/SM8150/tests/config_test.cpp', 'c++',
                 ['-std=c++17', '-I' + str(DRIVERS_ROOT / 'platform/GenericARMPlatform')]),
                ('geni-console', ROOT / 'tests/geni_console_test.c', 'cc',
                 ['-std=c11', '-I' + str(SRC / 'pexpert')]),
                ('geni-adapter', ROOT / 'tests/geni_adapter_test.c', 'cc',
                 ['-std=c11', '-pthread', '-I' + str(SRC / 'pexpert')])):
            exe = BUILD / (name + '-test')
            run([host(compiler_name), *flags, '-Wall', '-Wextra', '-Werror', source, '-o', exe])
            run([exe])
        print('SM8150: 8 CPU affinities, 4+3+1 scheduling groups, 4K pages and conservative ISA passed')
    elif TARGET == 'SDM835':
        test_sdm835()
    else:
        print('VMAPPLE: no Qualcomm platform checks selected')


def test_sdm835():
    collect_sdm835()
    BUILD.mkdir(exist_ok=True)
    exe = BUILD / 'uartdm-console-test'
    run([host('cc'), '-std=c11', '-pthread', '-Wall', '-Wextra', '-Werror',
         '-Wno-misleading-indentation', '-I' + str(SRC / 'pexpert'),
         ROOT / 'tests/uartdm_console_test.c', '-o', exe])
    run([exe])
    run([BIN / 'clang-arm64', '-fsyntax-only', ROOT / 'tests/sdm835_config_test.c',
         '-mkernel', '-march=armv8-a+crc+crypto', '-DARM64_BOARD_CONFIG_SDM835',
         '-DKERNEL_PRIVATE', '-DXNU_KERNEL_PRIVATE', '-DKERNEL', '-DPRIVATE',
         '-I' + str(SRC / 'pexpert'), '-I' + str(SRC / 'osfmk'),
         '-I' + str(OBJ / 'EXPORT_HDRS/osfmk'), '-isysroot', SDK])
    print('SDM835: physical target, 8 CPUs, 2 clusters, 4K pages and conservative ISA passed')


def package():
    verify_kernel()
    output = BUILD / 'output' / f'xnu-toolchain-linux-{os.uname().machine}.tar.gz'
    def include(info):
        if '/.git/' in info.name or info.name.endswith('/.git') or '/__pycache__' in info.name:
            return None
        if info.name.startswith('xnu-toolchain/tools/cache/'):
            return None
        if info.name.endswith(('.orig', '.rej', '.pyc')):
            return None
        return info
    with tarfile.open(output, 'w:gz', compresslevel=3) as archive:
        for name in ('Makefile', 'README.md', '.gitignore', 'scripts', 'tools', 'kernel', 'drivers',
                     'boot/bridge', 'boot/storage', 'device_trees', 'docs', 'tests'):
            path = ROOT / name
            if path.exists():
                archive.add(path, arcname='xnu-toolchain/' + name, filter=include)
        # Ordered patch verification requires the original, checksum-pinned
        # XNU input even when the portable toolchain is used offline.
        xnu_archive = TOOLS / 'cache' / json.loads(LOCK.read_text())['xnu']['archive']
        archive.add(xnu_archive, arcname='xnu-toolchain/tools/cache/' + xnu_archive.name)
        for path in (BUILD / 'output').glob('mach*'):
            archive.add(path, arcname='xnu-toolchain/build/output/' + path.name)
        archive.add(BUILD / 'output/verification.json', arcname='xnu-toolchain/build/output/verification.json')
        for path in (BUILD / 'output').glob('verification-*.json'):
            archive.add(path, arcname='xnu-toolchain/build/output/' + path.name)
        extensions = BUILD / 'output/Extensions'
        if extensions.exists():
            archive.add(extensions, arcname='xnu-toolchain/build/output/Extensions')
    print(f'Portable archive: {output} ({output.stat().st_size} bytes)')


def driver_configuration():
    path = DRIVERS_ROOT / 'Build.json'
    config = json.loads(path.read_text()) if path.exists() else {}
    definitions = config.get('target_collections', {}).get(TARGET, config.get('collections', {}))
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
        roots, tests = [], []
        for parent in parents:
            inherited = collection(parent, (*visiting, name))
            roots += inherited.get('roots', [])
            tests += inherited.get('tests', [])
        # Paths/output names belong to the child. Only dependency roots and
        # default checks are inherited; an explicit child tests list overrides.
        value['roots'] = list(dict.fromkeys([*roots, *value.get('roots', [])]))
        if 'tests' not in value and tests:
            value['tests'] = list(dict.fromkeys(tests))
        resolved[name] = value
        return value
    for name in definitions:
        collection(name)
    config['collections'] = resolved
    return config


def driver_sources():
    result, identifiers, executables = [], set(), set()
    for path in sorted(DRIVERS_ROOT.rglob('Info.plist')):
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
        raise RuntimeError(f'no Kext sources found: {DRIVERS_ROOT}')
    if DRIVER_FILTERS:
        matched = {value for source, info in result for value in DRIVER_FILTERS
                   if value in (source.name, info['CFBundleExecutable'], info['CFBundleIdentifier'])}
        if matched != set(DRIVER_FILTERS):
            raise RuntimeError(f'unknown driver selection: {sorted(set(DRIVER_FILTERS) - matched)}')
        result = [(source, info) for source, info in result if any(value in
                  (source.name, info['CFBundleExecutable'], info['CFBundleIdentifier']) for value in DRIVER_FILTERS)]
    return result


def prepare_driver_sdk(jobs):
    # Once the matching SDK is installed, a standalone Kext build should not
    # replay kernel-source patches (the kernel may be under active editing).
    aliases()
    marker = BUILD / '.workspace-root'
    if (not marker.exists() or marker.read_text().strip() != f'{ROOT}\n{SRC}' or
            not (BUILD / ('.headers-installed-' + TARGET.lower())).exists() or
            not (SDK / 'System/Library/Frameworks/Kernel.framework/Headers/IOKit/IOInterruptController.h').exists()):
        prepare_sdk(jobs)


def driver_compile_flags():
    """Shared SDK, architecture and diagnostics for Kexts and compile checks."""
    framework = SDK / 'System/Library/Frameworks/Kernel.framework'
    common = ['-isysroot', SDK, '-nostdinc',
              '-isystem', framework / 'PrivateHeaders', '-isystem', framework / 'Headers',
              '-isystem', SDK / 'usr/include',
              '-I' + str(SDK / 'usr/local/include/kernel'), '-I' + str(SRC / 'EXTERNAL_HEADERS'),
              '-mkernel', '-fno-builtin', '-fno-common', '-mgeneral-regs-only', '-march=armv8-a+crc+crypto',
              '-DKERNEL', '-DKERNEL_PRIVATE', '-DDRIVER_PRIVATE', '-DAPPLE', '-DNeXT',
              '-D__APPLE_CC__=6000', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
              '-Wno-unused-parameter', '-Wno-macro-redefined', '-Wno-c99-designator',
              '-Wno-reorder-init-list', '-Wno-missing-field-initializers',
              '-Wno-missing-designated-field-initializers']
    common += ['-I' + str(DRIVERS_ROOT / relative)
               for relative in driver_configuration().get('include_dirs', [])]
    return common


def driver_language_flags(cpp):
    return ['-std=gnu++17', '-fapple-kext', '-fno-exceptions', '-fno-rtti',
            '-fc++-static-destructors=none'] if cpp else ['-std=gnu11']


def build_drivers(jobs, sources=None):
    """Build real MH_KEXT_BUNDLEs with XNU headers and Apple's kmod stubs."""
    prepare_driver_sdk(jobs)
    common = driver_compile_flags()
    config = driver_configuration()
    sources = driver_sources() if sources is None else sources
    gpu_generated = build_gpu_shaders() if any(info['CFBundleExecutable'] == 'QcomGPU' for _,info in sources) else None
    for source, info in sources:
        name = info['CFBundleExecutable']
        units = sorted(p for p in source.iterdir() if p.suffix in ('.c', '.cpp', '.cc', '.cxx'))
        if not units:
            raise RuntimeError(f'no compilation units: {source}')
        has_cpp = any(p.suffix in ('.cpp', '.cc', '.cxx') for p in units)
        resident = config.get('resident', has_cpp)
        if has_cpp and not resident:
            raise RuntimeError('this compiler configuration supports resident C++ Kexts; set resident=true')
        objdir = BUILD / 'drivers/obj' / name
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
        units += [generated, SRC / 'libkern/kmod/c_start.c', SRC / 'libkern/kmod/c_stop.c']
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
            run([BIN / ('clang++-arm64' if cpp else 'clang-arm64'), *common, '-I' + str(source),
                 *(['-I'+str(gpu_generated)] if name == 'QcomGPU' else []),
                 *flags, '-c', unit, '-o', obj], log=layout_log)
            objects.append(obj)
        bundle = DRIVER_OUTPUT / (name + '.kext') / 'Contents'
        (bundle / 'MacOS').mkdir(parents=True, exist_ok=True)
        # Classic Apple ld64 supports -kext directly. No Mach-O header patching.
        run([BIN / 'arm-apple-darwin11-ld', '-arch', 'arm64', '-kext', '-syslibroot', SDK,
             '-platform_version', 'ios', '18.6', '18.6', '-undefined', 'dynamic_lookup',
             '-o', bundle / 'MacOS' / name, *objects])
        shutil.copy2(source / 'Info.plist', bundle / 'Info.plist')
        if name == 'QcomGPU':
            (bundle / 'Resources').mkdir(exist_ok=True)
            shutil.copyfile(TOOLS / 'gpu-compiler/NOTICE.txt', bundle / 'Resources/NOTICE.Mesa.txt')
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
    return {'sha256': digest(path), 'bytes': len(data), 'architecture': 'arm64',
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
            exports=SRC/'config'/(family+suffix+'.exports')
            if exports.exists():
                patterns.extend(line.split(':')[0].strip() for line in exports.read_text().splitlines()
                                if line.startswith('_'))
    return patterns


def verify_drivers(sources=None):
    for relative in driver_configuration().get('source_manifests', []):
        manifest = DRIVERS_ROOT / relative
        for name, source in json.loads(manifest.read_text()).items():
            path = manifest.parent / name
            if digest(path) != source['sha256']:
                raise RuntimeError(f'driver reference/header checksum mismatch: {path}')
    root = DRIVER_OUTPUT
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
            log = BUILD / 'drivers/obj' / name / (layout['unit'] + '.vtable.txt')
            report['native_abi_evidence'] = [verify_abi_contract(json.loads((DRIVERS_ROOT / p).read_text())) for p in layout.get('native_abi_contracts', [])]
            report['compiler_vtable'] = verify_compiler_vtable(log.read_text(), layout.get('class', name), layout)
            if layout.get('additional_classes'):
                report['additional_compiler_vtables'] = {}
                for cls, contract in layout['additional_classes'].items():
                    log = BUILD / 'drivers/obj' / name / (contract['unit'] + '.vtable.txt')
                    report['additional_compiler_vtables'][cls] = verify_compiler_vtable(log.read_text(), cls, contract)
        report.update({'bundle': name + '.kext', 'identifier': identifier, 'libraries': info.get('OSBundleLibraries', {}),
                       'personalities': info.get('IOKitPersonalities', {})})
        reports.append(report)
    nm = subprocess.check_output([BIN / 'arm-apple-darwin11-nm', '-j', '-U',
                                 BUILD / 'output' / (KERNEL_FILE + '.unstripped')], text=True)
    kernel_symbols = set(nm.splitlines())
    by_id = {r['identifier']: r for r in reports}
    needed = {dependency for report in reports for dependency in report['libraries']}
    contracts = [c for p in driver_configuration().get('abi_contracts', [])
                 if (c := json.loads((DRIVERS_ROOT / p).read_text()))['bundle_identifier'] in needed]
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
    import tempfile
    image = NATIVE_KERNELCACHE
    if image is None:
        configured = contract.get('image', driver_configuration().get('provider_image'))
        if not configured:
            raise RuntimeError('ABI contract needs --provider-image or a configured image path')
        image = (DRIVERS_ROOT / configured).resolve()
    if digest(image) != contract['image_sha256']:
        raise RuntimeError('provider image hash changed; re-audit the ABI contract')
    patcher = BUILD / 'kernelcache_patcher/kernelcache_patcher'
    project = TOOLS / 'kernelcache_patcher'
    inputs = [*project.glob('*.cpp'), *project.glob('include/*.h'), *project.glob('include/*.inc'), project / 'CMakeLists.txt']
    path = BUILD / 'drivers' / (contract['image_sha256'] + '.abi.json')
    path.parent.mkdir(parents=True, exist_ok=True)
    # Several independent driver builds can inspect the same provider. Parse
    # this invocation's private output; publishing a shared filename directly
    # lets one process read another process's partially written JSON.
    # One analysis recovers every provider class. Share that immutable result
    # within this invocation; each contract still gets its own alias bindings
    # and byte/layout checks. Include exact kernel/tool hashes in the identity.
    with ABI_ANALYSIS_LOCK:
        if not patcher.exists() or max(p.stat().st_mtime for p in inputs) > patcher.stat().st_mtime:
            run(['cmake', '-S', project, '-B', patcher.parent, '-DCMAKE_BUILD_TYPE=Release'])
            run(['cmake', '--build', patcher.parent, '-j', '3'])
        kernel = BUILD / 'output' / (KERNEL_FILE + '.unstripped')
        key = (str(image.resolve()), contract['image_sha256'], str(kernel.resolve()), digest(kernel), digest(patcher))
        if key not in ABI_ANALYSES:
            with tempfile.TemporaryDirectory(prefix='.abi-', dir=path.parent) as directory:
                pending = Path(directory) / 'report.json'
                run([patcher, 'inspect-abi', kernel, image, pending], log=Path(directory) / 'analysis.log')
                raw = pending.read_text()
                json.loads(raw)
                if digest(kernel) != key[3] or digest(patcher) != key[4]:
                    raise RuntimeError('ABI analysis inputs changed during inspection')
                pending.replace(path)
                ABI_ANALYSES[key] = raw
        evidence = json.loads(ABI_ANALYSES[key])
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


def test_drivers(names=None, sanitize=False):
    BUILD.mkdir(exist_ok=True)
    configured = driver_configuration().get('tests', [])
    if names is not None:
        if not names or len(names) != len(set(names)) or set(names) - {t['name'] for t in configured}:
            raise RuntimeError('empty, duplicate or unknown host test selection')
        configured = [t for t in configured if t['name'] in names]
    kernel_checks = any(t.get('kernel_compile') for t in configured)
    if kernel_checks:
        prepare_driver_sdk(3)
    for test in configured:
        if test['name'] in ('gpu-submit','gpu-constant-window'): build_gpu_shaders()
        if not re.fullmatch(r'[A-Za-z0-9_-]+', test['name']):
            raise RuntimeError('invalid test output name')
        exe = BUILD / ('test-' + test['name'] + ('-sanitized' if sanitize else ''))
        includes = ['-I' + str(DRIVERS_ROOT / p) for p in test.get('include_dirs', [])]
        sources = [DRIVERS_ROOT / p for p in test['sources']]
        host_flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitize else ['-O2']
        abi_flags = test.get('host_flags', [])
        if not isinstance(abi_flags, list) or any(not isinstance(flag, str) or not flag or '\0' in flag for flag in abi_flags):
            raise RuntimeError('invalid host test compiler flags')
        run([host('c++'), '-std=c++17', *host_flags, *abi_flags, '-Wall', '-Wextra', '-Werror',
             *includes, *sources, '-o', exe])
        run([exe])
        for index, relative in enumerate(test.get('kernel_compile', [])):
            source = DRIVERS_ROOT / relative
            cpp = source.suffix in ('.cpp', '.cc', '.cxx')
            if not cpp and source.suffix != '.c':
                raise RuntimeError(f'unsupported kernel compile check: {source}')
            objdir = BUILD / 'tests/kernel' / test['name']
            objdir.mkdir(parents=True, exist_ok=True)
            object_path = objdir / (str(index) + '-' + source.name + '.o')
            layouts = test.get('kernel_vtables', {}).get(relative, {})
            layout_flags = ['-Xclang', '-fdump-vtable-layouts'] if layouts else []
            layout_log = objdir / (str(index) + '-' + source.name + '.vtable.txt') if layouts else None
            if layout_log:
                layout_log.unlink(missing_ok=True)
            run([BIN / ('clang++-arm64' if cpp else 'clang-arm64'),
                 *driver_compile_flags(), *includes, *driver_language_flags(cpp), *layout_flags,
                 '-c', source, '-o', object_path], log=layout_log)
            for name, contract in layouts.items():
                verify_compiler_vtable(layout_log.read_text(), name, contract)
            # A positional ABI check alone does not prove that signatures in
            # a probe still match its audited imports. New/changed provider
            # references must be added to the contract before this can pass.
            provider_contracts = test.get('kernel_provider_imports', {}).get(relative, [])
            if provider_contracts:
                nm = subprocess.check_output([BIN / 'arm-apple-darwin11-nm', '-u', object_path], text=True)
                imported = {line.strip() for line in nm.splitlines()}
                for relative_contract in provider_contracts:
                    contract = json.loads((DRIVERS_ROOT / relative_contract).read_text())
                    # Itanium member/static/constructor/destructor names for
                    # classes owned by this provider; KPI references remain
                    # covered by the ordinary Kext verifier once linked.
                    prefixes = tuple('__ZN' + qualifier + str(len(name)) + name
                                     for name in contract.get('classes', {}) for qualifier in ('', 'K'))
                    selected = {name for name in imported if name.startswith(prefixes)}
                    missing = selected - set(contract.get('required_symbols', []))
                    if missing:
                        raise RuntimeError(f'unaudited kernel probe provider imports: {sorted(missing)}')
        for relative in test.get('abi_contracts', []):
            verify_abi_contract(json.loads((DRIVERS_ROOT / relative).read_text()))


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


def pack_drivers(jobs, collection, output=None, base_manifest=None, local_kernel=None):
    """Resolve a configured collection, build local bundles, then use the generic KC linker."""
    import importlib.util
    import tempfile
    if not collection or not re.fullmatch(r'[A-Za-z0-9_-]+', collection):
        raise RuntimeError('invalid or missing collection name')
    config = driver_configuration()
    definition = config.get('collections', {}).get(collection)
    if not definition:
        raise RuntimeError('select a configured collection with --collection')
    image = NATIVE_KERNELCACHE
    if image is None:
        configured = config.get('provider_image')
        if not configured:
            raise RuntimeError('collection needs --provider-image or a configured provider image')
        image = (DRIVERS_ROOT / configured).resolve()
    spec = importlib.util.spec_from_file_location('kext_inventory', ROOT / 'scripts/analyze-kernelcache.py')
    inventory = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(inventory)
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
    selected = [local[name] for name in order if name in local]
    if selected:
        build_drivers(jobs, selected)
    test_drivers(definition.get('tests'))
    (BUILD / 'collections').mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=collection + '-', dir=BUILD / 'collections'))
    inputs = stage / 'inputs'
    inputs.mkdir()
    kernel = snapshot_input(local_kernel or BUILD / 'output' / (KERNEL_FILE + '.unstripped'), inputs / 'kernel.macho')
    image = snapshot_input(image, inputs / 'provider.macho')
    binaries = {}
    for name in order:
        if name not in local:
            continue
        info = local[name][1]
        bundle = DRIVER_OUTPUT / (info['CFBundleExecutable'] + '.kext')
        link = stage / (name + '.kext')
        (link / 'Contents/MacOS').mkdir(parents=True)
        binary = snapshot_input(bundle / 'Contents/MacOS' / info['CFBundleExecutable'],
                                link / 'Contents/MacOS' / info['CFBundleExecutable'])
        (link / 'Contents/Info.plist').write_bytes(plistlib.dumps(info, sort_keys=False))
        binaries[name] = digest(binary)
    (stage / 'kexts.txt').write_text('\n'.join(order) + '\n')
    patcher = BUILD / 'kernelcache_patcher/kernelcache_patcher'
    run(['cmake', '-S', TOOLS / 'kernelcache_patcher', '-B', patcher.parent, '-DCMAKE_BUILD_TYPE=Release'])
    run(['cmake', '--build', patcher.parent, '-j', str(jobs)])
    run(['ctest', '--test-dir', patcher.parent, '--output-on-failure'])
    if output is None:
        configured = definition.get('output')
        if not configured:
            raise RuntimeError('collection needs --output or a configured output path')
        output = DRIVERS_ROOT / configured
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    log = stage / 'link.log'
    # The same hash/layout-checked provider contracts used for import
    # accounting must also reach the linker. Otherwise a stripped provider
    # can pass offline accounting but leave subclass virtual imports unresolved.
    provider_symbols = {}
    for relative in config.get('abi_contracts', []):
        contract = json.loads((DRIVERS_ROOT / relative).read_text())
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
    run([patcher, kernel, image, symbol_argument, stage, output], log=log)
    report = json.loads(Path(str(output) + '.verification.json').read_text())
    if report.get('status') != 'passed' or report.get('kext_count') != len(order):
        raise RuntimeError('collection verifier did not confirm the expected bundle count')
    manifest = {'bundle_order': order, 'local_bundle_sha256': binaries, 'kernel_sha256': digest(kernel),
                'provider_sha256': digest(image), 'output_sha256': digest(output),
                'inputs': {'kernel': os.path.relpath(kernel, output.parent),
                           'provider': os.path.relpath(image, output.parent)},
                'link_log': os.path.relpath(log, output.parent),
                'hardware_boot_verified': False}
    Path(str(output) + '.manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Collection published: {output}; {len(order)} Kexts; SHA256 {manifest["output_sha256"]}')


def build_power_client():
    build_iokit_client('powerctl', ['powerctl/powerctl.cpp'], headers=['include/IOKitUser.h'],
                       protocol='PowerUserProtocol.h', adhoc_sign=True)


def build_gpu_client():
    build_iokit_client('gpuctl', ['gpuctl/gpuctl.cpp', 'gpuctl/GPUConnection.cpp'],
                       extra_exports=['IOConnectCallMethod', 'IOObjectConformsTo', 'IORegistryEntryCreateCFProperty',
                                      'IODataQueueDataAvailable', 'IODataQueueDequeue',
                                      'IOConnectSetNotificationPort', 'IODataQueueAllocateNotificationPort'],
                       headers=['gpuctl/GPUConnection.h', 'gpuctl/CPUStatus.h', 'include/IOKitUser.h'],
                       protocol=['PowerUserProtocol.h', 'ClockControllerProtocol.h', 'GPUUserProtocol.h', 'GPUNativeProtocol.h', 'GPURenderPass.h', 'GPUMetalCapabilities.h', 'GPUNativeCompletion.h', 'GPUNativeMemory.h', 'GPUDiagnostics.h', 'GPUHardwarePhases.h', 'GPUMappingBounds.h', 'GPUJobActivity.h', 'GPUOperationActivity.h', 'GPUShaderCounters.h', 'GPUSurfaceProtocol.h', 'GPUShaderInterface.h', 'GPUShaderProtocol.h', 'GPUJITProtocol.h'],
                       adhoc_sign=True,
                       entitlements={'org.darwin.development.gpu-control':True},
                       signing_identifier='org.darwin.development.gpuctl')


def translate_gpu_msl(source):
    """AOT and runtime JIT share the production C++ MSL parser."""
    root = BUILD / 'gpu-compiler'; root.mkdir(parents=True, exist_ok=True)
    exe = root / 'msl-frontend'; stamp = root / 'msl-frontend.json'
    sources = [TOOLS / 'gpu-compiler' / name for name in ['MSLMain.cpp','MSL.cpp','Semantic.h','Compiler.h']]
    sources += [DRIVERS_ROOT / 'include' / name for name in ['GPUShaderInterface.h','GPUShaderProtocol.h','GPUJITProtocol.h','GPUUserProtocol.h']]
    hashes = {str(path.relative_to(ROOT)):digest(path) for path in sources}
    if not exe.exists() or not stamp.exists() or json.loads(stamp.read_text()).get('inputs') != hashes or json.loads(stamp.read_text()).get('sha256') != digest(exe):
        config = host('llvm-config')
        flags = shlex.split(subprocess.check_output([config,'--cxxflags','--ldflags','--libs','support'],text=True))
        flags = [flag for flag in flags if flag != '-fno-exceptions' and not flag.startswith('-std=')]
        run([host('clang++'),TOOLS/'gpu-compiler/MSLMain.cpp',TOOLS/'gpu-compiler/MSL.cpp',*flags,
             '-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-I'+str(DRIVERS_ROOT/'include'),
             '-o',exe],log=root/'msl-build.log')
        stamp.write_text(json.dumps({'inputs':hashes,'sha256':digest(exe)},indent=2)+'\n')
    result = subprocess.run([exe],input=source,capture_output=True,text=True)
    if result.returncode: raise RuntimeError(result.stderr.strip() or 'MSL frontend failed')
    return json.loads(result.stdout)


def prepare_gpu_compiler():
    root = BUILD / 'gpu-compiler'
    source = root / 'mesa'
    spec = json.loads((TOOLS / 'gpu-compiler/Source.json').read_text())
    if not (source / '.git').exists():
        run([host('git'), 'clone', '--depth', '1', '--filter=blob:none', '--sparse',
             '--branch', spec['tag'], spec['repository'], source])
        run([host('git'), '-C', source, 'sparse-checkout', 'set', *spec['sparse_paths']])
    commit = subprocess.check_output([host('git'), '-C', source, 'rev-parse', 'HEAD'],text=True).strip()
    if commit != spec['commit']: raise RuntimeError('Mesa compiler source commit changed')
    # Modern glibc exposes C23 call_once from stdlib.h. Mesa 24's emulation
    # conflicts with that type; use the detected native C11 API on Linux.
    meson_root = source / 'meson.build'
    text = meson_root.read_text()
    updated = text.replace("if cc.has_function('thrd_create', prefix: '#include <threads.h>')\n  if with_platform_android",
        "if cc.has_function('thrd_create', prefix: '#include <threads.h>')\n  if with_platform_android or host_machine.system() == 'linux'")
    if text != updated: meson_root.write_text(updated)
    # glibc's native mtx_t wraps pthread_mutex_t (as in glibc mtx_init.c).
    # Mesa's monotonic-condvar helper crosses that API boundary explicitly.
    monotonic = source / 'src/util/cnd_monotonic.c'
    text = monotonic.read_text()
    updated = text.replace('&cond->cond, mtx,', '&cond->cond, (pthread_mutex_t *)mtx,')
    updated = updated.replace('&cond->cond, mtx)', '&cond->cond, (pthread_mutex_t *)mtx)')
    if text != updated: monotonic.write_text(updated)
    standalone = source / 'src/compiler/glsl/standalone.cpp'
    text = standalone.read_text()
    # The shared NIR backend implements demotion in software on A5xx. Enable
    # only the language extension here, without advertising a GPU capability.
    if 'ctx->Extensions.EXT_demote_to_helper_invocation = true;' not in text:
        needle = 'ctx->Extensions.ARB_ES3_2_compatibility = true;'
        if needle not in text: raise RuntimeError('Mesa standalone extension initialization changed')
        text = text.replace(needle, needle+'\n   ctx->Extensions.EXT_demote_to_helper_invocation = true;',1)
    # Preserve shadow-array explicit LOD as txl, rather than approximating it
    # with gradients. This enables a private compiler bridge language feature;
    # it does not advertise OpenGL or Metal capabilities on the target.
    if 'ctx->Extensions.EXT_texture_shadow_lod = true;' not in text:
        needle = 'ctx->Extensions.ARB_ES3_2_compatibility = true;'
        if needle not in text: raise RuntimeError('Mesa standalone shadow-LOD initialization changed')
        text = text.replace(needle, needle+'\n   ctx->Extensions.EXT_texture_shadow_lod = true;',1)
    # Explicit binary16 values in translated AIR use Mesa's existing typed
    # arithmetic. This is a private compiler language, not a device API claim.
    if 'ctx->Extensions.AMD_gpu_shader_half_float = true;' not in text:
        needle = 'ctx->Extensions.ARB_ES3_2_compatibility = true;'
        if needle not in text: raise RuntimeError('Mesa standalone half initialization changed')
        text = text.replace(needle, needle+'\n   ctx->Extensions.AMD_gpu_shader_half_float = true;',1)
    # Match IR3's scalar derivative requirement while GLSL constructs NIR.
    for name in ('nir_vs_options','nir_fs_options'):
        text = text.replace(name+' = { 0 };',name+' = { .scalarize_ddx = true };')
    needle = 'ctx->Const.ShaderCompilerOptions[MESA_SHADER_FRAGMENT].NirOptions =\n      &nir_vs_options;'
    addition = '\n   ctx->Const.ShaderCompilerOptions[MESA_SHADER_COMPUTE].NirOptions = &nir_vs_options;'
    if addition not in text:
        if needle not in text: raise RuntimeError('Mesa standalone NIR initialization changed')
        text = text.replace(needle,needle+addition,1)
    limits = '\n   ctx->Const.MaxCombinedShaderStorageBlocks = 24;\n   for (unsigned stage = 0; stage < MESA_SHADER_STAGES; ++stage)\n      ctx->Const.Program[stage].MaxShaderStorageBlocks = 8;'
    if limits not in text:
        text = text.replace(addition, addition+limits, 1)
    sample_limits = '\n   ctx->Const.MaxCombinedTextureImageUnits = 384;\n   ctx->Const.Program[MESA_SHADER_COMPUTE].MaxTextureImageUnits = 32;\n   ctx->Const.Program[MESA_SHADER_VERTEX].MaxTextureImageUnits = 32;\n   ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxTextureImageUnits = 32;'
    text = text.replace('ctx->Const.MaxCombinedTextureImageUnits = 64;', 'ctx->Const.MaxCombinedTextureImageUnits = 384;')
    text = text.replace('ctx->Const.MaxCombinedTextureImageUnits = 128;', 'ctx->Const.MaxCombinedTextureImageUnits = 384;')
    text = text.replace('ctx->Const.MaxCombinedTextureImageUnits = 192;', 'ctx->Const.MaxCombinedTextureImageUnits = 384;')
    if sample_limits not in text:
        # Up to 32 active combined uniforms are split into independent A540
        # texture/sampler slots; physical sampler occupancy is checked later.
        text = text.replace('   ctx->Const.GenerateTemporaryNames = true;',
            '   ctx->Const.GenerateTemporaryNames = true;'+sample_limits,1)
    if standalone.read_text() != text: standalone.write_text(text)
    text = standalone.read_text().replace('ctx->Const.MaxShaderStorageBufferBindings = 4;',
        'ctx->Const.MaxShaderStorageBufferBindings = 8;').replace('ctx->Const.MaxShaderStorageBlockSize = 4096;',
        'ctx->Const.MaxShaderStorageBlockSize = 4194304;')
    text=text.replace('ctx->Const.MaxShaderStorageBufferBindings = 8;','ctx->Const.MaxShaderStorageBufferBindings = 16;')
    if 'ctx->Const.Program[MESA_SHADER_COMPUTE].MaxShaderStorageBlocks = 16;' not in text:
        text=text.replace(limits,limits+'\n   ctx->Const.Program[MESA_SHADER_COMPUTE].MaxShaderStorageBlocks = 16;')
    text = text.replace('ctx->Const.MaxComputeSharedMemorySize = 65536;',
                        'ctx->Const.MaxComputeSharedMemorySize = 32768;')
    if standalone.read_text() != text: standalone.write_text(text)
    # Preserve upstream text and derive these small compiler adaptations on
    # every build. GLSL's discard-loop escape rule does not apply to demote:
    # demoted Metal lanes must keep computing derivatives after the call.
    discard_flow = source / 'src/compiler/glsl/gl_nir_lower_discard_flow.c'
    discard_pristine = root / 'gl_nir_lower_discard_flow.original.c'
    if not discard_pristine.exists(): shutil.copyfile(discard_flow, discard_pristine)
    text = discard_pristine.read_text()
    needle = '''intrin->intrinsic == nir_intrinsic_terminate ||
                intrin->intrinsic == nir_intrinsic_demote_if ||
                intrin->intrinsic == nir_intrinsic_demote)'''
    if needle not in text: raise RuntimeError('Mesa discard-flow lowering changed')
    text = text.replace(needle, 'intrin->intrinsic == nir_intrinsic_terminate)', 1)
    if discard_flow.read_text() != text: discard_flow.write_text(text)
    # The deferred KILL must remain after explicit derivatives and implicit
    # texture derivatives. False dependencies survive both IR3 schedulers;
    # this emits no extra hardware barrier and affects only discarding FSs.
    ir3_nir = source / 'src/freedreno/ir3/ir3_compiler_nir.c'
    ir3_pristine = root / 'ir3_compiler_nir.original.c'
    if not ir3_pristine.exists(): shutil.copyfile(ir3_nir, ir3_pristine)
    text = ir3_pristine.read_text()
    dependency = '''   if (ctx->compiler->gen == 5 && ctx->so->type == MESA_SHADER_FRAGMENT && ctx->s->info.fs.uses_discard) {
      %s->barrier_class |= IR3_BARRIER_ACTIVE_FIBERS_R;
      %s->barrier_conflict |= IR3_BARRIER_ACTIVE_FIBERS_W;
   }
'''
    if text.count('   return sam;') != 1: raise RuntimeError('Mesa texture emission changed')
    text = text.replace('   return sam;', (dependency % ('sam', 'sam'))+'   return sam;', 1)
    for op in ('DSX', 'DSXPP_MACRO', 'DSY', 'DSYPP_MACRO'):
        needle = f'      dst[0] = ir3_{op}(b, src, 0);\n      dst[0]->cat5.type = TYPE_F32;'
        if text.count(needle) != 1: raise RuntimeError('Mesa derivative emission changed')
        text = text.replace(needle, needle+'\n'+(dependency % ('dst[0]', 'dst[0]')), 1)
    # Mesa MR 44343 (69f8c3fd): A5xx needs a live per-sample input even
    # when only sample-qualified varyings request sample-rate execution.
    needle = '      ctx->ij[IJ_PERSP_PIXEL] = create_input(ctx, 0x3);'
    if text.count(needle) != 1: raise RuntimeError('Mesa fragment input setup changed')
    text = text.replace(needle, needle+'''
      if (ctx->compiler->gen == 5 && ctx->s->info.fs.uses_sample_shading) {
         ctx->samp_id = create_sysval_input(ctx, SYSTEM_VALUE_SAMPLE_ID, 0x1);
         ctx->samp_id->dsts[0]->flags |= IR3_REG_HALF;
         array_insert(ctx->samp_id->block, ctx->samp_id->block->keeps, ctx->samp_id);
      }''', 1)
    if ir3_nir.read_text() != text: ir3_nir.write_text(text)
    # A5xx's untyped buffer opcodes carry an element type. Upstream's a4xx
    # emitter assumes 32-bit SSBOs; AIR half loads/stores need 16-bit registers
    # and the matching TYPE_U16 encoding, including their halfword offsets.
    a4xx = source / 'src/freedreno/ir3/ir3_a4xx.c'
    a4xx_pristine = root / 'ir3_a4xx.original.c'
    if not a4xx_pristine.exists(): shutil.copyfile(a4xx, a4xx_pristine)
    text = a4xx_pristine.read_text()
    needle = '   ldgb->cat6.type = TYPE_U32;'
    if text.count(needle) != 1: raise RuntimeError('Mesa A5xx buffer load emission changed')
    text = text.replace(needle, '''   bool half = ctx->compiler->gen == 5 && intr->def.bit_size == 16;
   ldgb->cat6.type = half ? TYPE_U16 : TYPE_U32;
   if (half) ldgb->dsts[0]->flags |= IR3_REG_HALF;''', 1)
    needle = '   stgb->cat6.type = TYPE_U32;'
    if text.count(needle) != 1: raise RuntimeError('Mesa A5xx buffer store emission changed')
    text = text.replace(needle, '''   stgb->cat6.type = ctx->compiler->gen == 5 &&
      nir_src_bit_size(intr->src[0]) == 16 ? TYPE_U16 : TYPE_U32;''', 1)
    if a4xx.read_text() != text: a4xx.write_text(text)
    # Targeted register definitions from the same upstream fixes. Keep the
    # pinned register database; do not silently switch the compiler revision.
    a5xx = source / 'src/freedreno/registers/adreno/a5xx.xml'
    a5xx_pristine = root / 'a5xx.original.xml'
    if not a5xx_pristine.exists(): shutil.copyfile(a5xx, a5xx_pristine)
    text = a5xx_pristine.read_text()
    needle = '\t\t<bitfield name="SAMPLEID" pos="2" type="boolean"/>'
    if text.count(needle) != 1: raise RuntimeError('A5xx sample control definitions changed')
    text = text.replace(needle, needle+'\n\t\t<bitfield name="SAMPLEMODE" low="3" high="4" type="uint"/>\n\t\t<bitfield name="CENTERRHW" pos="5" type="boolean"/>', 1)
    needle = '\t\t<bitfield name="FRAG_WRITES_Z" pos="5" type="boolean"/>'
    if text.count(needle) != 1: raise RuntimeError('A5xx fragment output definitions changed')
    text = text.replace(needle, needle+'\n\t\t<bitfield name="FRAG_WRITES_SAMPMASK" pos="6" type="boolean"/>', 1)
    if a5xx.read_text() != text: a5xx.write_text(text)
    linker = source / 'src/compiler/glsl/gl_nir_linker.c'
    text = linker.read_text()
    if 'qgpu_dynamic_pool' not in text:
        needle = '   if (prog->nir->info.stage == MESA_SHADER_COMPUTE) {\n      NIR_PASS(_, prog->nir, nir_lower_vars_to_explicit_types,'
        if needle not in text: raise RuntimeError('Mesa shared-memory lowering changed')
        # Preserve dynamic pool derefs until Interface.h assigns its aligned
        # base after static objects and trims the reservation to 32 KiB.
        text = text.replace(needle,'''   bool qgpu_dynamic_pool = false;
   nir_foreach_variable_with_modes(var, prog->nir, nir_var_mem_shared)
      if (var->name && !strcmp(var->name, "_qdynamic")) qgpu_dynamic_pool = true;
   if (prog->nir->info.stage == MESA_SHADER_COMPUTE && !qgpu_dynamic_pool) {
      NIR_PASS(_, prog->nir, nir_lower_vars_to_explicit_types,''',1)
        linker.write_text(text)
    venv = root / 'venv'
    if not (venv / 'bin/meson').exists(): run([sys.executable, '-m', 'venv', venv])
    if not (venv / '.gpu-build-deps-v1').exists():
        run([venv / 'bin/pip', 'install', 'meson==1.7.0', 'Mako==1.3.9', 'PyYAML==6.0.2', 'packaging==25.0'])
        (venv / '.gpu-build-deps-v1').touch()
    # Preserve the upstream compiler and add only an allocation/code exporter.
    original = source / 'src/gallium/drivers/freedreno/ir3/ir3_cmdline.c'
    pristine = root / 'ir3_cmdline.original.c'
    if not pristine.exists(): shutil.copyfile(original, pristine)
    body = pristine.read_text()
    body = body.replace('struct ir3_shader_key key = {};',
        'struct ir3_shader_key key = {.msaa = getenv("QCOM_IR3_MSAA") != NULL};', 1)
    begin = body.index('static void\nfixup_varying_slots(')
    finish = body.index('static struct ir3_compiler *compiler;',begin)
    body = body[:begin] + body[finish:]
    for mode in ['nir_var_shader_out','nir_var_shader_in']:
        body = body.replace('      fixup_varying_slots(nir, '+mode+');\n','')
    body = body.replace('.glsl_version = 310,', '.glsl_version = 430,')
    body = body.replace('nir_shader *nir = prog->_LinkedShaders[stage]->Program->nir;', '''nir_shader *nir = prog->_LinkedShaders[stage]->Program->nir;
   nir->options = nir_options;
   if (!qgpu_capture_interface(nir, &qcom_interface)) errx(1, "unsupported stage interface");
   if (stage == MESA_SHADER_VERTEX) {
      nir_foreach_variable_with_modes(var, nir, nir_var_system_value)
         if (var->data.location == SYSTEM_VALUE_INSTANCE_ID) var->data.location = SYSTEM_VALUE_INSTANCE_INDEX;
   }
   if (stage == MESA_SHADER_FRAGMENT) {
      nir_foreach_variable_with_modes(var, nir, nir_var_shader_in) {
         if (var->data.location == VARYING_SLOT_POS) {
            var->data.mode = nir_var_system_value;
            var->data.location = SYSTEM_VALUE_FRAG_COORD;
         }
      }
      nir_fixup_deref_modes(nir);
   }''', 1)
    io_locations = '   nir_assign_var_locations(nir, nir_var_uniform, &nir->num_uniforms,'
    if io_locations not in body: raise RuntimeError('Mesa uniform location assignment changed')
    body = body.replace(io_locations, '   qgpu_assign_graphics_io(nir);\n'+io_locations, 1)
    body = body.replace('NIR_PASS_V(nir, nir_lower_compute_system_values, NULL);',
        'const nir_lower_compute_system_values_options cs_options = {};\n   NIR_PASS_V(nir, nir_lower_compute_system_values, &cs_options);')
    body = body.replace('   ir3_finalize_nir(compiler, &options, nir);',
        '   qgpu_center_fragment_position(nir, key.msaa);\n   ir3_finalize_nir(compiler, &options, nir);', 1)
    body = body.replace('if (!prog)\n', 'if (!prog || !prog->data->LinkStatus || !prog->_LinkedShaders[stage] || !prog->_LinkedShaders[stage]->Program)\n', 1)
    body = body.replace('static void\ndump_info', '#include "QcomExport.h"\n\nstatic void\ndump_info', 1)
    body = body.replace('NIR_PASS_V(nir, gl_nir_lower_buffers, prog);',
        'NIR_PASS_V(nir, gl_nir_lower_buffers, prog);\n   qcom_remap_ssbos(nir, prog->_LinkedShaders[stage]->Program);', 1)
    body = body.replace('NIR_PASS_V(nir, gl_nir_lower_samplers, prog);', '''if (stage == MESA_SHADER_COMPUTE || stage == MESA_SHADER_FRAGMENT || stage == MESA_SHADER_VERTEX) {
      NIR_PASS_V(nir, gl_nir_lower_samplers_as_deref, prog);
      if (!qgpu_bind_samples(nir, &qcom_interface)) errx(1, "invalid compute sample bindings");
      NIR_PASS_V(nir, nir_lower_samplers);
   } else NIR_PASS_V(nir, gl_nir_lower_samplers, prog);
   if (!qgpu_remap_samples(nir, &qcom_interface)) errx(1, "unsupported compute sampling");''', 1)
    first = body.index('   struct ir3_shader *shader = rzalloc_size(NULL, sizeof(*shader));')
    last = body.index('   dump_info(v, info);', first)
    body = body[:first] + '''   struct ir3_shader_options shader_options = {.num_reserved_user_consts = qgpu_memory_constants(&qcom_interface) / 4};
   struct ir3_shader *shader = ir3_shader_from_nir(compiler, nir, &shader_options, NULL);
   struct ir3_shader_variant *v = ir3_shader_create_variant(shader, &key, true);
   if (!v) errx(1, "IR3 compilation failed");
   info = "NIR compiler";
''' + body[last:]
    body = body.replace('printf("; %s: %s\\n", type, str);', 'qcom_export_program(so, bin);\n   printf("; %s: %s\\n", type, str);', 1)
    body = body.replace('bin = ir3_shader_assemble(so);', 'bin = so->bin;', 1)
    body = body.replace('ir3_shader_disasm(so, bin, stdout);',
        'if (so->disasm_info.disasm) fputs(so->disasm_info.disasm, stdout);', 1)
    if body == pristine.read_text(): raise RuntimeError('Mesa compiler exporter insertion failed')
    if original.read_text() != body: original.write_text(body)
    shutil.copyfile(TOOLS / 'gpu-compiler/Export.h', original.parent / 'QcomExport.h')
    shutil.copyfile(TOOLS / 'gpu-compiler/Interface.h', original.parent / 'QcomInterface.h')
    for name in ['GPUJITProtocol.h','GPUShaderInterface.h']:
        shutil.copyfile(DRIVERS_ROOT / 'include' / name, original.parent / name)
    obj = root / 'host'
    environment = dict(os.environ)
    environment['PATH'] = str(venv / 'bin') + os.pathsep + environment.get('PATH','')
    if not (obj / 'build.ninja').exists():
        run([venv / 'bin/meson', 'setup', obj, source, '--buildtype=release',
             '-Dgallium-drivers=freedreno', '-Dfreedreno-kmds=msm', '-Dtools=freedreno',
             '-Dplatforms=', '-Dvulkan-drivers=', '-Dllvm=disabled', '-Dglx=disabled',
             '-Degl=disabled', '-Dgles1=disabled', '-Dgles2=disabled', '-Dgbm=disabled',
             '-Dglvnd=disabled', '-Dgallium-va=disabled', '-Dgallium-vdpau=disabled',
             '-Dgallium-xa=disabled', '-Dbuild-tests=false', '-Dlibunwind=disabled',
             '-Dshader-cache=disabled', '-Dvalgrind=disabled'], env=environment)
    run([host('ninja'), '-C', obj, '-j', str(JOBS), 'src/gallium/drivers/freedreno/ir3_compiler'], env=environment)
    return obj / 'src/gallium/drivers/freedreno/ir3_compiler', source


def prepare_gpu_air_compiler():
    """Build a host-only LLVM reader/lowerer; never link LLVM into the Kext."""
    source = TOOLS / 'gpu-compiler/AIR.cpp'
    root = BUILD / 'gpu-compiler'; root.mkdir(parents=True, exist_ok=True)
    config = host('llvm-config')
    version = subprocess.check_output([config, '--version'], text=True).strip()
    if version.split('.')[0] != '23':
        raise RuntimeError('AIR import currently requires LLVM 23 development headers/libraries and llvm-as')
    flags = shlex.split(subprocess.check_output([config, '--cxxflags', '--ldflags', '--libs',
                                               'core', 'irreader', 'bitreader', 'analysis'], text=True))
    inputs = {'source_sha256':digest(source), 'semantic_sha256':digest(TOOLS/'gpu-compiler/Semantic.h'),
              'control_flow_sha256':digest(TOOLS/'gpu-compiler/AIRControlFlow.h'),
              'library_archive_sha256':digest(TOOLS/'gpu-compiler/LibraryArchive.h'),
              'function_constants_sha256':digest(TOOLS/'gpu-compiler/FunctionConstants.h'),
              'air_specialization_sha256':digest(TOOLS/'gpu-compiler/AIRSpecialization.h'),
              'interface_sha256':digest(DRIVERS_ROOT/'include/GPUShaderInterface.h'),
              'protocol_sha256':digest(DRIVERS_ROOT/'include/GPUShaderProtocol.h'),
              'user_protocol_sha256':digest(DRIVERS_ROOT/'include/GPUUserProtocol.h'), 'llvm_version':version, 'flags':flags}
    exe = root / 'air-frontend'; stamp = root / 'air-frontend.json'
    if not (exe.exists() and stamp.exists() and json.loads(stamp.read_text()).get('inputs') == inputs and
            json.loads(stamp.read_text()).get('sha256') == digest(exe)):
        log = root / 'air-build.log'; log.unlink(missing_ok=True)
        temporary = root / 'air-frontend.tmp'
        run([host('clang++'), source, *flags, '-std=c++17', '-fexceptions', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-parameter', '-I'+str(DRIVERS_ROOT/'include'), '-O2', '-o', temporary], log=log)
        temporary.replace(exe)
        stamp.write_text(json.dumps({'inputs':inputs, 'sha256':digest(exe)},indent=2)+'\n')
    return exe, version


def prepare_gpu_jit_compiler(mesa):
    """In-process iOS shader compiler; share the pinned AOT Mesa checkout."""
    root = BUILD / 'gpu-compiler'; venv = root / 'venv'
    sdk = TOOLS / 'sdk/iPhoneOS18.6.sdk'
    # The AIR builder uses LLVM's exported C ABI inside the compiler dylib.
    # Target headers must never put host libc headers on an iOS include path.
    llvm_include = Path(subprocess.check_output([host('llvm-config'),'--includedir'],text=True).strip())
    llvm_headers = root/'include'
    shutil.copytree(llvm_include/'llvm-c',llvm_headers/'llvm-c',dirs_exist_ok=True)
    shutil.copytree(llvm_include/'llvm/Config',llvm_headers/'llvm/Config',dirs_exist_ok=True)
    options = mesa / 'src/freedreno/ir3/ir3_compiler.h'
    text = options.read_text().replace('struct ir3_compiler_options {',
        'struct ir3_compiler_options {\n   bool qcom_runtime; // No environment shader overrides in the trusted memory API.',1)
    if 'bool qcom_runtime;' not in options.read_text(): options.write_text(text)
    compiler_source = mesa / 'src/freedreno/ir3/ir3_compiler.c'
    text = compiler_source.read_text().replace('ir3_shader_debug = debug_get_option_ir3_shader_debug();',
        'ir3_shader_debug = options->qcom_runtime ? 0 : debug_get_option_ir3_shader_debug();').replace(
        '__normal_user() ? debug_get_option_ir3_shader_override_path() : NULL;',
        '(!options->qcom_runtime && __normal_user()) ? debug_get_option_ir3_shader_override_path() : NULL;')
    if compiler_source.read_text() != text: compiler_source.write_text(text)
    cache = mesa / 'src/freedreno/ir3/ir3_disk_cache.c'
    text = cache.read_text()
    if '#ifdef ENABLE_SHADER_CACHE\n   if (ir3_shader_debug' not in text:
        text = text.replace('   if (ir3_shader_debug & IR3_DBG_NOCACHE)',
                            '#ifdef ENABLE_SHADER_CACHE\n   if (ir3_shader_debug & IR3_DBG_NOCACHE)',1)
        text = text.replace('   compiler->disk_cache = disk_cache_create(renderer, timestamp, driver_flags);',
                            '   compiler->disk_cache = disk_cache_create(renderer, timestamp, driver_flags);\n#else\n   (void)compiler;\n#endif',1)
        cache.write_text(text)
    standalone = mesa / 'src/compiler/glsl/standalone.cpp'
    text = standalone.read_text()
    if 'qgpu_standalone_source' not in text:
        signature = 'standalone_compile_shader(const struct standalone_options *_options,\n      unsigned num_files, char* const* files, struct gl_context *ctx)'
        replacement = 'qgpu_standalone_inputs(const struct standalone_options *_options,\n      unsigned num_files, char* const* files, const char *const *sources,\n      struct gl_context *ctx, char *error, unsigned error_bytes)'
        if signature not in text: raise RuntimeError('Mesa standalone compiler signature changed')
        text = text.replace('extern "C" struct gl_shader_program *\n'+signature,
                            'static struct gl_shader_program *\n'+replacement,1)
        text = text.replace('const char *source = load_text_file(whole_program, files[i]);',
                            'const char *source = sources ? sources[i] : load_text_file(whole_program, files[i]);',1)
        text = text.replace('if (strlen(shader->InfoLog) > 0) {',
                            'if (error && strlen(shader->InfoLog)) snprintf(error, error_bytes, "%s", shader->InfoLog);\n      if (!error && strlen(shader->InfoLog) > 0) {',1)
        text = text.replace('if (strlen(whole_program->data->InfoLog) > 0) {',
                            'if (error && strlen(whole_program->data->InfoLog)) snprintf(error, error_bytes, "%s", whole_program->data->InfoLog);\n      if (!error && strlen(whole_program->data->InfoLog) > 0) {',1)
        text += '''
extern "C" struct gl_shader_program *
standalone_compile_shader(const struct standalone_options *options,
      unsigned count, char *const *files, struct gl_context *ctx) {
   return qgpu_standalone_inputs(options, count, files, NULL, ctx, NULL, 0);
}
extern "C" struct gl_shader_program *
qgpu_standalone_source(const char *source, unsigned stage, struct gl_context *ctx,
      char *error, unsigned error_bytes) {
   static const struct standalone_options opts = {430, 0, 0, 0, 1, 1, 0};
   char *files[] = {const_cast<char *>(stage == 0 ? "memory.comp" : stage == 1 ? "memory.vert" : "memory.frag")};
   const char *sources[] = {source};
   return qgpu_standalone_inputs(&opts, 1, files, sources, ctx, error, error_bytes);
}
'''
        if replacement not in text: raise RuntimeError('Mesa memory compiler insertion failed')
        standalone.write_text(text)
    compiler_meson = mesa / 'src/compiler/meson.build'
    text = compiler_meson.read_text().replace("if with_gallium\n  subdir('glsl')", "if with_gallium or with_tools.contains('freedreno')\n  subdir('glsl')")
    if compiler_meson.read_text() != text: compiler_meson.write_text(text)
    for name in ['Backend.c','AIRNIR.cpp']:
        shutil.copyfile(TOOLS / 'gpu-compiler' / name, mesa / 'src/freedreno' / ('Qcom' + name))
    for name in ['AIRRuntime.cpp','AIRNIR.h','AIRNIRLowering.h','AIRSpecialization.h','AIRControlFlow.h','FunctionConstants.h','Semantic.h']:
        shutil.copyfile(TOOLS/'gpu-compiler'/name,mesa/'src/freedreno'/name)
    shutil.copyfile(TOOLS / 'gpu-compiler/Interface.h', mesa / 'src/freedreno/QcomInterface.h')
    # Compiler.h includes the shared kernel wire description, not an IOKit ABI.
    shutil.copyfile(TOOLS / 'gpu-compiler/Compiler.h', mesa / 'src/freedreno/Compiler.h')
    for name in ['GPUJITProtocol.h','GPUShaderInterface.h']:
        shutil.copyfile(DRIVERS_ROOT / 'include' / name, mesa / 'src/freedreno' / name)
    meson_file = mesa / 'src/freedreno/meson.build'
    text = meson_file.read_text()
    marker = '\n# Project adapter: only compiler libraries'
    text = text.split(marker)[0]
    text += '''
# Project adapter: only compiler libraries; no Gallium device/DRM backend.
qcom_llvm = host_machine.system() == 'darwin' ? cpp.find_library('LLVM') : cpp.find_library('LLVM-23')
qcom_gpu_compiler = shared_library('QcomGPUCompiler', ['QcomBackend.c', 'QcomAIRNIR.cpp'],
  name_prefix: '', gnu_symbol_visibility: 'hidden',
  cpp_args: ['-fexceptions', '-Wno-deprecated-declarations'],
  link_args: host_machine.system() == 'darwin' ? ['-Wl,-install_name,@loader_path/QcomGPUCompiler.dylib', '-Wl,-adhoc_codesign'] : [],
  include_directories: [inc_freedreno, inc_include, inc_src, inc_mesa, inc_mapi, inc_gallium, inc_gallium_aux,
    include_directories('@QCOM_LLVM_HEADERS@'), include_directories('@QCOM_PROTOCOL_HEADERS@')],
  dependencies: [idep_nir, idep_mesautil, qcom_llvm],
  link_with: [libfreedreno_ir3, libglsl_standalone],
  build_by_default: false)
'''.replace('@QCOM_LLVM_HEADERS@',str(llvm_headers)).replace('@QCOM_PROTOCOL_HEADERS@',str(DRIVERS_ROOT/'include'))
    if meson_file.read_text() != text: meson_file.write_text(text)
    cross = root / 'ios.ini'
    # Direct Clang invocation keeps Meson probes and final objects on the same
    # target and SDK; build generators still use the native C/C++ compilers.
    c = [compiler(), '--target=arm64-apple-ios18.6', '-isysroot', str(sdk), *compiler_resource()]
    cpp = [*c, '--driver-mode=g++', '-stdlib=libc++']
    content = "[binaries]\nc = " + repr(c) + "\ncpp = " + repr(cpp) + "\nobjc = " + repr(c) + "\nar = '" + host('llvm-ar') + "'\nstrip = '" + host('llvm-strip') + "'\n"
    content += "c_ld = '" + host('ld64.lld') + "'\ncpp_ld = '" + host('ld64.lld') + "'\npkg-config = 'false'\n[host_machine]\nsystem = 'darwin'\ncpu_family = 'aarch64'\ncpu = 'arm64'\nendian = 'little'\n[properties]\nneeds_exe_wrapper = true\n"
    content += "[built-in options]\nc_args = ['-mcpu=generic', '-march=armv8-a+crc+crypto']\ncpp_args = ['-mcpu=generic', '-march=armv8-a+crc+crypto']\nobjc_link_args = ['-fuse-ld="+host('ld64.lld')+"', '-Wl,-platform_version,ios,18.6,18.6']\nc_link_args = ['-fuse-ld="+host('ld64.lld')+"', '-Wl,-platform_version,ios,18.6,18.6']\ncpp_link_args = ['-fuse-ld="+host('ld64.lld')+"', '-Wl,-platform_version,ios,18.6,18.6']\n"
    if not cross.exists() or cross.read_text()!=content: cross.write_text(content)
    obj = root / 'ios'; environment = dict(os.environ)
    environment['PATH'] = str(venv / 'bin') + os.pathsep + environment.get('PATH','')
    log = root / 'jit-build.log'
    if not (obj / 'build.ninja').exists():
        run([venv / 'bin/meson', 'setup', obj, mesa, '--cross-file', cross, '--buildtype=release', '--wrap-mode=nofallback',
             '-Dgallium-drivers=', '-Dfreedreno-kmds=', '-Dtools=freedreno',
             '-Dplatforms=', '-Dvulkan-drivers=', '-Dllvm=disabled', '-Dglx=disabled',
             '-Degl=disabled', '-Dgles1=disabled', '-Dgles2=disabled', '-Dgbm=disabled',
             '-Dglvnd=disabled', '-Dgallium-va=disabled', '-Dgallium-vdpau=disabled',
             '-Dgallium-xa=disabled', '-Dbuild-tests=false', '-Dlibunwind=disabled',
             '-Dshader-cache=disabled', '-Dvalgrind=disabled', '-Dzstd=disabled', '-Dexpat=disabled'],
             env=environment, log=log)
    # Meson caches machine-file built-in options at setup. Apply this profile
    # explicitly when upgrading an existing compiler build directory too.
    profile = ['-mcpu=generic','-march=armv8-a+crc+crypto']
    build_options = json.loads(subprocess.check_output([venv/'bin/meson','introspect','--buildoptions',obj],text=True))
    current = {item['name']:item['value'] for item in build_options}
    if current.get('c_args') != profile or current.get('cpp_args') != profile:
        run([venv/'bin/meson','configure',obj,'-Dc_args='+ ' '.join(profile),'-Dcpp_args='+ ' '.join(profile)],env=environment,log=log)
    run([host('ninja'), '-C', obj, '-j', str(JOBS), 'src/freedreno/QcomGPUCompiler.dylib'], env=environment, log=log)
    # The offline native probes use the same adapter as the iOS compiler.
    # Refresh its host library here too, after synchronizing the shared C
    # sources/headers, so callers cannot accidentally validate a stale backend.
    host_obj=root/'host'
    if (host_obj/'build.ninja').exists():
        run([host('ninja'),'-C',host_obj,'-j',str(JOBS),'src/freedreno/QcomGPUCompiler.so'],env=environment,log=root/'jit-host-build.log')
    # The target now contains Mesa code. Consolidate source/header notices
    # from the actual compiler build alongside the adapter's attribution.
    paths = set()
    for item in json.loads((obj/'compile_commands.json').read_text()):
        if (obj/item['output']).exists(): paths.add((obj/item['file']).resolve())
    dependencies = subprocess.check_output([host('ninja'),'-C',obj,'-t','deps'],text=True)
    for line in dependencies.splitlines():
        if line.startswith('    '): paths.add((obj/line.strip()).resolve())
    notices = {}
    for path in sorted(paths):
        if not path.is_file() or not path.is_relative_to(mesa): continue
        with path.open(errors='replace') as stream: prefix = stream.read(16384)
        header = re.match(r'\A((?:\s*(?:/\*[\s\S]*?\*/|//[^\n]*(?:\n|$)))+)',prefix)
        if header and re.search(r'copyright|SPDX-License|permission|license',header[1],re.I):
            notices.setdefault(header[1].strip(),[]).append(str(path.relative_to(mesa)))
    notice = (TOOLS/'gpu-compiler/NOTICE.txt').read_text()
    notice += '\nConsolidated notices from the pinned Mesa compiler build:\n'
    for header, names in notices.items(): notice += '\n'+', '.join(names)+'\n'+header+'\n'
    (root/'NOTICE.Mesa.txt').write_text(notice)
    libraries = [obj / 'src/freedreno/QcomGPUCompiler.dylib']
    llvm_exports = (sdk/'usr/lib/libLLVM.tbd').read_text()
    llvm_symbols = sorted(set(re.findall(r'\b(_LLVM\w+)\b',subprocess.check_output(
        [host('llvm-nm'),'--undefined-only',libraries[0]],text=True))))
    for symbol in llvm_symbols:
        if not re.search(r'\b'+re.escape(symbol)+r'\b',llvm_exports):
            raise RuntimeError('iOS LLVM lacks direct AIR compiler C export '+symbol)
    report = {'target':'arm64-apple-ios18.6','cpu_isa':'armv8-a+crc+crypto','mesa_commit':json.loads((TOOLS/'gpu-compiler/Source.json').read_text())['commit'],
              'runtime_llvm_c_exports':llvm_symbols,
              'hardware_tested':False, 'libraries':{str(p.relative_to(ROOT)):digest(p) for p in libraries},
              'sources':{str(p.relative_to(ROOT)):digest(p) for p in [SELF,TOOLS/'gpu-compiler/Source.json',
                  TOOLS/'gpu-compiler/Backend.c',TOOLS/'gpu-compiler/Interface.h',TOOLS/'gpu-compiler/Compiler.h',
                  TOOLS/'gpu-compiler/AIRNIR.cpp',TOOLS/'gpu-compiler/AIRNIR.h',TOOLS/'gpu-compiler/AIRNIRLowering.h',
                  DRIVERS_ROOT/'include/GPUJITProtocol.h',DRIVERS_ROOT/'include/GPUShaderInterface.h',
                  standalone,mesa/'src/compiler/glsl/gl_nir_linker.c',mesa/'src/freedreno/ir3/ir3_a4xx.c',compiler_source,options,cache,compiler_meson,meson_file]}}
    (root/'jit-compiler.json').write_text(json.dumps(report,indent=2)+'\n')
    return libraries


def test_gpu_compiler():
    """One host-only check of the production memory compiler and boundaries."""
    root = BUILD/'gpu-compiler'; obj = root/'host'; exe = root/'jit-test'
    library = obj/'src/freedreno/QcomGPUCompiler.so'
    run([host('ninja'),'-C',obj,'-j',str(JOBS),'src/freedreno/QcomGPUCompiler.so'],log=root/'jit-host-build.log')
    source = ROOT/'tests/gpu_compiler_test.cpp'
    compiler_sources = [TOOLS/'gpu-compiler'/name for name in ['MSL.cpp','AIRRuntime.cpp','Compiler.cpp']]
    flags = shlex.split(subprocess.check_output([host('llvm-config'),'--ldflags','--libs','core','irreader','analysis'],text=True))
    run([host('clang++'),'-std=c++17','-O2','-Wall','-Wextra','-Werror',
         '-I'+str(DRIVERS_ROOT/'include'),'-I'+str(TOOLS/'gpu-compiler'),'-I'+str(BUILD/'drivers/generated/GPU'),
         '-I'+subprocess.check_output([host('llvm-config'),'--includedir'],text=True).strip(),source,*compiler_sources,
         library,*flags,'-Wl,-rpath,'+str(library.parent),'-o',exe],log=root/'jit-host-build.log')
    result = subprocess.run([exe,ROOT],capture_output=True,text=True)
    (root/'jit-check.json').write_text(json.dumps({'hardware_tested':False,'returncode':result.returncode,
        'stdout':result.stdout,'stderr':result.stderr,
        'lowered_ios18_display_glsl':{name:digest(root/name) for name in
            ('display-math-f32.frag','display-math-half.frag') if (root/name).is_file()},
        'lowered_half_glsl':{name:digest(root/name) for name in ('half-memory-runtime.comp',) if (root/name).is_file()},
        'lowered_control_flow_glsl':{name:digest(root/name) for name in
            ('flow-runtime.comp','flow-diamond.comp','flow-scopes.comp') if (root/name).is_file()},
        'lowered_msaa_glsl':{name:digest(root/name) for name in
            ('resolve_color.comp','multisample_fragment.frag','multisample_vertex.vert') if (root/name).is_file()},
        'lowered_fragment_glsl':{name:digest(root/name) for name in
            ('raster_depth_only.frag','depth_fragment.frag','depth_only_fragment.frag','discard_depth_fragment.frag','discard_only_fragment.frag',
             'air_depth_fragment.frag','air_depth_only_fragment.frag','air_discard_fragment.frag','air_void_fragment.frag') if (root/name).is_file()},
        'lowered_mrt_glsl':{name:digest(root/name) for name in
            ('mrt_fragment.frag','all_colors_fragment.frag','color_only_fragment.frag') if (root/name).is_file()},
        'lowered_constant_sampler_glsl':{name:digest(root/name) for name in
            ('constant_sample.comp','constant_sample_fragment.frag','constant_cube_vertex.vert','constant_array_fragment.frag') if (root/name).is_file()},
        'lowered_compute_glsl':{name:digest(root/name) for name in
            ('compute-runtime.comp','compute-uint2.comp','nonuniform-runtime.comp') if (root/name).is_file()},
        'lowered_array_glsl':{name:digest(root/name) for name in
            ('array-copy-runtime.comp','array-sample-runtime.comp','array-gradient-runtime.comp','array-render-runtime.frag','array-implicit-runtime.frag','array-query-runtime.comp') if (root/name).is_file()},
        'lowered_cube_image_glsl':{name:digest(root/name) for name in
            ('cube_copy.comp','cube_array_copy.comp','cube_image_queries.comp','cube_image_update.comp') if (root/name).is_file()},
        'lowered_cube_glsl':{name:digest(root/name) for name in
            ('cube_textured.frag','cube_array_textured.frag','cube_sampled_vertex.vert','cube_sample.comp') if (root/name).is_file()},
        'lowered_graphics_texture_glsl':{name:digest(root/name) for name in
            ('graphics-textures-msl.vert','graphics-textures-msl.frag','graphics-textures-air.vert','graphics-textures-air.frag','graphics-textures-eight.vert','graphics-textures-eight.frag') if (root/name).is_file()},
        'lowered_air_texture_glsl':{name:digest(root/name) for name in
            ('air_fetch_vertex.vert','air_fetch_cube_fragment.frag','air_fetch_cube_array_fragment.frag',
             'air_array_image_transform.comp','air_cube_image_copy.comp','air_cube_array_sample_vertex.vert',
             'air_array_sample_fragment.frag','air_cube_sample_fragment.frag') if (root/name).is_file()},
        'lowered_vertex_texture_glsl':{name:digest(root/name) for name in
            ('vertex-texture-msl-runtime.vert','vertex-texture-array-runtime.vert','vertex-texture-air-runtime.vert','vertex-texture-implicit-runtime.vert','vertex-texture-gradient-runtime.vert') if (root/name).is_file()},
        'lowered_dynamic_threadgroup_glsl':{name:digest(root/name) for name in
            ('dynamic-runtime.comp','dynamic-atomic.comp','dynamic-air-atomic.comp') if (root/name).is_file()},
        'lowered_mipmap_glsl':{name:digest(root/name) for name in
            ('mip-runtime.frag','mip-bias.frag','mip-bias-runtime.frag','mipmap-runtime.comp') if (root/name).is_file()},
        'lowered_sampler_glsl':{name:digest(root/name) for name in
            ('sampler-gradient-runtime.comp','sampler-gradient-runtime.frag','sampler-mixed.frag') if (root/name).is_file()},
        'lowered_compute_sampling_glsl':{name:digest(root/name) for name in
            ('sampling-runtime.comp','sampling-pairs.comp') if (root/name).is_file()},
        'lowered_compute_image_glsl':{name:digest(root/name) for name in
            ('images-runtime.comp','images-rw.comp','images-query.comp') if (root/name).is_file()},
        'inputs':{str(p.relative_to(ROOT)):digest(p) for p in [source,*compiler_sources,library,
            TOOLS/'gpu-compiler/Semantic.h',TOOLS/'gpu-compiler/AIRControlFlow.h',TOOLS/'gpu-compiler/FunctionConstants.h',TOOLS/'gpu-compiler/AIRSpecialization.h',TOOLS/'gpu-compiler/Compiler.h',
            DRIVERS_ROOT/'include/GPUShaderInterface.h',DRIVERS_ROOT/'include/GPUShaderProtocol.h',
            DRIVERS_ROOT/'include/GPUJITProtocol.h',BUILD/'drivers/generated/GPU/GPUPrograms.h',
            ROOT/'drivers/gpu/QcomMTL/Shaders.metal',ROOT/'drivers/gpu/QcomMTL/Shaders.air.ll',
            ROOT/'drivers/gpu/QcomMTL/Textures.air.ll',
            ROOT/'tests/fixtures/gpu/static-switch.air.ll',ROOT/'tests/fixtures/gpu/packed-fluidity.air.ll',
            ROOT/'drivers/gpu/QcomMTL/SampleShading.metal',ROOT/'drivers/gpu/QcomMTL/SampleShading.air.ll',
            ROOT/'drivers/gpu/QcomMTL/Resolve.metal',ROOT/'drivers/gpu/QcomMTL/Multisample.metal',ROOT/'drivers/gpu/QcomMTL/Depth.metal',ROOT/'drivers/gpu/QcomMTL/Mipmaps.metal',ROOT/'drivers/gpu/QcomMTL/Compositor.metal',BUILD/'drivers/generated/GPU/Shaders.air',
            BUILD/'drivers/generated/GPU/Textures.air']}},indent=2)+'\n')
    if result.returncode: raise RuntimeError('GPU memory compiler check failed: '+result.stderr)
    return result.stdout


def extract_gpu_metallib(data):
    """Use the same bounded FAT/MTLB parser as Metal's in-process loader."""
    if not data or len(data) > 16*1024*1024: raise RuntimeError('invalid or oversized metallib header')
    compiler, _ = prepare_gpu_air_compiler()
    result = subprocess.run([compiler, '--library-index'], input=data, capture_output=True)
    if result.returncode: raise RuntimeError(result.stderr.decode(errors='replace').strip())
    records = json.loads(result.stdout)
    for record in records:
        record['hash'] = bytes.fromhex(record['hash'])
        record['data'] = data[record['offset']:record['offset']+record['size']]
    return records

def translate_gpu_air(path, entries, compiler, work):
    data = path.read_bytes()
    if not data or len(data) > 16*1024*1024: raise RuntimeError('AIR input must be 1 byte..16 MiB')
    if entries and (len(entries) != len(set(entries)) or any(not isinstance(n,str) or not n for n in entries)):
        raise RuntimeError('invalid or duplicate selected AIR entries')
    key = hashlib.sha256(data).hexdigest()[:24]
    aliases = [path]; modules = []
    if data[:4] in (b'MTLB',b'\xca\xfe\xba\xbe',b'\xbe\xba\xfe\xca',b'\xca\xfe\xba\xbf',b'\xbf\xba\xfe\xca'):
        records = extract_gpu_metallib(data)
        if entries and set(entries) - {r['name'] for r in records}: raise RuntimeError('selected metallib function is absent')
        for index,record in enumerate(records):
            if entries and record['name'] not in entries: continue
            if record['type'] not in (0,1,2): raise RuntimeError('metallib contains non-stage functions; select supported entries explicitly')
            module = work / (key+'-'+str(index)+'.air'); module.write_bytes(record['data'])
            modules.append((module,[record['name']],{0:1,1:2,2:0}[record['type']]))
    elif path.suffix == '.ll':
        module = work / (key+'.air')
        run([host('llvm-as'), path, '-o', module])
        modules.append((module,entries,None)); aliases.append(module)
    else:
        if data[:4] not in (b'BC\xc0\xde',b'\xde\xc0\x17\x0b'): raise RuntimeError('input is not LLVM AIR bitcode or metallib')
        modules.append((path,entries,None))
    functions = []
    for index,(module,selected,expected_stage) in enumerate(modules):
        output = work / (key+'-'+str(index)+'.json'); output.unlink(missing_ok=True)
        run([compiler,module,output,*selected])
        lowered = json.loads(output.read_text())
        if expected_stage is not None and any(f['stage'] != expected_stage for f in lowered):
            raise RuntimeError('metallib TYPE disagrees with AIR stage metadata')
        functions.extend(lowered)
    return functions, aliases


def build_gpu_shaders():
    root = ROOT / 'drivers/gpu/QcomMTL'
    config = root / 'ShaderBuild.json'
    settings = json.loads(config.read_text())
    if settings['gpu'] != 540 or settings.get('local_size') != 'variable': raise RuntimeError('unsupported shader profile')
    definitions = [(item,[]) if isinstance(item,str) else (item['path'],item.get('entries',[])) for item in settings['sources']]
    sources = [(root / name).resolve() for name,_ in definitions]
    inputs = [config, *sources, TOOLS / 'gpu-compiler/Export.h', TOOLS / 'gpu-compiler/Interface.h',
              TOOLS / 'gpu-compiler/MSL.cpp', TOOLS / 'gpu-compiler/MSLMain.cpp', TOOLS / 'gpu-compiler/Compiler.h',
              TOOLS / 'gpu-compiler/Semantic.h', TOOLS / 'gpu-compiler/AIRControlFlow.h', TOOLS / 'gpu-compiler/LibraryArchive.h', TOOLS / 'gpu-compiler/FunctionConstants.h', TOOLS / 'gpu-compiler/AIRSpecialization.h', TOOLS / 'gpu-compiler/Source.json', TOOLS / 'gpu-compiler/AIR.cpp', SELF,
              DRIVERS_ROOT / 'include/GPUShaderProtocol.h', DRIVERS_ROOT / 'include/GPUShaderInterface.h', DRIVERS_ROOT / 'include/GPUJITProtocol.h']
    generated = BUILD / 'drivers/generated/GPU'
    manifest = generated / 'shaders.json'; header = generated / 'GPUPrograms.h'
    hashes = {str(p.relative_to(ROOT)):digest(p) for p in inputs}
    if manifest.exists() and header.exists():
        previous = json.loads(manifest.read_text())
        cached = previous.get('generated_sha256', {})
        expected_files = {'GPUPrograms.h','A5xxRegs.h','default.qmetallib'} | set(previous.get('air_libraries',[]))
        if previous.get('inputs') == hashes and set(cached) == expected_files and all((generated / name).exists() and
                digest(generated / name) == value for name,value in cached.items()):
            return generated
    air_compiler = None; llvm_version = None
    if any(path.suffix != '.metal' for path in sources): air_compiler,llvm_version = prepare_gpu_air_compiler()
    compiler, mesa = prepare_gpu_compiler()
    generate_gpu_registers(mesa, generated)
    programs = []; names = set(); work = BUILD / 'gpu-compiler/shaders'; work.mkdir(parents=True, exist_ok=True)
    air_work = BUILD / 'gpu-compiler/air'; air_work.mkdir(parents=True,exist_ok=True)
    binary_libraries = []; generated_libraries = {}
    for path,(_,selected) in zip(sources,definitions):
        if path.suffix == '.metal':
            functions = translate_gpu_msl(path.read_text())
            if selected: raise RuntimeError('MSL entry selection is not supported')
        else:
            functions,aliases = translate_gpu_air(path,selected,air_compiler,air_work)
            for alias in aliases:
                binary_libraries.append({'sha256':digest(alias),'bytes':alias.stat().st_size,
                    'names':[f['name'] for f in functions],'format':'metallib' if path.suffix=='.metallib' else 'air'})
            if path.suffix == '.ll':
                name = path.name[:-3]
                if not name.endswith('.air'): name += '.air'
                generated_libraries[name] = aliases[-1]
        for function in functions:
            if function['name'] in names: raise RuntimeError('duplicate shader entry point ' + function['name'])
            names.add(function['name']); identifier = len(programs) + 1
            glsl = work / (str(identifier) + '.' + function['extension'])
            glsl.write_text(function.pop('glsl'))
            variants = []
            for msaa in ([False, True] if function['stage']==2 else [False]):
                suffix = '.msaa' if msaa else ''
                output = work / (str(identifier)+suffix+'.json')
                output.unlink(missing_ok=True)
                compiler_log = work / (str(identifier)+suffix+'.log')
                compiler_log.unlink(missing_ok=True)
                env = {k:v for k,v in os.environ.items() if not k.startswith(('IR3_','FD_MESA_','QCOM_IR3_'))}
                env['QCOM_IR3_OUTPUT'] = str(output)
                if msaa: env['QCOM_IR3_MSAA'] = '1'
                run([compiler, '-g', '540', glsl], log=compiler_log, env=env)
                result = json.loads(output.read_text()); result['metadata'].update({
                    'id':identifier, 'name':function['name'], 'bufferMask':function['bufferMask'],
                    'writeMask':function['writeMask'], 'strides':function['strides'], 'instanceMask':function['instanceMask'],
                    'textureBindings':function.get('textureBindings',0),
                    'imageUintMask':function.get('imageUintMask',0)&result['metadata']['imageMask']})
                m=result['metadata'];m['depthTextureMask']=function.get('depthTextureMask',0)&(m['imageMask']|m['sampleMask']|m['fetchMask']|m['queryMask'])
                if m['framebufferMask'] & ~function.get('framebufferMask',0):
                    raise RuntimeError('compiler framebuffer input exceeds its source declarations')
                declarations = function['varyings']
                if len(declarations)>32 or result['metadata']['varyingCount']>len(declarations):
                    raise RuntimeError('compiler stage interface exceeds its source declarations')
                merged = []
                for index in range(32):
                    if index>=len(declarations):
                        merged.append({'semantic':[0]*4,'components':0,'mode':0,'regid':0,'mask':0,'inloc':0,'reserved':0})
                        continue
                    decl = declarations[index]; hardware = result['metadata']['varyings'][index]
                    if hardware['components'] and (hardware['components'] != decl['components'] or
                            (function['stage']==2 and hardware['mode'] != decl['mode'])):
                        raise RuntimeError('compiler stage interface disagrees with source declarations')
                    merged.append({**hardware,**decl,'reserved':0})
                result['metadata']['varyings'] = merged
                result['metadata']['varyingCount'] = len(declarations)
                variants.append(result)
            result = variants[0]
            if len(variants)>1: result['msaa'] = variants[1]
            result['source'] = str(path.relative_to(ROOT)); result['source_format'] = 'msl' if path.suffix=='.metal' else 'air'
            programs.append(result)
    if not programs or len(programs) > 256: raise RuntimeError('invalid AOT program count')
    registry = int.from_bytes(hashlib.sha256(json.dumps(programs,sort_keys=True).encode()).digest()[:8], 'little')
    fields = ['id','stage','codeWords','instrlen','constlen','fullRegs','halfRegs','branchStack',
        'threadSize','subgroupSize','immediateBase','immediateCount','driverBase','driverCount',
        'localID','groupID','vertexID','position','color','fragCoord','fragMask','bufferMask','writeMask','textureCount','flags','instanceID','instanceMask',
        'varyingCount','totalIn','perspective','linear','centerRHW','sharedBytes','threadgroupMask','imageMask','imageReadMask','imageWriteMask','imageUintMask','sampleMask','samplerMask','explicitSamplerMask','cubeSamplerMask','fetchMask','queryMask']
    sampler_fields = ['implicitSamplerMask','constantSamplerMask','constantImplicitSamplerMask','constantExplicitSamplerMask','constantCubeSamplerMask','framebufferMask']
    sampler_state_fields = ['filters','minLOD','maxLOD','addressModes','maxAnisotropy','borderColor','normalized']
    lines = ['// Generated by scripts/toolchain.py build-gpu-shaders. Do not edit.',
        '// Shader machine code generated by Mesa IR3 (MIT); see tools/gpu-compiler/Source.json.',
        '#pragma once', '#include <GPUShaderProtocol.h>', 'namespace GPUPrograms {',
        f'constexpr uint64_t Registry = 0x{registry:016x}ULL;']
    sample_fields = ['sampleFlags','sampleID','sampleMaskIn','sampleMaskOut','perspectiveCentroid','linearCentroid','perspectiveSample','linearSample','textureBindings']
    def program_initializer(program, symbol, variant='nullptr'):
        m = program['metadata']
        interface = []
        for v in m['varyings']:
            interface.append('{{'+','.join(map(str,v['semantic']))+'},'+','.join(str(v[f]) for f in ['components','mode','regid','mask','inloc','reserved'])+'}')
        return '{'+','.join(str(m[f]) for f in fields)+',{'+','.join(map(str,m['strides']))+'},{'+','.join(interface)+'},'+str(m['attributeMask'])+',{'+','.join('{'+','.join(str(a[f]) for f in ['components','type','regid','mask'])+'}' for a in m['attributes'])+'},'+','.join(str(m[f]) for f in sampler_fields)+',{'+','.join(map(str,m['constantSamplerSlots']))+'},{'+','.join(map(str,m['constantSamplerLevelSlots']))+'},{'+','.join('{'+','.join(str(a[f]) for f in sampler_state_fields)+'}' for a in m['constantSamplers'])+'},'+str(m['colorMask'])+',{'+','.join(map(str,m['colorRegisters']))+'},'+str(m['depthReg'])+','+str(m['multisampleMask'])+','+str(m['depthTextureMask'])+','+','.join(str(m[f]) for f in sample_fields)+','+str(m['constantWindowCount'])+',{'+','.join('{'+str(v['buffer'])+','+str(v['byteOffset'])+'}' for v in m['constantWindows'])+'},Code'+symbol+',Immediates'+symbol+','+json.dumps(m['name'])+','+variant+'}'
    for owner in programs:
        for suffix,program in [('',owner)]+([('MSAA',owner['msaa'])] if 'msaa' in owner else []):
            symbol=str(program['metadata']['id'])+suffix
            lines.append('static const uint32_t Code'+symbol+'[] = {'+','.join(hex(v) for v in program['code'])+'};')
            lines.append('static const uint32_t Immediates'+symbol+'[] = {'+','.join(hex(v) for v in (program['immediates'] or [0]))+'};')
            if suffix: lines.append('static const GPU::Shader::Program MSAA'+str(owner['metadata']['id'])+' = '+program_initializer(program,symbol)+';')
    lines.append('static const GPU::Shader::Program Programs[] = {')
    for program in programs:
        symbol=str(program['metadata']['id'])
        lines.append(program_initializer(program,symbol,'&MSAA'+symbol if 'msaa' in program else 'nullptr')+',')
    lines += ['};','inline const GPU::Shader::Program *lookup(uint32_t id) {',
              'return id && id <= sizeof(Programs)/sizeof(Programs[0]) ? &Programs[id-1] : nullptr;', '}']
    library = {'schema':'QcomMTLLibrary','version':1,'registry':f'{registry:016x}',
        'sources':[{ 'source':p.read_text(), 'names':[v['metadata']['name'] for v in programs if v['source']==str(p.relative_to(ROOT))]} for p in sources if p.suffix=='.metal'],
        'binaries':binary_libraries,
        'functions':[{'id':p['metadata']['id'],'name':p['metadata']['name'],'stage':p['metadata']['stage']} for p in programs]}
    blob = json.dumps(library,sort_keys=True,separators=(',',':'))
    lines += ['static const char Library[] = '+json.dumps(blob)+';', '}']
    generated.mkdir(parents=True,exist_ok=True); header.write_text('\n'.join(lines)+'\n')
    (generated / 'default.qmetallib').write_text(blob)
    for name,path in generated_libraries.items(): shutil.copyfile(path,generated / name)
    manifest.write_text(json.dumps({'inputs':hashes,'registry':f'{registry:016x}', 'programs':programs,
        'generated_sha256':{name:digest(generated / name) for name in ['GPUPrograms.h','A5xxRegs.h','default.qmetallib',*generated_libraries]},
        'compiler_commit':json.loads((TOOLS / 'gpu-compiler/Source.json').read_text())['commit'],
        'air_llvm_version':llvm_version, 'air_libraries':list(generated_libraries),
        'hardware_tested':False},indent=2)+'\n')
    print('Compiled '+str(len(programs))+' MSL/AIR entries to real A540 IR3; registry '+f'{registry:016x}')
    return generated


def generate_gpu_registers(mesa, generated):
    """Keep Mesa's integer register encodings, including address-unit shifts."""
    registers = mesa / 'src/freedreno/registers'
    text = subprocess.check_output([sys.executable, registers / 'gen_header.py', '--rnn', registers,
        '--xml', registers / 'adreno/a5xx.xml', 'c-defines'],text=True)
    notice = re.search(r'/\* Autogenerated.*?\*/',text,re.S)
    output = ['#pragma once', '#include <stdint.h>', notice[0] if notice else '']
    output += [line for line in text.splitlines() if re.match(r'#define (REG_A5XX_|A5XX_)',line)]
    for match in re.finditer(r'static inline uint32_t (REG_A5XX_\w+)\(([^)]*)\)\s*\{\s*return ([^;]+);\s*\}',text):
        args = ','.join(part.strip().split()[-1] for part in match[2].split(',') if part.strip())
        output.append('#define '+match[1]+'('+args+') ('+match[3]+')')
    for match in re.finditer(r'static inline uint32_t (A5XX_\w+)\(([^)]*)\)\s*\{([^}]+)\}',text):
        expression = re.search(r'return ([^;]+);',match[3])
        # Floating-point encoders need user-mode math. Kernel uses raw IEEE bits
        # for viewport state and never expands those encoders.
        if expression and 'float' not in match[2] and not re.search(r'\b(fui|util_|fui16)\b',expression[1]):
            arg = match[2].strip().split()[-1]
            output.append('#define '+match[1]+'('+arg+') ('+expression[1]+')')
    generated.mkdir(parents=True,exist_ok=True)
    (generated / 'A5xxRegs.h').write_text('\n'.join(output)+'\n')


def build_gpu_metal():
    source = ROOT / 'drivers/gpu/QcomMTL'
    generated = build_gpu_shaders()
    bundle = BUILD / 'output/QcomMTLDriver.bundle'
    bundle.mkdir(parents=True,exist_ok=True)
    libraries = prepare_gpu_jit_compiler(BUILD / 'gpu-compiler/mesa')
    runtime = bundle / 'QcomGPUCompiler.dylib'; shutil.copyfile(libraries[0],runtime)
    run([sys.executable,TOOLS/'gpu-compiler/system-shaders.py',
         '--output',generated/'GPUSystemShaders.h','--work',BUILD/'gpu-system-shaders'],
        log=BUILD/'gpu-system-shaders-build.log')
    # Copy only LLVM's C declarations/configuration. Adding /usr/include to a
    # target compile would accidentally pick up Linux libc headers.
    llvm_include = Path(subprocess.check_output([host('llvm-config'),'--includedir'],text=True).strip())
    headers = BUILD / 'gpu-compiler/include'
    shutil.copytree(llvm_include/'llvm-c',headers/'llvm-c',dirs_exist_ok=True)
    shutil.copytree(llvm_include/'llvm/Config',headers/'llvm/Config',dirs_exist_ok=True)
    # Keep persisted shader code across render/GMEM tuning and version bumps.
    # Compiler, constant interpretation, archive ABI and AOT registry changes
    # still invalidate it. The runtime adds the OS build to this identity.
    from gpu_archive_context import build_context
    archive_build = build_context(ROOT, runtime, generated,
        subprocess.check_output([BIN/'clang++-arm64','--version'],text=True).strip())
    context = bytes.fromhex(archive_build['context'])
    (generated/'GPUArchiveBuild.h').write_text('// Generated by the unified graphics build.\n#pragma once\n#include <stdint.h>\n'
        'namespace GPUArchiveBuild { constexpr uint8_t Context[32] = {'+
        ','.join('0x%02x'%byte for byte in context)+'}; }\n')
    (generated/'archive-build.json').write_text(json.dumps(archive_build,indent=2)+'\n')
    build_iokit_client('QcomMTLDriver', ['../drivers/gpu/QcomMTL/QcomMTL.mm', 'gpuctl/GPUConnection.cpp',
                       'gpu-compiler/MSL.cpp','gpu-compiler/AIRRuntime.cpp','gpu-compiler/Compiler.cpp'],
                       extra_exports=['IOConnectCallMethod', 'IOObjectConformsTo', 'IOObjectRetain',
                                      'IORegistryEntryGetRegistryEntryID', 'IORegistryEntryCreateCFProperty',
                                      'IODataQueueDataAvailable', 'IODataQueueDequeue',
                                      'IOConnectSetNotificationPort', 'IODataQueueAllocateNotificationPort'],
                       headers=['gpuctl/GPUConnection.h', 'include/IOKitUser.h',
                                'gpu-compiler/Compiler.h','gpu-compiler/AIRNIR.h','gpu-compiler/AIRNIRLowering.h','gpu-compiler/AIRNIR.cpp','gpu-compiler/Semantic.h','gpu-compiler/AIRControlFlow.h','gpu-compiler/LibraryArchive.h','gpu-compiler/FunctionConstants.h','gpu-compiler/AIRSpecialization.h','gpu-compiler/Interface.h','gpu-compiler/Source.json','gpu-compiler/NOTICE.txt',
                                '../drivers/gpu/QcomMTL/QcomMTL.h', '../drivers/gpu/QcomMTL/NativeMetal.h', '../drivers/gpu/QcomMTL/MetalSync.h', '../drivers/gpu/QcomMTL/MetalTexture.h', '../drivers/gpu/QcomMTL/MetalColor.h', '../drivers/gpu/QcomMTL/MetalHeap.h', '../drivers/gpu/QcomMTL/MetalReference.h', '../drivers/gpu/QcomMTL/MetalArchive.h', '../drivers/gpu/QcomMTL/CoreFSCache.h', '../drivers/gpu/QcomMTL/MetalBatch.h', '../drivers/gpu/QcomMTL/MetalUploadPool.h', '../drivers/gpu/QcomMTL/MetalRetirement.h',
                                'gpu-compiler/SystemShaderCatalog.h', '../build/drivers/generated/GPU/GPUSystemShaders.h',
                                '../build/drivers/generated/GPU/GPUArchiveBuild.h', '../build/drivers/generated/GPU/archive-build.json',
                                *('sdk/iPhoneOS18.6.sdk/System/Library/Frameworks/Metal.framework/Headers/'+name for name in
                                  ['MTLBinaryArchive.h','MTLCommandQueue.h','MTLCommandBuffer.h','MTLDevice.h','MTLBuffer.h','MTLTexture.h','MTLHeap.h','MTLResource.h','MTLAllocation.h','MTLFunctionDescriptor.h','MTLRenderPipeline.h','MTLComputePipeline.h','MTLLinkedFunctions.h','MTLLibrary.h','MTLArgument.h','MTLFence.h','MTLBlitCommandEncoder.h','MTLComputeCommandEncoder.h','MTLRenderCommandEncoder.h']),
                                '../drivers/gpu/QcomMTL/Info.plist', '../drivers/abi-22H374/metal-plugin-abi.json', '../drivers/abi-22H374/ui-presentation-abi.json', '../drivers/abi-22H374/coreanimation-library-abi.json', '../drivers/abi-22H374/function-constants-abi.json', '../docs/gpu/native-fragment-profile.json', '../drivers/abi-22H374/framebuffer-fetch-abi.json', '../drivers/abi-22H374/mask-formats-abi.json', '../drivers/abi-22H374/draw-batches-abi.json', '../drivers/abi-22H374/native-pipelines-abi.json', '../docs/gpu/ui-service-access.json',
                                '../build/drivers/generated/GPU/GPUPrograms.h'],
                       protocol=['GPUUserProtocol.h', 'GPUNativeProtocol.h', 'GPURenderPass.h', 'GPUMetalCapabilities.h', 'GPUNativeCompletion.h', 'GPUNativeMemory.h', 'GPUSurfaceProtocol.h', 'GPUBlit.h', 'GPUMemoryRanges.h', 'GPUShaderInterface.h', 'GPUShaderProtocol.h', 'GPUJITProtocol.h', 'GPUDiagnostics.h'],
                       bundle=bundle, frameworks=['Foundation', 'Metal', 'IOSurface'],
                       include_dirs=[TOOLS / 'gpuctl', TOOLS/'gpu-compiler', headers, generated],
                       libraries=[runtime, TOOLS/'sdk/iPhoneOS18.6.sdk/usr/lib/libLLVM.tbd'],
                       exception_sources=['MSL.cpp','AIRRuntime.cpp','Compiler.cpp'], adhoc_sign=True)
    shutil.copyfile(source / 'Info.plist', bundle / 'Info.plist')
    shutil.copyfile(generated / 'default.qmetallib', bundle / 'default.qmetallib')
    for name in json.loads((generated / 'shaders.json').read_text()).get('air_libraries',[]):
        shutil.copyfile(generated / name,bundle / name)
    shutil.copyfile(BUILD / 'gpu-compiler/NOTICE.Mesa.txt', bundle / 'NOTICE.Mesa.txt')
    build_iokit_client('metalctl', ['gpuctl/metalctl.mm'],
                       headers=['gpuctl/AIRNIROutputDiagnostics.h','gpuctl/RenderClearDiagnostics.h','gpuctl/GMEMDiagnostics.h', '../drivers/gpu/QcomMTL/CoreFSCache.h',
                                '../drivers/gpu/QcomMTL/QcomMTL.h', '../drivers/gpu/QcomMTL/NativeMetal.h', 'include/IOKitUser.h'],
                       protocol=['GPUDiagnostics.h', 'GPUUserProtocol.h','GPUNativeProtocol.h','GPURenderPass.h','GPUShaderProtocol.h','GPUShaderInterface.h','GPUJITProtocol.h'],
                       extra_exports=['IORegistryEntryGetRegistryEntryID', 'IORegistryEntryCreateCFProperty', 'IOConnectCallMethod'],
                       frameworks=['Foundation', 'Metal', 'IOSurface', 'QuartzCore', 'CoreGraphics'], include_dirs=[source],
                       adhoc_sign=True)
    # A filesystem overlay is a reviewable build product, never an installer.
    # Metal.framework derives this exact bundle path from MetalPluginName.
    staging = BUILD / 'output/gpu-rootfs'
    installed = staging / 'System/Library/Extensions' / bundle.name
    shutil.copytree(bundle, installed, dirs_exist_ok=True)
    diagnostic = staging / 'usr/local/bin/metalctl'
    diagnostic.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(BUILD / 'output/metalctl', diagnostic)
    diagnostic.chmod(0o755)
    from bridge_trustcache import development_module, module_info
    trustcache, executables = development_module([installed, diagnostic])
    trustcache_path = BUILD / 'output/gpu.trustcache'
    trustcache_path.write_bytes(trustcache)
    files = {str(path.relative_to(staging)): digest(path) for path in
             [*sorted(p for p in installed.rglob('*') if p.is_file()), diagnostic]}
    manifest = {'status':'staged-offline', 'hardware_tested':False, 'system_load_verified':False,
                'metal_plugin_name':'QcomMTLDriver', 'principal_class':'QcomMTLDevice',
                'device_superclass':'_MTLDevice', 'entry_selector':'initWithAcceleratorPort:',
                'kernel_personality':str((DRIVERS_ROOT/'gpu/QcomGPU/Info.plist').relative_to(ROOT)),
                'kernel_personality_sha256':digest(DRIVERS_ROOT/'gpu/QcomGPU/Info.plist'), 'files':files,
                'development_trustcache':{'path':str(trustcache_path.relative_to(staging.parent)),
                                        **module_info(trustcache), 'executables':executables}}
    (staging/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


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
    archive,profile_path,output=Path(archive),Path(profile_path),Path(output)
    profile=json.loads(profile_path.read_text())
    if not isinstance(profile,dict) or profile.get('schema')!=1 or profile.get('controller')!='WCN3990' or digest(archive)!=profile.get('archive_sha256'):
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
    import tempfile
    contract=json.loads((ROOT/'drivers/abi-22H374/driverkit-abi.json').read_text())
    reference=BUILD/'wifi/reference'
    reference.mkdir(parents=True,exist_ok=True)
    headers=TOOLS/'driverkit/include'
    for name,record in json.loads((headers/'sources.json').read_text()).items():
        if digest(headers/name)!=record['sha256']:
            raise ValueError('modified public DriverKit header: '+name)
    cache=ROOT/contract['cache']
    caches={cache:contract['cache_sha256'],Path(str(cache)+'.symbols'):contract['cache_symbols_sha256']}
    missing=[]
    for path,sha in caches.items():
        if path.exists():
            if digest(path)!=sha:raise ValueError('DriverKit reference cache changed: '+str(path))
        else:missing.append(path)
    if missing:
        source=TOOLS/'src/go-apfs'
        reader=contract['sealed_reader']
        patch=ROOT/reader['patch']
        if digest(patch)!=reader['patch_sha256']:raise ValueError('sealed reader patch changed')
        for name,sha in reader['source_files'].items():
            if digest(source/name)!=sha:raise ValueError('sealed reader source changed: '+name)
        image=ROOT/contract['source_cryptex']
        if not image.is_file():raise ValueError('prepare-wifi requires the local system cryptex: '+str(image))
        # A disposable source checkout keeps APFS writer behavior unchanged.
        # Output hashes below authenticate extraction against the pinned cache;
        # this narrow reader does not claim to authenticate the APFS seal.
        with tempfile.TemporaryDirectory(prefix='.reader-',dir=BUILD/'wifi') as temp:
            directory=Path(temp)
            shutil.copytree(source,directory/'source',ignore=shutil.ignore_patterns('.git','vendor'))
            run([host('patch'),'-p1','--batch','-i',patch],cwd=directory/'source',log=reference/'sealed-reader.log')
            executable=directory/'apfs'
            run([PREFIX/'libexec/go/bin/go','build','-o',executable,'./cmd/apfs'],cwd=directory/'source',log=reference/'sealed-reader.log')
            cache.parent.mkdir(parents=True,exist_ok=True)
            for path in missing:
                run([executable,'extract',image,'/System/DriverKit/System/Library/dyld/'+path.name,
                     '-C',cache.parent],log=reference/'sealed-reader.log')
                if digest(path)!=caches[path]:
                    path.unlink()
                    raise ValueError('extracted DriverKit cache hash mismatch')
    ipsw=BIN/'ipsw'
    if digest(ipsw)!=contract['extractor_sha256']:raise ValueError('DriverKit extraction tool changed')
    for name,record in contract['frameworks'].items():
        path=ROOT/record['path']
        if path.exists() and digest(path)!=record['sha256']:
            raise ValueError('DriverKit framework reference changed: '+name)
        if not path.exists():
            run([ipsw,'dyld','extract',cache,record['install_name'],'--slide','--output',path.parent],
                log=reference/'framework-extract.log')
            if digest(path)!=record['sha256']:raise ValueError('DriverKit framework extraction differs: '+name)
    for symbol,binding in contract['function_bindings'].items():
        path=ROOT/contract['frameworks'][binding['framework']]['path']
        expected=bytes.fromhex(binding['bytes'])
        if provider_function_bytes(path,int(binding['address'],0),len(expected))!=expected:
            raise ValueError('native WLAN function binding changed: '+symbol)
    binding=contract['public_vtable_binding']
    path=ROOT/contract['frameworks'][binding['framework']]['path']
    expected=bytes.fromhex(binding['bytes'])
    if provider_function_bytes(path,int(binding['address'],0),len(expected),executable=False)!=expected:
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
    from macho_development_sign import signature_blobs
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
    source=ROOT/'drivers/wifi/QcomWLANDriver'
    headers=TOOLS/'driverkit/include'
    contract=json.loads((ROOT/'drivers/abi-22H374/driverkit-abi.json').read_text())
    linker=PREFIX/'libexec/native/arm-apple-darwin11-ld'
    paths=[SELF,ROOT/'scripts/macho_development_sign.py',ROOT/'drivers/abi-22H374/driverkit-abi.json',headers/'sources.json',PREFIX/'compiler.json',
           Path(compiler()),linker if linker.exists() else BIN/'arm-apple-darwin11-ld',
           *source.glob('*'),*(headers/name for name in json.loads((headers/'sources.json').read_text())),
           DRIVERS_ROOT/'include/WLANProtocol.h',DRIVERS_ROOT/'include/WLANDriverPolicy.h',
           *(ROOT/contract['frameworks'][name]['path'] for name in ('DriverKit','NetworkingDriverKit','libc++.dylib'))]
    # Include the actual Darwin C/framework headers found by Clang, including
    # system headers. -MD is intentional; -MMD would omit these inputs.
    for name in ('QcomWLANDriver.cpp','QcomWLANDriverRPC.cpp','NativeWLAN.cpp'):
        dependency=BUILD/'wifi/obj'/(name+'.d')
        if not dependency.exists():raise ValueError('missing DriverKit dependency record; rebuild wifi-driverkit')
        value=dependency.read_text().replace('\\\n',' ')
        if ':' not in value:raise ValueError('malformed DriverKit dependency record')
        for item in shlex.split(value.split(':',1)[1]):
            path=Path(item).resolve()
            if not path.is_relative_to(ROOT):raise ValueError('DriverKit used a header outside the project: '+str(path))
            paths.append(path)
    return {str(path.relative_to(ROOT)):digest(path) for path in paths if path.is_file()}


def build_wifi_driverkit():
    import tempfile
    aliases();prepare_wifi_driverkit()
    source=ROOT/'drivers/wifi/QcomWLANDriver'
    contract=json.loads((ROOT/'drivers/abi-22H374/driverkit-abi.json').read_text())
    sdk=TOOLS/'sdk/iPhoneOS18.6.sdk'
    headers=TOOLS/'driverkit/include'
    objdir=BUILD/'wifi/obj';objdir.mkdir(parents=True,exist_ok=True)
    includes=['-isystem',PREFIX/'lib/clang'/json.loads((PREFIX/'compiler.json').read_text())['resource_version']/'include',
              '-isystem',sdk/'usr/include','-isystem',headers,'-F',sdk/'System/Library/Frameworks',
              '-I',DRIVERS_ROOT/'include']
    objects=[]
    for name in ('QcomWLANDriver.cpp','QcomWLANDriverRPC.cpp','NativeWLAN.cpp'):
        obj=objdir/(name+'.o');log=objdir/(name+'.log');log.unlink(missing_ok=True)
        run([BIN/'clang-real','--driver-mode=g++','--target=arm64-apple-driverkit24.6',*compiler_resource(),
             '-nostdinc','-nostdinc++',*includes,'-std=c++17','-fno-exceptions','-fno-rtti','-fblocks',
             '-Wall','-Wextra','-Werror','-O2','-fvisibility=hidden','-Xclang','-fdump-vtable-layouts',
             '-MD','-MF',objdir/(name+'.d'),'-c',source/name,'-o',obj],log=log)
        objects.append(obj)
    libraries=[ROOT/contract['frameworks'][name]['path'] for name in ('DriverKit','NetworkingDriverKit','libc++.dylib')]
    output=BUILD/'output/QcomWLANDriver.dext';output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.driverkit-',dir=BUILD/'wifi') as directory:
        pending=Path(directory)/'QcomWLANDriver'
        run([BIN/'arm-apple-darwin11-ld','-arch','arm64','-platform_version','driverkit','24.6','24.6',
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
                'sha256':digest(pending),'metadata':metadata,
                'inputs':wifi_driverkit_inputs()}
        output.mkdir(exist_ok=True)
        shutil.copy2(source/'Info.plist',output/'Info.plist')
        pending.replace(output/'QcomWLANDriver')
        (output/'build.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Cross-linked DriverKit 24.6: '+str(output.relative_to(ROOT)))


def verify_wifi_driverkit():
    output=BUILD/'output/QcomWLANDriver.dext'
    report=json.loads((output/'build.json').read_text())
    if report.get('status')!='cross-linked-offline' or report.get('hardware_tested') is not False or report.get('installed') is not False:
        raise ValueError('invalid offline DriverKit build report')
    if report.get('inputs')!=wifi_driverkit_inputs():raise ValueError('stale/incomplete DriverKit inputs; rebuild wifi-driverkit')
    if report.get('apple_platform_identity_claimed') is not False or report.get('apple_private_entitlements') is not False or \
            report.get('native_registration_bridge_compiled') is not True or report.get('native_wlan_control_ready') is not False:
        raise ValueError('invalid DriverKit readiness/signing claims')
    if digest(output/'QcomWLANDriver')!=report['sha256'] or driverkit_macho(output/'QcomWLANDriver')!=report['metadata']:
        raise ValueError('DriverKit executable changed')
    if (output/'Info.plist').read_bytes()!=(ROOT/'drivers/wifi/QcomWLANDriver/Info.plist').read_bytes():
        raise ValueError('stale DriverKit matching metadata')
    return {'status':'passed','bundle':str(output.relative_to(ROOT)),'sha256':report['sha256'],
            'platform':'DriverKit 24.6','native_registration_bridge_compiled':True,
            'native_wlan_control_ready':False,'hardware_tested':False,'installed':False}


def build_iokit_client(name, source_paths, extra_exports=(), headers=(), protocol=None,
                       bundle=None, frameworks=(), include_dirs=(), libraries=(), exception_sources=(),
                       adhoc_sign=False, loadable=False, blocks=False, entitlements=None, signing_identifier=None):
    """Cross-link internal iOS IOKit tools through one build path; no execution."""
    aliases()
    user_sdk = TOOLS / 'sdk/iPhoneOS18.6.sdk'
    sources = [(TOOLS / path).resolve() for path in source_paths]
    output = bundle / name if bundle else BUILD / 'output' / (name + '.dylib' if loadable else name)
    output.parent.mkdir(parents=True, exist_ok=True)
    objdir = BUILD / 'drivers/obj' / name
    objdir.mkdir(parents=True, exist_ok=True)
    tbd = user_sdk / 'System/Library/Frameworks/IOKit.framework/Versions/A/IOKit.tbd'
    exports = tbd.read_text()
    symbols = ['IOServiceMatching', 'IOServiceGetMatchingServices', 'IOIteratorNext', 'IOObjectRelease',
               'IOServiceOpen', 'IOServiceClose', 'IOConnectCallScalarMethod', *extra_exports]
    for symbol in symbols:
        if not re.search(r'\b_' + symbol + r'\b', exports):
            raise RuntimeError('user SDK lacks IOKit export ' + symbol)
    objects = []
    for source in sources:
        obj = objdir / (source.name + '.o')
        objc = ['-fobjc-arc', '-fblocks', '-fobjc-exceptions', '-fvisibility=hidden'] if source.suffix == '.mm' else []
        exceptions = '-fexceptions' if source.name in exception_sources else '-fno-exceptions'
        cpu = ['-mcpu=generic','-march=armv8-a+crc+crypto'] if name in ('QcomMTLDriver','metalctl') else []
        run([BIN / 'clang++-arm64', '-isysroot', user_sdk, '-std=c++17', exceptions, '-fno-rtti',
             '-Wall', '-Wextra', '-Werror', '-Oz' if name == 'displayctl' else '-O2', *cpu, '-I' + str(DRIVERS_ROOT / 'include'),
             *(['-fblocks'] if blocks else []),
             '-I' + str(TOOLS / 'include'), *('-I' + str(path) for path in include_dirs), *objc,
             '-c', source, '-o', obj])
        objects.append(obj)
    llvm_symbols = set()
    if name == 'QcomMTLDriver':
        exports = (user_sdk/'usr/lib/libLLVM.tbd').read_text()
        for obj in objects:
            undefined = subprocess.check_output([host('llvm-nm'),'--undefined-only',obj],text=True)
            for symbol in re.findall(r'\b(_LLVM\w+)\b',undefined):
                if not re.search(r'\b'+re.escape(symbol)+r'\b',exports):
                    raise RuntimeError('iOS LLVM lacks runtime AIR C export '+symbol)
                llvm_symbols.add(symbol)
    run([host('ld64.lld'), '-arch', 'arm64', '-syslibroot', user_sdk,
         '-platform_version', 'ios', '18.6', '18.6', '-o', output, *objects,
         *(['-adhoc_codesign'] if adhoc_sign or entitlements else []),
         *(['-x'] if name == 'displayctl' and not loadable else []),
         *(['-dylib', '-install_name', '@rpath/' + name + '.dylib', '-dead_strip', '-x', '-lobjc',
            '-no_function_starts', '-no_data_in_code_info',
            '-exported_symbol', '_' + name + '_struct'] if loadable else []),
         *(['-dylib', '-install_name', '@rpath/QcomMTLDriver.bundle/QcomMTLDriver', '-lobjc', '-lc++',
            '-exported_symbol', '_QcomMTLCreateDevice', '-exported_symbol', '_QcomMTLCreateSystemDefaultDevice', '-exported_symbol', '_QcomMTLEncodeComposition',
            '-exported_symbol', '_OBJC_CLASS_$_QcomMTLDevice', '-exported_symbol', '_OBJC_METACLASS_$_QcomMTLDevice'] if bundle else []),
         '-lSystem', '-framework', 'IOKit', '-framework', 'CoreFoundation',
         *(arg for framework in frameworks for arg in ['-framework', framework]),
         *libraries, *(['-rpath', '@executable_path'] if libraries else []),
         *(['-lobjc', '-lc++'] if libraries or any(source.suffix == '.mm' for source in sources) else [])])
    signature=None
    if entitlements:
        from macho_development_sign import sign_new_macho
        data,signature=sign_new_macho(output.read_bytes(),signing_identifier,entitlements,
                                     compact=name == 'displayctl')
        output.write_bytes(data)
    data = output.read_bytes()
    if struct.unpack_from('<4I', data) != (0xfeedfacf,0x100000c,0,6 if bundle or loadable else 2):
        raise RuntimeError('user binary has the wrong arm64 Mach-O type')
    report = {'status':'cross-linked-offline', 'hardware_tested':False, 'path':str(output.relative_to(ROOT)),
              'sha256':digest(output),'source_sha256':digest(sources[0]),'iokit_exports_verified':symbols,
              'inputs':{str(path.relative_to(ROOT)):digest(path) for path in
                        [*sources, *((TOOLS / path).resolve() for path in headers)]}}
    if signature:report['signature']=signature
    if entitlements:
        for path in [SELF,ROOT/'scripts/macho_development_sign.py',
                     TOOLS/'sdk/iPhoneOS18.6.sdk/usr/lib/system/libxpc.tbd']:
            report['inputs'][str(path.relative_to(ROOT))]=digest(path)
    if bundle:
        protocol_versions={name:int(re.search(r'\bVersion\s*=\s*(\d+)',
            (DRIVERS_ROOT/('include/GPU'+name+'Protocol.h')).read_text()).group(1)) for name in ('Shader','JIT','Surface')}
        report.update({'stage':'native-system-plugin-shader-render-compute-linear-textures',
                       'universal_air64_metallib_import':True,'metallib_parser_shared_with_aot':True,
                       'metallib_function_compilation':'on-demand, exact selected AIR entry',
                       'bundled_system_shaders':{key:value for key,value in json.loads((BUILD/'gpu-system-shaders/report.json').read_text()).items()
                           if key in ('registry','entry_count','variant_count','packed_bytes','header_sha256')},
                       'bundled_system_shader_match':'full AIR SHA-256, entry name, resolved constant indices/types/bytes; current registry',
                       'bundled_system_shader_miss':'normal exact compiler/cache path; no substitute shader',
                       'native_submission_dispatch_target':True,'native_completion_dispatch_target':True,
                       'native_dispatch_target_configuration':'before first submission; serial wrappers retained',
                       'native_commit_and_wait_until_submitted':True,'native_commit_and_wait_return_type':'BOOL',
                       'native_inline_fifo_drain':True,'native_inline_fifo_scope':'guarded direct drawable and private full-submission helper; actual pixel admission',
                       'native_direct_drawable_commit_admission':True,
                       'native_direct_drawable_guards':'no uncommitted prefix, future event wait, external pending fence or explicit presentation barrier',
                       'native_direct_drawable_scheduled':'whole real operation list admitted before raw present follows commit',
                       'native_commit_submission_marker':False,
                       'native_commit_uncommitted_prefix':'return NO without waiting; caller can commit the predecessor on its serial submission target',
                       'native_commit_submission_scope':'drain FIFO through the requested buffer; YES only after complete real operation-list admission',
                       'native_commit_private_direct_drawable_wait':False,
                       'native_bounded_async_execution':True,'native_device_pending_jobs_max':16,
                       'native_resource_namespace_limit':1024,'native_resources_per_job':64,
                       'native_memory_limits':'version1 IORegistry data,64 bytes; effective client/global byte quotas from actual IOVA aperture',
                       'native_memory_accounting':'tracked namespace charges; actual live native objects and retained DMA mappings reported separately',
                       'native_mapper_capacity':4096,'native_mapper_allocator':'stable records with address-ordered index; one pass for first fit',
                       'native_single_resource_limit_bytes':16777216,
                       'native_async_completion_consumer':'actual Family IODataQueue, matching FIFO end records, separate retirement executor',
                       'native_admission_backpressure':'pre-Family vendor status, unchanged tokens/fences; bounded asynchronous retry',
                       'native_cpu_index_boundary':'all device jobs retired before private CPU index snapshot; kernel checks other clients',
                       'native_blit_parameter_reuse':'all5 immutable builtin layouts frozen in job IB before admission returns',
                       'native_partial_blit_resume':'accepted row/byte cursor; no duplicate GPU batch after capacity wake',
                       'native_callback_waits':'public waits include registered handler delivery; terminal buffers retire in FIFO; private helper returns NO until full operation-list admission',
                       'native_marker_scheduled_state':'every per-operation admission stays private; public Scheduled and helper YES require complete operation-list admission',
                       'native_display_submission_boundary':'retain drawable/FIFO barrier through real retirement; native IOSurface fence capability permits once-only presentation at whole-list Scheduled',
                       'native_fences':True,'native_fence_passes':['blit','compute','render vertex/fragment'],
                       'native_fence_capture':'snapshot all captured producing points in device commit order; prune retired points on the next update; producers committed before consuming waits',
                       'native_fence_multiple_producers':True,'native_fence_wait_future_updates':False,
                       'native_fence_completion':'successful real IOGPU end token plus native queue identity; producer errors propagate',
                       'native_fence_waits':'suspend FIFO head and resume by point callback; no busy wait or occupied submission target',
                       'native_event_waits':'bounded background wait, suspended FIFO head; original event strong or zeroing weak according to retainedReferences',
                       'native_memory_barriers':'validated resource/scope/stages; actual native CP Barrier submission',
                       'native_command_buffer_retained_references':True,
                       'native_command_buffer_reference_interfaces':['commandBuffer','commandBufferWithUnretainedReferences','commandBufferWithDescriptor:'],
                       'native_external_resource_ownership':'strong through real retirement or zeroing weak for caller-owned resources; actual retainedReferences query',
                       'native_borrowed_identity_validation':'commit and per-operation admission/retry; missing objects fail InvalidResource without resolving recycled IDs',
                       'native_internal_resource_ownership':'inline bytes, resolve parameters and helper-owned composition objects retained until real retirement',
                       'native_reference_snapshot_release':'partial and complete CPU snapshots released after condition/order/transport locks; encoder bindings cleared at endEncoding',
                       'native_reference_runtime_verified':False,
                       'native_command_buffer_quota_policy':'nonblocking bring-up limit; nil when no capacity',
                       'coreanimation_function_constants':True,
                       'function_constant_scope':'AIR inside MTLB/FAT, scalar/vector constants and bounded static initialization',
                       'native_function_constant_values_reused':True,'native_function_constant_reflection':True,
                       'function_constant_async_creation':True,'function_constant_cache':'effective typed values; index precedence; unrelated values ignored',
                       'native_function_descriptor':True,'native_function_descriptor_async':True,
                       'native_specialized_function_names':True,'native_pipeline_descriptor_options':True,
                       'native_render_pipeline_async':True,'native_compute_pipeline_async':True,
                       'native_compute_pipeline_descriptor':True,'native_pipeline_reflection':False,
                       'native_pipeline_binary_archives':True,'native_archive_only_miss_returns_error':True,
                       'native_binary_archive_profile':'A540 Compute/Render pipeline functions; lazy AIR function lookup before lowering/IR3; original Metal descriptors and archive-only options',
                       'native_binary_archive_format':'version1 bounded pointer-free QMTLARC1 with SHA-256, exact registry/Shader/JIT, build-source/compiler fingerprint and native OS context',
                       'native_binary_archive_aot':'validated catalog references only; never re-register builtin AOT code as mutable-parameter JIT',
                       'native_binary_archive_limits':{'functions':256,'pipelines':1024,'bytes':16777216,'lookup_archives':16},
                       'native_binary_archive_runtime_verified':False,
                       'native_pipeline_inputs':'copied original Apple descriptors and constant values; existing A540 AIR/JIT registration and IOGPU submission',
                       'function_constant_indices':65536,'function_constants_per_module':256,
                       'msl_function_constant_source_syntax':False,'half_shader_arithmetic':True,
                       'half_shader_scope':'AIR scalar/vector binary16 arithmetic, conversions, typed SSBO loads/stores, half texture sampling/read/write and float32 stage transport',
                       'half_shared_memory':False,'msl_half_source_syntax':False,
                       'unsigned_integer_textures':['R32Uint'],
                       'unsigned_integer_texture_scope':'compute read/write/query; 2D, arrays, cube and cube arrays; mip levels, exact uint swizzles, BGRA/RGBA raw word views and IOSurface imports',
                       'unsigned_integer_texture_filtering':False,'unsigned_integer_render_targets':False,'unsigned_integer_msaa':False,
                       'air_private_array_bytes_max':1024,'air_unorm4x8_pack':True,
                       'native_drawable_present_bridge':True,'drawable_present_after_gpu_completion':'legacy or non-IOSurface; native surface-fence capability queues at Scheduled',
                       'native_surface_producer_fence':'IOGPUNativeSurfaceFenceVersion1; real kernel IOSurface access fences, success only after A540 completion and coherence',
                       'drawable_presentation_modes':['immediate','atTime','afterMinimumDuration','native options'],
                       'native_drawable_texture_descriptor':True,'framebuffer_only_texture_restrictions':True,
                       'native_shared_event_command_bridge':True,'shared_event_wait_timeout_ms':5000,
                       'shared_event_wait_releases_device_transport_lock':True,
                       'gpu_layer_composition':True,'composition_layers_max':8,
                       'composition_features':['premultiplied alpha','opacity','affine geometry','contents crop','scissor','GPU background draw'],
                       'system_coreanimation_compositor_selected':False,'system_ui_gpu_acceleration_verified':False,
                       'system_metal_plugin_registered':False, 'system_metal_plugin_declared':True,
                       'system_metal_plugin_entry_implemented':True, 'system_metal_load_verified':False,
                       'system_metal_plugin_path':'/System/Library/Extensions/QcomMTLDriver.bundle',
                       'device_superclass':'_MTLDevice', 'principal_class':'QcomMTLDevice',
                       'system_entry_selector':'initWithAcceleratorPort:',
                       'shader_control_flow':True, 'msl_lexical_scopes':True,
                       'bounded_buffer_access':True,'buffer_load_oob':'zero','buffer_store_oob':'discard',
                       'constant_scalar_vector_parameters':True,'constant_struct_fields':True,
                       'inline_compute_vertex_bytes':4096,'shader_isa_sandbox':False,
                       'compute_variable_workgroups':True,'compute_grid_dimensions':3,
                       'compute_max_threads_per_group':1024,'compute_max_local_dimensions':[1024,1024,64],
                       'compute_max_invocations':16777216,'compute_buffer_index_proof':'NIR byte-range guards from SSBO descriptors',
                       'compute_nonuniform_workgroups':True,'compute_nonuniform_backend':'up to eight exact-size CP regions in one native submission, original grid/group coordinates and requested local size in native constants, no padded barrier lanes','compile_option_thread_limit_preserved':True,
                       'air_cfg_max_blocks':128, 'air_phi_parallel_copies':True,
                       'air_cyclic_cfg':'per-invocation dispatch without an artificial iteration limit',
                       'runtime_llvm_compatibility':{'branch_opcodes':[2,70,71],
                           'switch_case_access':'legacy operand pairs or optional exported LLVMGetSwitchCaseValue'},
                       'native_limits_bytes':264, 'native_feature_profile':'no complete Apple GPU family',
                       'shader_compilation':True, 'shader_compilation_mode':'aot-and-runtime-jit-msl-air-profile', 'render_compute':True, 'multiple_color_attachments':True, 'max_color_attachments':8,
                       'multisample_rendering':True, 'texture_sample_counts':[1,2,4], 'gpu_color_resolve':True, 'msl_multisample_read':True, 'alpha_to_coverage':True,
                       'multisample_arrays':True, 'depth_stencil_arrays':True, 'multisample_array_stage_reads':['compute','vertex','fragment'],
                       'depth_texture_read_sample_compare':True, 'depth_texture_mip_chains':True, 'depth_stencil_mip_views':True, 'depth_texture_mip_levels_max':15,
                       'depth_texture_formats':['Depth32Float','Depth32Float_Stencil8 depth plane'],
                       'depth_texture_shapes':['2d','2d-array','2d-ms','2d-ms-array profile extension'],
                       'gpu_depth_stencil_resolve':True, 'depth_resolve_filters':['sample0','min','max'], 'stencil_resolve_filters':['sample0','depth-resolved-sample'],
                       'msl_as_type_32bit_same_lanes':True,
                       'sample_frequency_shading':True, 'centroid_sample_interpolation':True, 'fragment_sample_mask':True,
                       'fragment_single_msaa_variants':True, 'program_description_bytes':2468, 'jit_program_image_bytes':83720, 'fragment_constant_window_variants':True,
                       'fragment_depth_output':True, 'fragment_colorless_output':True, 'depth_only_nil_fragment':True,
                       'fragment_discard':True, 'fragment_software_demote':True, 'fragment_derivatives':True, 'air_float_comparisons':True,
                       'air_compilation':True, 'metallib_import':True, 'runtime_air_compilation':True,
                       'runtime_air_direct_nir':True, 'runtime_air_direct_nir_fallback':'existing AIR-to-GLSL compiler',
                       'runtime_air_direct_nir_scope':'selected/single-entry Compute and Fragment DAG profile; unsupported input uses GLSL',
                       'runtime_air_direct_nir_hardware_verified':False,
                       'constant_samplers':True, 'constant_sampler_states_max':16, 'hardware_sampler_slots':16,
                       'runtime_msl_compilation':True, 'jit_version':protocol_versions['JIT'], 'trusted_admin_compiler':True, 'jit_programs_max':256,
                       'shader_command_version':protocol_versions['Shader'], 'texture_arrays':True, 'texture_array_layers_max':2048,
                       'graphics_texture_fetch':True, 'graphics_texture_queries':True, 'graphics_fetch_alias_base':8, 'graphics_texture_descriptors':16,
                       'air_graphics_texture_binding_indices':16, 'graphics_enabled_textures_max':8, 'graphics_texture_binding_remap':True,
                       'air_texture_fetch_queries':True, 'air_texture_current_read_sampler':True, 'air_texture_legacy_read':True,
                       'air_texture_array_cube_operations':True, 'air_texture_gradient_sampling':True,
                       'shader_code_bytes_max':16384, 'shader_memory_fetch':True, 'graphics_shader_cache_partition':True,
                       'framebuffer_draw_ib_batches':True,'framebuffer_draw_ib_count_max':256,'framebuffer_draw_ib_bytes_max':16777216,'framebuffer_live_ib_quota_bytes':67108864,'framebuffer_draw_completion':'one native end token after all private read-only IBs','framebuffer_progress_watchdog_ms':500,
                       'cube_image_operations':True, 'cube_image_queries':True, 'cube_image_read_write':True,
                       'cube_textures':True, 'cube_arrays':True, 'cube_face_blit_render_mipmap':True, 'cube_msl_sampling':True, 'cube_air_intrinsics_verified':True,
                       'vertex_texture_sampling':True, 'vertex_texture_bindings':16, 'vertex_sampler_bindings':8, 'fragment_texture_bindings':16, 'fragment_sampler_bindings':8, 'graphics_independent_sampler_pairs':True, 'graphics_sparse_texture_slots':True,
                       'vertex_texture_msl_air_2d':True, 'vertex_texture_msl_arrays':True,
                       'vertex_fragment_texture_bindings_independent':True,
                       'texture_array_msl_read_write_sample':True, 'texture_array_air_intrinsics_verified':True,
                       'texture_array_slice_views':True, 'texture_array_render_slices':True,
                       'render_state_bridge':['blend','color-write-mask','viewport','cull','front-winding','depth-range','depth-stencil'],
                       'compute_texture_bindings':8,'compute_texture_access':['read','write','read_write','sample'],
                       'compute_texture_formats':['rgba8unorm','bgra8unorm','rgba8unorm_srgb','bgra8unorm_srgb','r8unorm','rg8unorm','rgba16float'],'compute_texture_mip_access':True, 'compute_texture_sampling':True, 'compute_sampler_bindings':8,
                       'compute_texture_backend':'guarded SSBO bank 8..15 for images; hardware CS texture state block 5 with independent texture/sampler slots; native-owned geometry constants for image/query access',
                       'texture_views':True,'texture_view_formats':['rgba8unorm','bgra8unorm','rgba8unorm_srgb','bgra8unorm_srgb','r8unorm','rg8unorm'],'texture_view_equal_element_size_required':True,'texture_view_mip_subranges':True,'texture_view_parent_retained':True,'texture_view_arbitrary_swizzle':True,'texture_swizzle_composition':True,'texture_swizzle_read_only':True,'texture_swizzle_mip_generation_ignored':True,'texture_view_storage':'shared original native backing; parent-relative metadata, rebased mip planes and bounded access span without allocating padding',
                       'buffer_backed_textures':True,'buffer_texture_type':'2d, single mip/layer/sample, existing6 color formats',
                       'buffer_texture_alignment_bytes':64,'buffer_texture_storage':'same native buffer resource ID and allocation; bounds checked against logical buffer length; buffer retains lifetime',
                       'native_linear_texture_entrypoint':'newLinearTextureWithDescriptor:offset:bytesPerRow:bytesPerImage:',
                       'native_linear_texture_image_stride':'2D, one mip/layer/sample; zero or complete bounded plane; original buffer backing retained',
                       'buffer_texture_operations':['CPU access for shared storage','blit','compute images','hardware sampling','render target','nested format/swizzle views'],
                       'buffer_texture_metadata':'direct buffer/offset/rowBytes; texture-created child reports parentTexture; rootResource resolves original buffer',
                       'buffer_texture_runtime_verified':False,
                       'native_texture_layout_spi':True,
                       'native_texture_layout_selector':'newTextureLayoutWithDescriptor:isHeapOrBufferBacked:',
                       'native_texture_layout_storage':'same checked A540 linear color mip/slice layout as allocated textures;16-byte adapter watermark trailer',
                       'native_texture_layout_copy':'checked bidirectional2D region copies, all supported single-sample color formats, arrays/cube faces; no GPU allocation',
                       'native_texture_layout_alignment_bytes':64,'native_texture_layout_watermark_bytes':16,
                       'native_texture_layout_runtime_verified':False,
                       'astc_4x4_textures':True,'astc_4x4_formats':['ASTC_4x4_LDR','ASTC_4x4_sRGB'],
                       'astc_4x4_storage':'16-byte blocks,1024-byte aligned base block rows,A540 pitch alignment4,last mip32 block rows',
                       'astc_4x4_access':'sample/read only; checked block-aligned copies with right/bottom edge exceptions; no runtime compression',
                       'native_blit_layout_version':2,'immutable_builtin_blit_layouts':5,
                       'render_load_clear':'dedicated immutable GPU row-fill; active width times format/sample bytes; stride and original backing offset retained',
                       'render_clear_padding':'preserve per-row and outer bytes; unique active-word threads on aligned rows, unique global-word merge for unaligned rows',
                       'render_clear_pattern':'32-bit or paired 64-bit pattern repeats from byte zero of each logical row',
                       'rgba16float_render_targets':True,'rgba16float_render_samples':[1,2,4],
                       'rgba16float_clear':'RGBA binary16 pairs; signed/HDR values retained; direct double conversion',
                       'rgba16float_image_storage':'two packed half2 words per texel; signed/HDR read/write and MSAA resolve',
                       'rgba16float_framebuffer_read':'per-primitive private snapshots; native 0x62 texture conversion; linear color',
                       'render_clear_cpu_pixel_edges':False,'render_clear_runtime_verified':False,
                       'native_resource_heaps':True,'heap_types':['automatic','placement'],'heap_storage_modes':['shared','private'],
                       'heap_backing':'one original IOGPU buffer per heap;64-byte suballocations and bounded resource-relative offsets rebased into backing',
                       'heap_resource_formats':'existing texture shapes, color/depth/stencil formats, mip/array/MSAA layouts and buffer views',
                       'heap_aliasing':'automatic makeAliasable permits reuse; unique lease tokens prevent stale frees; placement overlaps tracked independently',
                       'heap_lifetime':'resources and texture views retain allocation/heap/backing; commands retain resources through real native retirement',
                       'heap_size_limit':'native maxBufferLength, at most16MiB per backing; descriptor rounded to native page granularity',
                       'heap_purgeability':'existing KeepCurrent/NonVolatile only; no volatile/empty or sparse support',
                       'heap_runtime_verified':False,
                       'color_texture_mipmaps':True,'max_texture_mip_levels':15,'texture_mip_layout':'A5xx linear fdl5, 64-pixel base pitch alignment per format, aligned-base-pitch minification, padded last level, page-aligned layer',
                       'packed_color_formats':['r8unorm','rg8unorm'],'packed_color_render_blend':True,'packed_color_image_store':'per-word CAS preserves adjacent pixels and padding','packed_color_default_channels':'R8=(r,0,0,1),RG8=(r,g,0,1), including unspecified components of out-of-bounds reads',
                       'gpu_mipmap_generation':True,'srgb_color_formats':True,'srgb_compute_read_write':True,'srgb_render_targets':True,'srgb_cpu_clear_encoding':True,'srgb_alpha_linear':True,'mipmap_generation_filter':'per-level hardware bilinear sampling in linear color space, guarded image stores; sRGB RGB encode/decode and linear alpha',
                       'texture_mip_upload_readback_blit':True,'mip_render_attachments':True,'sampler_mip_filters':['none','nearest','linear'], 'sampler_address_modes':['clamp-to-edge','mirror-clamp-to-edge','repeat','mirror-repeat','clamp-to-zero','clamp-to-border-color'],
                       'sampler_max_anisotropy':16, 'sampler_pixel_coordinates':True, 'sampler_border_colors':['transparent-black','opaque-black','opaque-white'],
                       'sampler_explicit_level_ignores_lod_clamp':True, 'msl_gradient2d':True,
                       'sampler_lod_clamps':'unsigned 8.8 fixed-point; none clamps to 0.125 as fd5',
                       'shader_sampling_lod':['implicit','float32 explicit level','fragment float32 bias'],
                       'vertex_descriptor':True, 'vertex_attribute_locations':16, 'vertex_buffer_bindings':8, 'fragment_buffer_bindings':8,
                       'fragment_inline_bytes':4096, 'integer_atomics':True, 'atomic_memory_orders':['relaxed'], 'atomic_integer_bits':32,
                       'static_threadgroup_memory':True, 'max_threadgroup_memory_bytes':32768,
                       'threadgroup_barriers':True, 'dynamic_threadgroup_memory':True,
                       'dynamic_threadgroup_bindings':8,'threadgroup_alignment_bytes':16,
                       'threadgroup_constant_words':16,'threadgroup_allocation':'static prefix plus per-dispatch aligned lengths; combined <=32768 bytes',
                       'graphics_buffer_banks':{'vertex':[0,7],'fragment':[8,15]},
                       'vertex_fetch':'A540 VFD', 'indexed_draw':True, 'instanced_draw':True, 'private_index_snapshot':True,
                       'depth_stencil':True, 'depth_stencil_formats':['depth32float','stencil8','depth32float_stencil8'],
                       'depth_stencil_aspect_blits':True, 'varying_linkage':True, 'interpolation':['perspective','linear','flat'], 'shared_msl_frontend':True,
                       'runtime_llvm_exports_verified':sorted(llvm_symbols),
                       'full_metal_family_supported':False,
                       'native_command_version':int(re.search(r'\bCommandVersion\s*=\s*(\d+)',
                           (DRIVERS_ROOT/'include/GPUNativeProtocol.h').read_text())[1]),
                       'iosurface_import':True, 'iosurface_formats':['rgba8unorm','bgra8unorm','r8unorm','rg8unorm'],
                       'iosurface_pixel_formats':['RGBA','BGRA','L008','2C08','LA08'],
                       'iosurface_la08_mapping':'rg8unorm; preserve bytes and caller swizzle; QuartzCore shader interprets gray/alpha',
                       'iosurface_mask_fourcc':['L008','2C08','LA08'], 'iosurface_metadata_version':protocol_versions['Surface'],
                       'iosurface_planes':[0], 'iosurface_copy_required':False,
                       'client_buffer_import':True, 'client_buffer_storage_mode':'shared',
                       'client_buffer_cache_mode':'default', 'client_buffer_requires':'native page-aligned writable whole allocation',
                       'client_buffer_copy_required':False, 'client_buffer_runtime_verified':False,
                       'native_admission':'asynchronous except guarded direct-drawable real admission', 'native_completion_version':1,
                       'native_private_address_spaces':True, 'native_address_space_lifetime':'per-submission',
                       'scheduled_handler_after_native_admission':True,
                       'completion_source':'Family end token after A540 timestamp/cache/IOSurface retirement',
                       'frameworks':['IOKit', 'CoreFoundation', *frameworks],
                       'exports':['QcomMTLCreateDevice', 'QcomMTLCreateSystemDefaultDevice', 'QcomMTLEncodeComposition']})
    for library in libraries:
        report['inputs'][str(library.relative_to(ROOT))] = digest(library)
    if protocol:
        for name in ([protocol] if isinstance(protocol, str) else protocol):
            path = DRIVERS_ROOT / 'include' / name
            report['inputs'][str(path.relative_to(ROOT))] = digest(path)
    Path(str(output)+'.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Built arm64 iOS ' + ('Metal adapter: ' if bundle else 'IOKit client: ') + str(output))


def patch_no_sep_service_readiness(image):
    """Use native port death instead of retaining requests for a dead server.

    No FairPlay request, key, result or daemon code is replaced. KeepAlive
    still starts the original job; exit invalidates its old Mach endpoints.
    """
    expected = '75d3014dcecf67a9129240a469d08d202cecede2ba66704bbf09c8b4c5ab0fae'
    if hashlib.sha256(image).hexdigest() != expected:
        raise ValueError('native FairPlay launchd reference hash mismatch')
    job = plistlib.loads(image)
    endpoints = ('com.apple.fairplayd.versioned', 'com.apple.fairplayd')
    if (job.get('Label') != 'com.apple.fairplayd.A2' or job.get('Program') != '/usr/sbin/fairplayd.A2'
            or job.get('KeepAlive') is not True
            or any(job.get('MachServices', {}).get(name) is not True for name in endpoints)
            or job.get('CFBundleInfoDictionaryVersion') != '6.0'):
        raise ValueError('unsupported native FairPlay launchd configuration')
    # This descriptive bundle-format key is not a launchd job setting. Omit
    # it to retain the file's existing logical size and APFS allocation.
    del job['CFBundleInfoDictionaryVersion']
    readiness = {'ResetAtClose': True}
    for name in endpoints:
        job['MachServices'][name] = readiness
    encoded = plistlib.dumps(job, fmt=plistlib.FMT_BINARY, sort_keys=False)
    padding = len(image) - len(encoded)
    if padding < 0:
        raise ValueError('readiness plist exceeds original allocation')
    # Padding before the offset table leaves every object/reference intact;
    # the trailer remains the final32 bytes, as required by the native parser.
    table, = struct.unpack_from('>Q', encoded, len(encoded)-8)
    result = bytearray(encoded[:table] + bytes(padding) + encoded[table:])
    struct.pack_into('>Q', result, len(result)-8, table+padding)
    if len(result) != len(image) or plistlib.loads(result) != job:
        raise ValueError('readiness plist failed allocation-preserving round trip')
    return bytes(result)


def patch_launchd_cache_readiness(data):
    """Reuse a native binary-plist readiness object; change only two references.

    iOS18 boots from the cached LaunchDaemons dictionary. Updating an individual
    job plist alone does not change that cached configuration.
    """
    if len(data) < 40 or len(data) > 16*1024*1024 or data[:8] != b'bplist00':
        raise ValueError('unsupported launchd cache format')
    width, refs, count, top, table = struct.unpack('>6xBBQQQ', data[-32:])
    if width not in (1,2,4,8) or refs not in (1,2,4,8) or not 0 <= top < count or table+count*width != len(data)-32:
        raise ValueError('invalid launchd cache trailer')
    offsets = [int.from_bytes(data[table+i*width:table+(i+1)*width], 'big') for i in range(count)]
    if any(not 8 <= offset < table for offset in offsets):
        raise ValueError('invalid launchd cache object offset')
    def header(reference):
        if not 0 <= reference < count: raise ValueError('invalid launchd cache reference')
        pos = offsets[reference]; kind, length = data[pos] >> 4, data[pos] & 15; pos += 1
        if length == 15:
            if pos >= table or data[pos] >> 4 != 1 or data[pos] & 15 > 3:
                raise ValueError('invalid launchd cache object length')
            size = 1 << (data[pos] & 15); pos += 1
            if pos+size > table: raise ValueError('truncated launchd cache object length')
            length = int.from_bytes(data[pos:pos+size], 'big'); pos += size
        return kind, pos, length
    def string(reference):
        kind, pos, length = header(reference)
        size = length * (2 if kind == 6 else 1)
        if kind not in (5,6) or pos+size > table:
            raise ValueError('invalid launchd cache dictionary key')
        return data[pos:pos+size].decode('utf-16-be' if kind == 6 else 'ascii')
    def items(reference):
        kind, pos, length = header(reference)
        if kind != 13 or length > count or pos+2*length*refs > table:
            raise ValueError('invalid launchd cache dictionary')
        keys = [int.from_bytes(data[pos+i*refs:pos+(i+1)*refs], 'big') for i in range(length)]
        pos += length*refs
        result = [(string(key), int.from_bytes(data[pos+i*refs:pos+(i+1)*refs], 'big'), pos+i*refs)
                  for i,key in enumerate(keys)]
        if len({name for name,_,_ in result}) != length:
            raise ValueError('duplicate launchd cache key')
        return result
    selected = top
    for name in ('LaunchDaemons', '/System/Library/LaunchDaemons/com.apple.fairplayd.A2.plist', 'MachServices'):
        matches = [entry for entry in items(selected) if entry[0] == name]
        if len(matches) != 1: raise ValueError('missing native FairPlay cached job')
        selected = matches[0][1]
    choices = []
    for index,offset in enumerate(offsets):
        if data[offset] >> 4 != 13: continue
        entries = items(index)
        if len(entries) == 1 and entries[0][0] == 'ResetAtClose':
            value = entries[0][1]
            if value < count and data[offsets[value]] == 9: choices.append(index)
    if not choices: raise ValueError('no reusable native ResetAtClose dictionary')
    original = plistlib.loads(data)
    job = original['LaunchDaemons']['/System/Library/LaunchDaemons/com.apple.fairplayd.A2.plist']
    if job.get('Program') != '/usr/sbin/fairplayd.A2' or job.get('Label') != 'com.apple.fairplayd.A2' or job.get('KeepAlive') is not True:
        raise ValueError('unsupported native cached FairPlay job')
    result, patches = bytearray(data), []
    for name in ('com.apple.fairplayd.versioned', 'com.apple.fairplayd'):
        entry = next((entry for entry in items(selected) if entry[0] == name), None)
        if entry is None or job['MachServices'].get(name) is not True:
            raise ValueError('unexpected cached FairPlay endpoint policy')
        _,reference,pos = entry
        if reference >= count or data[offsets[reference]] != 9:
            raise ValueError('cached FairPlay endpoint is not a boolean')
        replacement = choices[0].to_bytes(refs, 'big')
        patches.append({'endpoint': name, 'offset': pos, 'before': data[pos:pos+refs].hex(), 'after': replacement.hex()})
        result[pos:pos+refs] = replacement
        job['MachServices'][name] = {'ResetAtClose': True}
    if plistlib.loads(result) != original or len(result) != len(data):
        raise ValueError('cached readiness policy changed unrelated configuration')
    return bytes(result), patches


def prepare_no_sep_service_readiness(source, output, resource_fork=None, compression_attribute=None,
                                     cache=None, cache_signature=None):
    original = source.read_bytes()
    patched = patch_no_sep_service_readiness(original)
    relative = Path('System/Library/LaunchDaemons/com.apple.fairplayd.A2.plist')
    destination = output/'overlay'/relative
    if destination.exists() and destination.read_bytes() != patched:
        raise ValueError('refusing to replace a different prepared service policy')
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(patched)
    report = {'schema_version': 1, 'runtime_build': '22H374', 'development_only': True,
        'installed': False, 'hardware_tested': False, 'source': str(source.resolve()),
        'source_sha256': hashlib.sha256(original).hexdigest(), 'path': str(destination.resolve()),
        'overlay': str((output/'overlay').resolve()), 'bytes': len(patched),
        'sha256': hashlib.sha256(patched).hexdigest(), 'policy': 'native-mach-reset-at-close',
        'endpoints': ['com.apple.fairplayd.versioned', 'com.apple.fairplayd'],
        'removed_descriptive_metadata': ['CFBundleInfoDictionaryVersion'],
        'daemon_code_modified': False, 'crypto_success_fabricated': False,
        'setup_completion_written': False,
        'reference': 'https://github.com/apple-oss-distributions/launchd/blob/main/man/launchd.plist.5',
        'inputs': {str(SELF.relative_to(ROOT)): digest(SELF)}}
    if resource_fork is not None:
        fork, chunks = replace_zlib_resource_chunks(resource_fork.read_bytes(),
            compression_attribute.read_bytes(), original, patched)
        (output/'resource-fork.bin').write_bytes(fork)
        report['resource_fork'] = {'path': str((output/'resource-fork.bin').resolve()),
            'bytes': len(fork), 'sha256': hashlib.sha256(fork).hexdigest(), 'changed_chunks': chunks}
    if bool(cache) != bool(cache_signature):
        raise ValueError('both launchd cache and detached signature are required')
    if cache is not None:
        from detached_codesign import HEADER, sign_data, verify_data
        original_cache, original_signature = cache.read_bytes(), cache_signature.read_bytes()
        declared, = struct.unpack_from('>I', original_signature, 4)
        if declared > len(original_signature) or any(original_signature[declared:]):
            raise ValueError('unsupported launchd cache signature padding')
        verify_data(original_cache, original_signature[:declared])
        modified, patches = patch_launchd_cache_readiness(original_cache)
        fields = HEADER.unpack_from(original_signature, 20)
        identifier = original_signature[20+fields[5]:20+fields[4]-1].decode('utf8')
        signature = sign_data(modified, identifier)
        entries = verify_data(modified, signature)
        if len(signature) != declared: raise ValueError('launchd signature allocation changed')
        signature += bytes(len(original_signature)-len(signature))
        for relative,payload in [('System/Library/xpc/launchd.plist', modified),
                                 ('System/Library/xpc/launchd.plist.sig', signature)]:
            target = output/'overlay'/relative; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(payload)
        module = struct.pack('<I16sI', 1, hashlib.sha256(signature).digest()[:16], len(entries))
        module += b''.join(cdhash+bytes((kind,0)) for cdhash,kind in entries)
        (output/'trust-cache.bin').write_bytes(module)
        report['launchd_cache'] = {'source': str(cache.resolve()), 'source_sha256': digest(cache),
            'sha256': hashlib.sha256(modified).hexdigest(), 'bytes': len(modified), 'patches': patches,
            'signature_source_sha256': digest(cache_signature), 'signature_sha256': hashlib.sha256(signature).hexdigest(),
            'signature_bytes': len(signature), 'cdhash': entries[0][0].hex(),
            'signature_identity_preserved': True, 'trust_cache': str((output/'trust-cache.bin').resolve())}
    (output/'manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


def patch_migration_unlock(image):
    """Skip only negative MKB lock-state results in the pinned native service.

    Preserve stash-load and lock-state calls, their errors, normal state0/3
    handling and positive locked states. No keybag result is rewritten.
    """
    expected = '4275137397d7df5d4c0f46d90eb5141ed3bb7f6759c41cc5c5ca019d5f14ee92'
    if hashlib.sha256(image).hexdigest() != expected:
        raise ValueError('native datamigrator reference hash mismatch')
    original = bytes.fromhex('750800341f0c00712008005400080034')
    if image[0xd038:0xd048] != original:
        raise ValueError('native datamigrator unlock branch mismatch')
    patched = bytearray(image)
    # cmp w0,#3; ccmp w0,#0,#4,ne; b.le original_epilogue.
    # If state==3, CCMP supplies Z=1; otherwise compare signed state to0.
    patched[0xd040:0xd048] = bytes.fromhex('0418407a0d080054')
    return bytes(patched)


def replace_zlib_resource_chunks(fork, attribute, original, replacement):
    """Keep native fork allocation, chunk offsets and Resource Manager map intact.

    Only changed logical chunks are encoded again. Refuse any that exceed their
    original compressed slot; padding is outside the descriptor's exact length.
    """
    import zlib
    if (len(attribute) != 16 or struct.unpack('<4sIQ', attribute) !=
            (b'fpmc', 4, len(original)) or len(original) != len(replacement) or not original):
        raise ValueError('unsupported compression attribute or logical allocation')
    if len(fork) < 264:
        raise ValueError('truncated resource fork')
    base, map_offset, data_size, map_size = struct.unpack_from('>4I', fork)
    count = (len(original) + 65535) // 65536
    if (base != 256 or map_size != 50 or map_offset + map_size != len(fork) or
            base + data_size != map_offset or 264 + count * 8 > map_offset or
            struct.unpack_from('>I', fork, base)[0] != data_size - 4 or
            struct.unpack_from('<I', fork, base + 4)[0] != count):
        raise ValueError('unsupported resource fork layout')

    def decode(compressed, length):
        if compressed[:1] == b'\xff':
            result = compressed[1:]
        else:
            if (len(compressed) < 3 or compressed[0] != 0x78 or compressed[1] & 0x20 or
                    int.from_bytes(compressed[:2], 'big') % 31):
                raise ValueError('invalid zlib chunk header')
            decoder = zlib.decompressobj(-15)
            result = decoder.decompress(compressed[2:], length + 1)
            if not decoder.eof or decoder.unconsumed_tail:
                raise ValueError('invalid or oversized deflate chunk')
        if len(result) != length:
            raise ValueError('decoded chunk size mismatch')
        return result

    result, changed, previous = bytearray(fork), [], 264 + count * 8
    for index in range(count):
        descriptor = 264 + index * 8
        relative, size = struct.unpack_from('<2I', fork, descriptor)
        start, end = 260 + relative, 260 + relative + size
        if size == 0 or start < previous or end > map_offset:
            raise ValueError('invalid or overlapping compressed chunk range')
        previous = end
        before, after = original[index * 65536:(index + 1) * 65536], replacement[index * 65536:(index + 1) * 65536]
        if decode(fork[start:end], len(before)) != before:
            raise ValueError('resource fork does not match the original service')
        if before == after:
            continue
        # decmpfs zlib chunks omit the Adler-32 trailer; native descriptors
        # supply the exact compressed length independently of the next offset.
        encoded = zlib.compress(after, 9)[:-4]
        if len(encoded) > size:
            raise ValueError('replacement compressed chunk exceeds its original allocation')
        result[start:end] = encoded + bytes(size - len(encoded))
        struct.pack_into('<I', result, descriptor + 4, len(encoded))
        if decode(encoded, len(after)) != after:
            raise ValueError('replacement chunk failed round trip')
        changed.append({'index': index, 'offset': start, 'old_bytes': size, 'new_bytes': len(encoded)})
    return bytes(result), changed


def prepare_display_services(source, output, resource_fork=None, compression_attribute=None):
    from macho_development_sign import resign_macho, signature_location, signature_blobs
    original = source.read_bytes()
    patched = patch_migration_unlock(original)
    signed, signature = resign_macho(patched, 'org.darwin.development.datamigrator')
    off, _, _, _ = signature_location(original)
    before, _ = signature_blobs(original[off:])
    after, _ = signature_blobs(signed[off:])
    if len(signed) != len(original) or any(before[k] != after[k] for k in (2,5,7)):
        raise ValueError('native service allocation or preserved policy changed')
    if signed[:0xd040] != original[:0xd040] or signed[0xd048:off] != original[0xd048:off]:
        raise ValueError('unrelated native service code changed')
    relative = Path('System/Library/PrivateFrameworks/DataMigration.framework/XPCServices/com.apple.datamigrator.xpc/com.apple.datamigrator')
    destination = output / 'overlay' / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    report = {'schema_version': 1, 'runtime_build': '22H374', 'development_only': True,
        'installed': False, 'hardware_tested': False, 'source': str(source.resolve()),
        'source_sha256': hashlib.sha256(original).hexdigest(), 'path': str(destination.resolve()),
        'overlay': str((output/'overlay').resolve()),
        'bytes': len(signed), 'sha256': hashlib.sha256(signed).hexdigest(), 'signature': signature,
        'code_patch': {'vm_address': '0x10000d040', 'file_offset': 0xd040,
            'before': original[0xd040:0xd048].hex(), 'after': signed[0xd040:0xd048].hex(),
            'effect': 'Skip PreBoard unlock wait only when original MKB lock-state query is negative; preserve normal0/3 and positive locked states'},
        'keybag_operations_implemented': False, 'keybag_success_fabricated': False,
        'migration_plugins_skipped': False, 'setup_completion_written': False,
        'inputs': {str(path.relative_to(ROOT)): digest(path) for path in
            (SELF, ROOT/'scripts/macho_development_sign.py')}}
    if destination.exists() and destination.read_bytes() != signed:
        raise ValueError('refusing to replace a different prepared service')
    destination.write_bytes(signed)
    destination.chmod(0o755)
    if resource_fork is not None:
        original_fork = resource_fork.read_bytes()
        attribute = compression_attribute.read_bytes()
        fork, chunks = replace_zlib_resource_chunks(original_fork, attribute, original, signed)
        fork_path = output / 'resource-fork.bin'
        fork_path.write_bytes(fork)
        report['resource_fork'] = {'path': str(fork_path.resolve()), 'bytes': len(fork),
            'source_sha256': hashlib.sha256(original_fork).hexdigest(),
            'sha256': hashlib.sha256(fork).hexdigest(), 'changed_chunks': chunks,
            'decmpfs_sha256': hashlib.sha256(attribute).hexdigest(),
            'allocation_changed': False, 'filesystem_metadata_changed': False,
            'chunk_offsets_changed': False, 'resource_map_changed': False}
    (output/'manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


def patch_system_app_install_wait(image):
    """Fail missing-app placeholders promptly and retain native install timeout."""
    if hashlib.sha256(image).hexdigest() != '36bedfaefa65bf10b6e166a8feef36669246a3a8008756d4e140ebc781585515':
        raise ValueError('native system-app migrator reference hash mismatch')
    if (image[0x4890:0x48a4].hex() != '0808008b08f17dd3691640f9290540f9280d00f9' or
            image[0x7638:0x7640].hex() != 'ff8302d1fc6f04a9'):
        raise ValueError('native system-app migration deadline calculation mismatch')
    patched = bytearray(image)
    # The preceding ADD computes count*5; replace LSL #3 with MOV X8,#0.
    # dispatch_time(NOW,0) is an immediate deadline, not DISPATCH_TIME_FOREVER.
    # Original dispatch_group_wait, timeout diagnostics and failure result run.
    patched[0x4894:0x4898] = bytes.fromhex('080080d2')
    # Return false before creating an install coordinator or localized on-disk
    # placeholder. The native caller logs failure and retains the missing app
    # and App Store request; this does not report an installed application.
    patched[0x7638:0x7640] = bytes.fromhex('00008052c0035fd6')
    return bytes(patched)


def patch_springboard_snapshot_wait(image):
    """Defer launch-image discovery and capture through the native defaults path."""
    if hashlib.sha256(image).hexdigest() != 'd3fe19db1b7a97ab9d5299d18fd9903865094ca159ec2cbb111b905516afed10':
        raise ValueError('native SpringBoard migrator reference hash mismatch')
    if (image[0xb418:0xb420].hex() != 'ff4301d1f65702a9' or
            image[0xb578:0xb588].hex() != 'f40300aa130440f90a090094f50300aa' or
            image[0xb618:0xb624].hex() != 'e10300aae00314aa61080094'):
        raise ValueError('native SpringBoard launch-image path mismatch')
    patched = bytearray(image)
    # The launch-image-only discovery method returns nil without entering UIKit
    # application queries. Its caller retains nil and continues other migration.
    patched[0xb418:0xb420] = bytes.fromhex('000080d2c0035fd6')
    # Keep the capture method's real prologue/epilogue. Zero only the three
    # objects released on exit (x19,x20,SP+0x28), then branch to its existing
    # NSUserDefaults code at0xb624. That code requests post-migration capture.
    patched[0xb578:0xb588] = bytes.fromhex('140080d2130080d2ff1700f928000014')
    return bytes(patched)


def prepare_system_app_migrator(source, output, resource_fork=None, compression_attribute=None):
    return prepare_native_migration_bundle(source, output, patch_system_app_install_wait,
        'systemappmigrator', 'SystemAppMigrator.migrator/SystemAppMigrator', [(0x4894, 4), (0x7638, 8)],
        'Fail missing-app placeholder materialization and use immediate native install-group timeout; retain native missing/error paths',
        {'app_install_success_fabricated': False, 'asynchronous_installations_may_remain_pending': True,
         'missing_app_placeholder_materialization_skipped': True},
        resource_fork, compression_attribute)


def prepare_springboard_migrator(source, output, resource_fork=None, compression_attribute=None):
    return prepare_native_migration_bundle(source, output, patch_springboard_snapshot_wait,
        'springboardmigrator', 'SpringBoard.migrator/SpringBoard', [(0xb418, 8), (0xb578, 16)],
        'Defer launch-image discovery and capture; execute the original post-migration capture request',
        {'snapshot_success_fabricated': False, 'launch_images_deferred': True,
         'original_snapshot_timeout_seconds': 300, 'post_migration_capture_flag_retained': True},
        resource_fork, compression_attribute)


def prepare_native_migration_bundle(source, output, patch, identifier, relative, patches, effect,
                                    properties, resource_fork=None, compression_attribute=None):
    from macho_development_sign import resign_macho, signature_location, signature_blobs
    original = source.read_bytes()
    edited = patch(original)
    signed, signature = resign_macho(edited, 'org.darwin.development.' + identifier)
    off, _, _, _ = signature_location(original, allow_bundle=True)
    before, _ = signature_blobs(original[off:]); after, _ = signature_blobs(signed[off:])
    cursor = 0
    for offset, size in patches:
        if (not cursor <= offset < offset + size <= off or
                signed[cursor:offset] != original[cursor:offset] or
                signed[offset:offset+size] != edited[offset:offset+size]):
            raise ValueError('unrelated native migration bundle code changed')
        cursor = offset + size
    if (len(signed) != len(original) or before[2] != after[2] or
            set(after) != {0, 2} or signed[cursor:off] != original[cursor:off]):
        raise ValueError('unrelated native migration bundle code or signing policy changed')
    destination = output/'overlay/System/Library/DataClassMigrators'/relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() and destination.read_bytes() != signed:
        raise ValueError('refusing to replace a different prepared migration bundle')
    destination.write_bytes(signed); destination.chmod(0o755)
    report = {'schema_version': 1, 'runtime_build': '22H374', 'development_only': True,
        'installed': False, 'hardware_tested': False, 'source': str(source.resolve()),
        'source_sha256': hashlib.sha256(original).hexdigest(), 'path': str(destination.resolve()),
        'bytes': len(signed), 'sha256': hashlib.sha256(signed).hexdigest(), 'signature': signature,
        'code_patches': [{'vm_address': hex(offset), 'file_offset': offset, 'before': original[offset:offset+size].hex(),
            'after': signed[offset:offset+size].hex()} for offset, size in patches],
        'effect': effect,
        'migration_plugins_skipped': False,
        'setup_completion_written': False, 'keybag_operations_implemented': False,
        **properties,
        'inputs': {str(path.relative_to(ROOT)): digest(path) for path in (SELF, ROOT/'scripts/macho_development_sign.py')}}
    if len(report['code_patches']) == 1:
        report['code_patch'] = dict(report['code_patches'][0], effect=effect)
    if resource_fork is not None:
        original_fork, attribute = resource_fork.read_bytes(), compression_attribute.read_bytes()
        fork, chunks = replace_zlib_resource_chunks(original_fork, attribute, original, signed)
        path = output/'resource-fork.bin'; path.write_bytes(fork)
        report['resource_fork'] = {'path': str(path.resolve()), 'bytes': len(fork),
            'source_sha256': hashlib.sha256(original_fork).hexdigest(), 'sha256': hashlib.sha256(fork).hexdigest(),
            'decmpfs_sha256': hashlib.sha256(attribute).hexdigest(), 'changed_chunks': chunks,
            'allocation_changed': False, 'filesystem_metadata_changed': False}
    (output/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def verify_display_readback(prefix):
    """Export actual DPU captures, comparing pixels only when a software source exists."""
    import zlib
    prefix = prefix.resolve()
    text = Path(str(prefix) + '-validation.txt').read_text()
    native_window = bool(re.search(r'^NATIVE_WINDOW_BEGIN native_content_only=1 ', text, re.M))
    size = re.search(r'^SWAP_TEST_SIZE result=0x0 width=(\d+) height=(\d+)$', text, re.M)
    layout = re.search(r'^SWAP_TEST_REFERENCE saved=1 bytes=(\d+) row=(\d+) ', text, re.M)
    if native_window:
        initial = json.loads(Path(str(prefix) + '-before-scanout.json').read_text())
        width, height = initial.get('Width', 0), initial.get('Height', 0)
        if not (0 < width <= 4096 and 0 < height <= 4096):
            raise ValueError('invalid native window geometry')
        allocation = source_stride = 0
    elif not size or not layout:
        raise ValueError('capture lacks a successful native size query and software source export')
    else:
        width, height = map(int, size.groups())
        allocation, source_stride = map(int, layout.groups())
    if not native_window and not (0 < width <= 4096 and 0 < height <= 4096 and width * 4 <= source_stride <= 65536
            and allocation == source_stride * height and allocation <= 32 * 1024 * 1024):
        raise ValueError('invalid software source geometry')

    def visible(path, stride):
        data = path.read_bytes()
        if len(data) != stride * height:
            raise ValueError('truncated or oversized framebuffer: ' + str(path))
        return data, b''.join(data[y * stride:y * stride + width * 4] for y in range(height))

    def png(path, bgra):
        # Standard-library RGB encoder; no image-generation or host imaging
        # dependency. Discard only alpha, which is not a panel color channel.
        filtered = bytearray()
        for y in range(height):
            row = bgra[y * width * 4:(y + 1) * width * 4]
            rgb = bytearray(width * 3)
            rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
            filtered.extend(b'\0' + rgb)
        def chunk(kind, data):
            return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
        path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>2I5B', width, height, 8, 2, 0, 0, 0)) +
                         chunk(b'IDAT', zlib.compress(filtered)) + chunk(b'IEND', b''))

    source = None
    report = {'mode': 'native-window' if native_window else 'software-source',
              'size': [width, height], 'captures': {}, 'optical_presentation_verified': False}
    if not native_window:
        source_raw, source = visible(Path(str(prefix) + '-source.bgra'), source_stride)
        png(Path(str(prefix) + '-source.png'), source)
        report.update(source_size=[width, height], source_stride=source_stride,
            source_raw_sha256=hashlib.sha256(source_raw).hexdigest(),
            source_visible_bgra_sha256=hashlib.sha256(source).hexdigest())
    for phase in ('before', 'on', 'after'):
        stem = str(prefix) + '-' + phase + '-scanout'
        metadata = json.loads(Path(stem + '.json').read_text())
        stride = metadata.get('RowBytes', 0)
        if (metadata.get('Version') != 1 or metadata.get('PixelFormat') != 0x42475241 or
                (metadata.get('Width'), metadata.get('Height')) != (width, height) or
                not width * 4 <= stride <= 65536 or stride * height > 16 * 1024 * 1024):
            raise ValueError('incompatible scanout geometry: ' + phase)
        raw, pixels = visible(Path(stem + '.bgra'), stride)
        if metadata.get('Bytes') != len(raw):
            raise ValueError('scanout metadata byte count differs: ' + phase)
        stable = (metadata['SourceAddress'] & 0xffffffff) == (metadata['SourceAddressAfter'] & 0xffffffff)
        matches = sum(pixels[i:i + 4] == source[i:i + 4] for i in range(0, len(source), 4)) if source is not None else None
        image = Path(str(prefix) + '-' + phase + '.png')
        png(image, pixels)
        report['captures'][phase] = {'raw_sha256': hashlib.sha256(raw).hexdigest(),
            'visible_bgra_sha256': hashlib.sha256(pixels).hexdigest(),
            'source_address_stable': stable, 'matching_bgra_pixels': matches,
            'total_pixels': width * height, 'pixels_match_source': matches == width * height and stable if source is not None else None,
            'png': str(image), 'metadata': metadata}
    for name in ('END', 'WAIT'):
        result = re.search(r'^SWAP_TEST_' + name + r' result=(0x[0-9a-f]+)', text, re.M)
        report['native_swap_' + name.lower()] = result[1] if result else None
    flags = re.search(r'^SWAP_TEST_WAIT result=0x[0-9a-f]+ flags=(\d+) ', text, re.M)
    report['native_swap_wait_flags'] = int(flags[1]) if flags else None
    retire = re.search(r'^SWAP_RETIREMENT_WAIT result=(0x[0-9a-f]+) flags=0 first_token=(\d+) replacement_token=(\d+) elapsed_ms=([0-9.]+)$', text, re.M)
    if retire:
        report['native_retirement_wait'] = {'result': retire[1], 'flags': 0,
            'first_token': int(retire[2]), 'replacement_token': int(retire[3]), 'elapsed_ms': float(retire[4])}
    report['scanout_pixels_verified'] = report['captures']['after']['pixels_match_source']
    report['native_window_captured'] = native_window and all(c['source_address_stable'] for c in report['captures'].values())
    report['scanout_changed'] = report['captures']['before']['raw_sha256'] != report['captures']['after']['raw_sha256']
    path = Path(str(prefix) + '-readback.json')
    path.write_text(json.dumps(report, indent=2) + '\n')
    if native_window:
        return {'report': str(path), 'native_window_captured': report['native_window_captured'],
                'scanout_changed': report['scanout_changed'], 'software_source_submitted': False,
                'size': [width, height], 'optical_presentation_verified': False}
    return {'report': str(path), 'scanout_pixels_verified': report['scanout_pixels_verified'],
            'scanout_changed': report['scanout_changed'], 'matching_bgra_pixels': report['captures']['after']['matching_bgra_pixels'],
            'total_pixels': width * height, 'native_swap_end': report['native_swap_end'],
            'native_swap_wait': report['native_swap_wait'], 'native_swap_wait_flags': report['native_swap_wait_flags'],
            'native_retirement_wait': report.get('native_retirement_wait'), 'optical_presentation_verified': False}


def collect_power_clocks():
    """Generate reviewed CC descriptors from hash-pinned reference C, never execute it."""
    from fractions import Fraction
    reference = ROOT / 'docs/power/reference'
    manifest = json.loads((reference / 'sources.json').read_text())
    for filename, entry in manifest.items():
        if digest(reference / filename) != entry['sha256']:
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
        output = ['// SPDX-License-Identifier: GPL-2.0-only', '// Generated by scripts/toolchain.py collect-power from pinned Linux v6.12 data.',
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
        (DRIVERS_ROOT/'clock'/module/'ClockTables.h').write_text('\n'.join(output)+'\n')
        print(module+': '+str(len(roots))+' roots, '+str(len(branches))+' branches, '+str(len(domains))+' domains')


def snapshot_input(source, destination):
    """Freeze mutable build inputs before a collection consumes them."""
    for _ in range(3):
        before = digest(source)
        run(['cp', '--reflink=auto', source, destination])
        if digest(destination) == before == digest(source):
            return destination
    raise RuntimeError(f'input changed repeatedly while snapshotting: {source}')


def verify_collection(collection=None, output=None):
    if output is None:
        configured = driver_configuration().get('collections', {}).get(collection, {}).get('output')
        if not configured:
            raise RuntimeError('select --output or a configured --collection')
        output = DRIVERS_ROOT / configured
    output = output.resolve()
    manifest = json.loads(Path(str(output) + '.manifest.json').read_text())
    kernel = (output.parent / manifest['inputs']['kernel']).resolve()
    provider = (output.parent / manifest['inputs']['provider']).resolve()
    for path, key in ((output, 'output_sha256'), (kernel, 'kernel_sha256'), (provider, 'provider_sha256')):
        if digest(path) != manifest[key]:
            raise RuntimeError(f'collection input/output differs from manifest: {path}')
    expected = [arg for name in manifest['bundle_order'] for arg in ('--expected-id', name)]
    run([host('python3'), ROOT / 'scripts/verify-kernelcache.py', output,
         '--report', str(output) + '.verification.json', '--source-kernelcache', provider,
         '--local-kernel', kernel, *expected])


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, ValueError, zipfile.BadZipFile, subprocess.SubprocessError) as e:
        print(f'toolchain: {e}', file=sys.stderr)
        sys.exit(1)
