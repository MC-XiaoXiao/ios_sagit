"""Pinned host toolchain, SDK tools and compiler wrappers."""
import concurrent.futures
import hashlib
import html
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import struct
import sys
import tarfile
import zipfile
from project.build import context as build_state


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
    loader = build_state.PREFIX / 'libexec/ld.so'
    path = Path(tool)
    with path.open('rb') as stream:
        is_elf = stream.read(4) == b'\x7fELF'
    if loader.exists() and is_elf and path.is_relative_to(build_state.PREFIX):
        libraries = str(build_state.PREFIX / 'lib/glibc') + os.pathsep + str(build_state.PREFIX / 'lib')
        os.execv(str(loader), [str(loader), '--library-path', libraries,
                              '--argv0', str(build_state.BIN / path.name), str(path), *map(str, args)])
    os.execv(str(tool), [str(tool), *map(str, args)])


def host(name):
    # Avoid recursively resolving our own multicall aliases.
    search = os.pathsep.join(p for p in os.environ.get('PATH', '').split(os.pathsep)
                             if Path(p).resolve() != build_state.BIN.resolve())
    found = shutil.which(name, path=search)
    if not found:
        raise RuntimeError(f'missing host dependency: {name}')
    return found


def digest(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def compiler(cxx=False):
    path = build_state.PREFIX / 'libexec/native/clang-real'
    if not path.exists():
        path = build_state.BIN / 'clang-real'
    return str(path) if path.exists() else host('clang++' if cxx else 'clang')


def compiler_resource():
    meta = build_state.PREFIX / 'compiler.json'
    if meta.exists():
        return ['-resource-dir', str(build_state.PREFIX / 'lib/clang' / json.loads(meta.read_text())['resource_version'])]
    return []


def bundle_host_runtime():
    native = build_state.PREFIX / 'libexec/native'
    glibc = build_state.PREFIX / 'lib/glibc'
    native.mkdir(parents=True, exist_ok=True)
    glibc.mkdir(parents=True, exist_ok=True)
    result = subprocess.check_output(['ldd', compiler()], text=True)
    for name, path in re.findall(r'\s+(\S+) => (/\S+) \(', result):
        if name in ('libc.so.6', 'libm.so.6'):
            shutil.copy2(Path(path).resolve(), glibc / name)
    match = re.search(r'(/\S*ld-linux[^\s]*) \(', result)
    if not match:
        raise RuntimeError('cannot locate the host ELF loader')
    shutil.copy2(Path(match[1]).resolve(), build_state.PREFIX / 'libexec/ld.so')
    # Every native executable uses the same multicall dispatcher and bundled
    # loader. Keep glibc out of global LD_LIBRARY_PATH for host shell tools.
    for path in list(build_state.BIN.glob('*')) + [build_state.PREFIX / 'libexec/migcom']:
        if path.is_symlink() or not path.is_file():
            continue
        with path.open('rb') as stream:
            is_elf = stream.read(4) == b'\x7fELF'
        if is_elf:
            path.rename(native / path.name)
            path.symlink_to(os.path.relpath(build_state.SELF, path.parent))


def bundle_compiler():
    """Freeze stage-0 Clang and non-libc ELF dependencies in the prefix."""
    clang = Path(host('clang')).resolve()
    shutil.copy2(clang, build_state.BIN / 'clang-real')
    resource = Path(subprocess.check_output([clang, '-print-resource-dir'], text=True).strip())
    target = build_state.PREFIX / 'lib/clang' / resource.name
    shutil.copytree(resource / 'include', target / 'include', dirs_exist_ok=True)
    exclusions = {'libc.so.6', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2', 'librt.so.1'}
    dependencies = {}
    for executable in [clang, build_state.BIN / 'arm-apple-darwin11-ld', build_state.BIN / 'tcsh']:
        result = subprocess.check_output(['ldd', str(executable)], text=True)
        for name, path in re.findall(r'\s+(\S+) => (/\S+) \(', result):
            if name not in exclusions and not (build_state.PREFIX / 'lib' / name).exists():
                source = Path(path).resolve()
                shutil.copy2(source, build_state.PREFIX / 'lib' / name)
                dependencies[name] = {'origin': str(source), 'sha256': digest(source)}
    metadata = {'compiler_version': subprocess.check_output([clang, '--version'], text=True),
                'compiler_sha256': digest(clang), 'resource_version': resource.name,
                'host': os.uname().machine, 'libraries': dependencies}
    (build_state.PREFIX / 'compiler.json').write_text(json.dumps(metadata, indent=2) + '\n')
    for name in ('clang', 'llvm-libs', 'libbsd', 'gcc-libs', 'gcc', 'glibc', 'libedit',
                 'libffi', 'icu', 'ncurses', 'zlib', 'zstd', 'libxml2', 'util-linux-libs'):
        license_dir = Path('/usr/share/licenses') / name
        if license_dir.exists():
            shutil.copytree(license_dir, build_state.PREFIX / 'licenses' / name, dirs_exist_ok=True)


def fetch(name, spec):
    cache = build_state.ROOT / 'tools/cache' / spec['archive']
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
    build_state.BIN.mkdir(parents=True, exist_ok=True)
    for name in ['clang-arm64', 'clang++-arm64', 'xcrun', 'host-cc', 'plutil',
                 'sed', 'install', 'makekdebugevents', 'iig', 'mig']:
        p = build_state.BIN / name
        if not p.exists():
            p.symlink_to(os.path.relpath(build_state.SELF, build_state.BIN))


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
    catalog = build_state.TOOLS / 'mirrors.json'
    if not catalog.exists():
        raise RuntimeError('missing source mirror lockfile')
    def fetch_one(item):
        relative, info = item
        path = build_state.TOOLS / relative
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
    manifest = json.loads(build_state.LOCK.read_text())
    def download(item):
        name, spec = item
        return name, fetch(name, spec)
    results = dict(concurrent.futures.ThreadPoolExecutor(4).map(download, manifest.items()))
    for name, spec in manifest.items():
        archive, checksum = results[name]
        spec['sha256'] = checksum
        if name == 'sdk':
            dest = build_state.ROOT / 'tools/sdk'
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
            if not (build_state.SRC / 'Makefile').exists():
                unpack(archive, build_state.SRC)
        else:
            unpack(archive, build_state.ROOT / 'tools/src' / name)
    build_state.LOCK.write_text(json.dumps(manifest, indent=2) + '\n')
    fetch_mirrors()
    aliases()
    env = dict(os.environ, CC=host('clang'), CXX=host('clang++'))
    env['PATH'] = str(build_state.BIN) + os.pathsep + env['PATH']
    env['LD_LIBRARY_PATH'] = str(build_state.PREFIX / 'lib') + os.pathsep + env.get('LD_LIBRARY_PATH', '')
    log = build_state.BUILD / 'logs/bootstrap.log'
    for name in ('unifdef', 'tcsh'):
        if (build_state.BIN / name).exists():
            continue
        source = build_state.ROOT / 'tools/src' / name
        print(f'Build {name}', flush=True)
        if name == 'unifdef':
            (source / 'version.sh').write_text('V="unifdef-2.12"\nD="2020-02-14"\n')
            run(['make', f'-j{jobs}', f'prefix={build_state.PREFIX}', 'install'], cwd=source, log=log, env=env)
        else:
            if not (source / 'configure').exists():
                run(['autoreconf', '-fi'], cwd=source, log=log, env=env)
            work = build_state.BUILD / 'obj/host/tcsh'
            work.mkdir(parents=True, exist_ok=True)
            run([source / 'configure', f'--prefix={build_state.PREFIX}'], cwd=work, log=log, env=env)
            run(['make', f'-j{jobs}', 'install'], cwd=work, log=log, env=env)
    if not (build_state.PREFIX / 'lib/libdispatch.so').exists():
        print('Build host libdispatch', flush=True)
        work = build_state.BUILD / 'obj/host/libdispatch'
        run(['cmake', '-S', build_state.ROOT / 'tools/src/host-dispatch', '-B', work, '-G', 'Ninja',
             f'-DCMAKE_INSTALL_PREFIX={build_state.PREFIX}', '-DCMAKE_BUILD_TYPE=Release',
             '-DCMAKE_C_FLAGS=-Wno-error=unused-but-set-global',
             '-DBUILD_SHARED_LIBS=ON', '-DENABLE_TESTING=OFF', '-DBUILD_TESTING=OFF',
             '-DENABLE_SWIFT=OFF', '-DUSE_DTRACE=OFF'], log=log, env=env)
        run(['cmake', '--build', work, '-j', jobs], log=log, env=env)
        run(['cmake', '--install', work], log=log, env=env)
    if not (build_state.BIN / 'arm-apple-darwin11-ld').exists():
        print('Build cctools/ld64', flush=True)
        source = build_state.ROOT / 'tools/src/cctools-port/cctools'
        work = build_state.BUILD / 'obj/host/cctools'
        work.mkdir(parents=True, exist_ok=True)
        run([source / 'configure', '--target=arm-apple-darwin11', f'--prefix={build_state.PREFIX}',
             f'--with-llvm-config={host("llvm-config")}', f'--with-libdispatch={build_state.PREFIX}',
             f'--with-libblocksruntime={build_state.PREFIX}'], cwd=work, log=log, env=env)
        run(['make', f'-j{jobs}'], cwd=work, log=log, env=env)
        run(['make', 'install'], cwd=work, log=log, env=env)
    migcom = build_state.PREFIX / 'libexec/migcom'
    if not migcom.exists():
        print('Build MIG', flush=True)
        source = build_state.ROOT / 'tools/src/bootstrap_cmds/migcom.tproj'
        work = build_state.BUILD / 'obj/host/mig'
        work.mkdir(parents=True, exist_ok=True)
        migcom.parent.mkdir(parents=True, exist_ok=True)
        run(['bison', '-y', '-d', '-o', work / 'y.tab.c', source / 'parser.y'], log=log)
        run(['flex', '-o', work / 'lex.yy.c', source / 'lexxer.l'], log=log)
        names = 'error global header mig routine server statement string type user utils'.split()
        run([host('clang'), '-std=gnu11', '-O2', '-D_GNU_SOURCE', '-Ulinux',
             '-DMIG_VERSION="mig-138"', f'-I{source}', f'-I{work}', '-idirafter',
             build_state.ROOT / 'tools/src/cctools-port/cctools/include/foreign',
             *[source / (n + '.c') for n in names], work / 'y.tab.c', work / 'lex.yy.c',
             '-o', migcom], log=log)
        driver = (source / 'mig.sh').read_text().replace('`/usr/bin/arch`', '`uname -m`')
        (build_state.PREFIX / 'libexec/mig-driver').write_text(driver)
        (build_state.PREFIX / 'libexec/mig-driver').chmod(0o755)
    for name in ('libmacho', 'libstuff'):
        source = build_state.BUILD / f'host/cctools/{name}/.libs/{name}.a'
        if source.exists():
            shutil.copy2(source, build_state.PREFIX / f'lib/{name}.a')
        elif not (build_state.PREFIX / f'lib/{name}.a').exists():
            raise RuntimeError(f'missing host archive: {name}')
    if not (build_state.PREFIX / 'compiler.json').exists():
        bundle_compiler()
    relocate_elf()
    if not (build_state.PREFIX / 'libexec/ld.so').exists():
        bundle_host_runtime()
    print('Toolchain ready', flush=True)


def relocate_elf():
    """Replace install-prefix RUNPATH strings in-place; no patchelf dependency."""
    for path in list(build_state.BIN.glob('*')) + list((build_state.PREFIX / 'lib').glob('*.so*')):
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
                if str(build_state.PREFIX).encode() in value:
                    new = b'$ORIGIN/../lib:$ORIGIN:/usr/lib'
                    if len(new) <= len(value):
                        data[pos:end] = new + b'\0' * (len(value) - len(new))
        path.write_bytes(data)


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
    cache = build_state.TOOLS / 'iig'
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
        info = json.loads((build_state.TOOLS / 'mirrors.json').read_text()).get('iig/' + filename)
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
    native = build_state.PREFIX / 'libexec/native' / name
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
                new.append(str(build_state.SDK / 'usr/local/lib/kernel' / build_state.TARGET.lower() / ('lib' + arg[2:] + '.a')))
            else:
                new.append(arg)
        os.environ['LD_LIBRARY_PATH'] = str(build_state.PREFIX / 'lib') + os.pathsep + os.environ.get('LD_LIBRARY_PATH', '')
        modes = ['--driver-mode=g++'] if '++' in name else []
        exec_tool(compiler('++' in name),
                  [*modes, '-no-canonical-prefixes', '--target=arm64-apple-ios18.6', f'-fuse-ld={build_state.BIN}/arm-apple-darwin11-ld',
                   *compiler_resource(),
                   *new, '-Wno-unknown-warning-option', '-Wno-unused-command-line-argument',
                   f'-I{build_state.TOOLS}/compat', f'-I{build_state.TOOLS}/private-headers',
                   '-Wno-shadow-header', '-Wno-unsafe-buffer-usage', '-Wno-reserved-identifier',
                   '-Wno-pre-c11-compat', '-Wno-deprecated-non-prototype', '-fheinous-gnu-extensions'])
    if name == 'mig':
        os.environ['MIGCOM'] = str(build_state.PREFIX / 'libexec/migcom')
        os.environ['MIGCC'] = str(build_state.BIN / 'clang-arm64')
        exec_tool(build_state.PREFIX / 'libexec/mig-driver', args)
    if name == 'host-cc':
        base = build_state.ROOT / 'tools/src/cctools-port/cctools'
        flags = ['-D__DARWIN_UNIX03', '-DHAVE_BCMP', '-DHAVE_BZERO', '-DHAVE_BCOPY',
                 '-DHAVE_INDEX', '-DHAVE_RINDEX', '-D__private_extern__=extern', '-DO_SYMLINK=0',
                 '-include', 'bsd/stdlib.h', '-include', 'bsd/string.h', '-include', 'bsd/unistd.h',
                 f'-I{build_state.TOOLS}/compat', f'-I{base}/include', f'-I{base}/include/foreign']
        if '-c' not in args:
            args += [str(build_state.PREFIX / 'lib/libmacho.a'), str(build_state.PREFIX / 'lib/libstuff.a'),
                     '-lbsd', '-luuid', '-lz', '-ldl']
        os.environ['LD_LIBRARY_PATH'] = str(build_state.PREFIX / 'lib') + os.pathsep + os.environ.get('LD_LIBRARY_PATH', '')
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
            print(build_state.SDK)
        elif args == ['-show-sdk-version']:
            print('18.6')
        elif args == ['-show-sdk-platform-path']:
            print(build_state.BUILD / 'obj/platform/iPhoneOS.platform')
        elif args[:1] == ['-find']:
            tool = args[1]
            mapped = {'clang': 'clang-arm64', 'cc': 'clang-arm64', 'clang++': 'clang++-arm64',
                      'c++': 'clang++-arm64', 'gm4': 'm4'}
            tool = mapped.get(tool, tool)
            if tool in ('strip', 'lipo', 'libtool', 'nm', 'nmedit', 'otool', 'ctf_insert', 'ranlib'):
                print(build_state.BIN / ('arm-apple-darwin11-' + tool))
            elif tool == 'migcom':
                print(build_state.PREFIX / 'libexec/migcom')
            elif (build_state.BIN / tool).exists():
                print(build_state.BIN / tool)
            elif tool in ('embedded_device_map', 'ctfconvert', 'ctfmerge', 'ctfdump', 'codesign_allocate', 'dsymutil', 'docc'):
                print(host('false'))
            else:
                print(host(tool))
        else:
            raise RuntimeError(f'unsupported xcrun: {args}')
        return
    raise RuntimeError(f'unknown tool alias: {name}')
