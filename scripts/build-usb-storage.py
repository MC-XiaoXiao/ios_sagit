#!/usr/bin/env python3
"""Build the pinned UFS exporter with a polled UART command console; no flashing."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT/'tools/src/linux-simple-mass-storage'
OUT = ROOT/'build/usb-storage'
SPEC = json.loads((ROOT/'tools/usb-storage.json').read_text())
FIRMWARE_EFI = ROOT/'boot/edk2-msm8998/Platforms/Msm8998FamilyPkg/LinuxSimpleMassStorage/LinuxSimpleMassStorage.efi'


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def sha(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def bootstrap():
    OUT.mkdir(parents=True, exist_ok=True)
    if not SOURCE.exists():
        run('git', 'clone', '--no-checkout', SPEC['source']['url'], SOURCE)
        run('git', '-C', SOURCE, 'checkout', '--detach', SPEC['source']['commit'])
    commit = subprocess.check_output(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD'], text=True).strip()
    if commit != SPEC['source']['commit']:
        raise ValueError('unexpected mass-storage source commit')
    for name, archive in SPEC['archives'].items():
        path = ROOT/'tools/cache'/archive['file']
        if not path.exists():
            temporary = path.with_suffix(path.suffix+'.download')
            urllib.request.urlretrieve(archive['url'], temporary)
            temporary.replace(path)
        if sha(path) != archive['sha256']:
            raise ValueError(f'{name} source archive hash mismatch')
        marker = SOURCE/name/('.mi6-source-'+archive['commit'])
        if not marker.exists():
            (SOURCE/name).mkdir(parents=True, exist_ok=True)
            if any((SOURCE/name).iterdir()):
                raise ValueError(f'{name} source exists without its pinned-source marker; refusing to overwrite it')
            with tarfile.open(path) as package:
                for member in package:
                    relative = Path(*Path(member.name).parts[1:])
                    if not relative.parts:
                        continue
                    member.name = str(relative)
                    package.extract(member, SOURCE/name, filter='data')
            marker.touch()
    if not (SOURCE/'.stamp-submodule').exists():
        (SOURCE/'.stamp-submodule').touch()
    for name in SPEC['patches']:
        patch = ROOT/'tools/patches'/name
        applied = subprocess.run(['git', '-C', str(SOURCE), 'apply', '--reverse', '--check', str(patch)], capture_output=True)
        if applied.returncode:
            run('git', '-C', SOURCE, 'apply', '--check', patch)
            run('git', '-C', SOURCE, 'apply', patch)
    for name in ('serial-command.h', 'serial-control.c'):
        src, dst = ROOT/'boot/storage'/name, SOURCE/'src'/name
        if not dst.exists() or dst.read_bytes() != src.read_bytes():
            shutil.copy2(src, dst)


def build(jobs):
    bootstrap()
    env = dict(os.environ, KBUILD_OUTPUT=str(OUT/'kernel'))
    (OUT/'kernel').mkdir(exist_ok=True)
    common = ['CROSS_COMPILE=aarch64-none-linux-gnu-', 'TARGET=aarch64-none-linux-gnu',
              f'SYSROOT={SOURCE / "sysroot"}', f'-j{jobs}']
    with (OUT/'sysroot-build.log').open('w') as log:
        run('make', 'sysroot/include', *common, cwd=SOURCE, env=env, stdout=log, stderr=subprocess.STDOUT)
    with (OUT/'init-build.log').open('w') as log:
        run('make', 'init', *common, 'X_CCFLAGS=-Os -g -std=gnu17 -nostdinc -nolibc -nostartfiles -nostdlib',
            cwd=SOURCE, env=env, stdout=log, stderr=subprocess.STDOUT)
    # A deterministic newc archive contains only the static PID 1 binary.
    raw = (SOURCE/'init').read_bytes()
    def entry(name, data, mode, ino):
        encoded = name.encode()+b'\0'
        fields = (ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(encoded), 0)
        record = b'070701'+b''.join(f'{v:08x}'.encode() for v in fields)+encoded
        record += bytes(-len(record)%4)
        record += data
        return record+bytes(-len(data)%4)
    archive = entry('init', raw, 0o100755, 1)+entry('TRAILER!!!', b'', 0, 0)
    ramdisk = SOURCE/'initramfs.cpio'
    if not ramdisk.exists() or ramdisk.read_bytes() != archive:
        ramdisk.write_bytes(archive)
    kernel = SOURCE/'kernel'
    output = OUT/'kernel'
    output.mkdir(exist_ok=True)
    config = output/'.config'
    if not config.exists():
        shutil.copy2(SOURCE/'configs/msm8998-minimal.config', config)
    run(kernel/'scripts/config', '--file', config, '--disable', 'WERROR',
        '--enable', 'TTY', '--enable', 'SERIAL_MSM', '--enable', 'SERIAL_MSM_CONSOLE',
        '--enable', 'DEVTMPFS', '--enable', 'DEVTMPFS_MOUNT', '--enable', 'PRINTK', '--enable', 'PRINTK_TIME',
        '--set-str', 'CMDLINE', 'earlycon=msm_serial_dm,0xc1b0000 console=ttyMSM1,115200n8 efi=novamap pd_ignore_unused clk_ignore_unused',
        '--set-str', 'INITRAMFS_SOURCE', ramdisk)
    kernel_args = ['ARCH=arm64', 'CROSS_COMPILE=aarch64-none-linux-gnu-', f'O={output}',
                   'HOSTCFLAGS=-O2 -std=gnu17', 'KBUILD_BUILD_USER=mi6', 'KBUILD_BUILD_HOST=local',
                   'KBUILD_BUILD_TIMESTAMP=2026-10-15', 'KBUILD_BUILD_VERSION=1']
    with (OUT/'kernel-build.log').open('w') as log:
        run('make', '-C', kernel, *kernel_args, 'olddefconfig', stdout=log, stderr=subprocess.STDOUT)
        required = ('CONFIG_TTY=y', 'CONFIG_SERIAL_MSM=y', 'CONFIG_SERIAL_MSM_CONSOLE=y', 'CONFIG_DEVTMPFS=y')
        if not all(value in config.read_text().splitlines() for value in required):
            raise ValueError('kernel configuration lost required UART/device support')
        run('make', '-C', kernel, *kernel_args, f'-j{jobs}', 'Image', stdout=log, stderr=subprocess.STDOUT)
    image = OUT/'LinuxSimpleMassStorage.efi'
    shutil.copy2(output/'arch/arm64/boot/Image', image)
    report = {'schema':1, 'source_commit':SPEC['source']['commit'], 'archives':SPEC['archives'],
              'uart_address':SPEC['uart_address'], 'serial_commands':['status','help','reboot'],
              'image':str(image), 'image_sha256':sha(image), 'image_bytes':image.stat().st_size,
              'init_sha256':sha(SOURCE/'init'), 'kernel_config_sha256':sha(config),
              'firmware_flashed':False}
    (OUT/'build.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f'Built {image} ({image.stat().st_size} bytes); no firmware flashed.')


def install_source():
    image = OUT/'LinuxSimpleMassStorage.efi'
    report = json.loads((OUT/'build.json').read_text())
    if sha(image) != report['image_sha256']:
        raise ValueError('exporter differs from build report')
    backup = OUT/'LinuxSimpleMassStorage.original.efi'
    if not backup.exists():
        if sha(FIRMWARE_EFI) != SPEC['original_efi_sha256']:
            raise ValueError('unexpected original firmware exporter; preserve it before replacement')
        shutil.copy2(FIRMWARE_EFI, backup)
    shutil.copy2(image, FIRMWARE_EFI)
    print('Updated the EDK2 source payload; rebuilding/flashing firmware is a separate operation.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['bootstrap', 'build', 'install-source'])
    parser.add_argument('-j', '--jobs', type=int, default=3)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('jobs must be positive')
    if args.command == 'bootstrap': bootstrap()
    elif args.command == 'install-source': install_source()
    else: build(args.jobs)
