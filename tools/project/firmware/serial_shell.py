#!/usr/bin/env python3
"""Prepare a pinned iOS arm64 Bash overlay for the System volume."""
from project import ROOT
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import runpy
import shutil
import struct
import subprocess

OUT=ROOT/'build/firmware/serial-shell'
SPEC=json.loads((ROOT/'tools/serial-bash-packages.json').read_text())


def prepare(output=OUT, launchd_cache=None):
    output = Path(output)
    output.mkdir(parents=True,exist_ok=True)
    # Preserve an already staged cache before rebuilding the overlay. iOS loads
    # its cached job definition, so changing only the XML does not change policy.
    overlay=output/'overlay'
    cache_source=launchd_cache or overlay/'System/Library/xpc/launchd.plist'
    cache_data=cache_source.read_bytes() if launchd_cache or cache_source.exists() else None
    unpacked=output/'rootfs'
    for item in SPEC['packages']:
        package=output/Path(item['Filename']).name
        if not package.exists():
            subprocess.run(['curl','-L','--fail','--silent','--show-error','https://apt.procurs.us/'+item['Filename'],'-o',str(package)],check=True)
        if hashlib.sha256(package.read_bytes()).hexdigest()!=item['SHA256']:
            raise ValueError('package SHA-256 mismatch: '+str(package))
        subprocess.run(['dpkg-deb','-x',str(package),str(unpacked)],check=True)
    if overlay.exists():shutil.rmtree(overlay)
    for relative in ['usr/bin/bash','usr/lib','usr/share/terminfo','usr/share/doc/bash','usr/share/doc/libiosexec1']:
        source=unpacked/relative;target=overlay/relative
        target.parent.mkdir(parents=True,exist_ok=True)
        if source.is_dir():
            shutil.copytree(source,target,symlinks=True,dirs_exist_ok=True)
        elif source.exists():
            shutil.copy2(source,target)
    # Keep shared runtime dylibs; optional Bash loadable modules/pkg-config files
    # are build extras rather than part of this serial service's dependency closure.
    for child in (overlay/'usr/lib').iterdir():
        if child.is_dir() and not child.is_symlink():shutil.rmtree(child)
    binary=(overlay/'usr/bin/bash').read_bytes()
    if struct.unpack_from('<III',binary)!=(0xfeedfacf,0x100000c,0):
        raise ValueError('Bash must be thin arm64/ALL Mach-O, not arm64e or a host ELF')
    load_report=subprocess.check_output(['llvm-objdump','--macho','--private-headers',str(overlay/'usr/bin/bash')],text=True)
    if 'platform ios' not in load_report:raise ValueError('Bash must target iOS')
    daemon=overlay/'System/Library/LaunchDaemons/org.mi6.debug.serial-bash.plist'
    daemon.parent.mkdir(parents=True,exist_ok=True)
    job={'Label':'org.mi6.debug.serial-bash',
         'ProgramArguments':['/usr/bin/bash','--noprofile','--rcfile','/private/etc/mi6-serial-bash.rc','-i'],
         'UserName':'root','LimitLoadToSessionType':'System','WorkingDirectory':'/',
         # This is an interactive diagnostic console. A background daemon's
         # task policy caps CPU priority independently of its BSD nice value.
         'ProcessType':'Interactive',
         'RunAtLoad':True,'KeepAlive':True,
         'StandardInPath':'/dev/console','StandardOutPath':'/dev/console','StandardErrorPath':'/dev/console',
         'EnvironmentVariables':{'TERM':'vt100','HOME':'/private/var/root','PATH':'/usr/bin:/bin:/usr/sbin:/sbin'}}
    daemon.write_bytes(plistlib.dumps(job,sort_keys=False))
    if cache_data is not None:
        from project.firmware.launchd import merge_cache_data
        merge_cache_data(cache_data,overlay)
    rc=overlay/'private/etc/mi6-serial-bash.rc';rc.parent.mkdir(parents=True,exist_ok=True)
    rc.write_text("export TERM=vt100\nexport PATH=/usr/bin:/bin:/usr/sbin:/sbin\nPS1='mi6-xnu:\\w# '\nprintf '\\nMi 6 XNU serial Bash (development console)\\n'\n")
    report={'overlay':str(overlay),'bash_sha256':hashlib.sha256(binary).hexdigest(),
            'macho':'arm64/ALL iOS','service':job,'packages':SPEC['packages'],
            'interactive_cache_prepared':cache_data is not None,
            'installed_to_device':False,'serial_shell_hardware_verified':False,
            'requires':['mounted System/Data','working /dev/console','dyld shared cache/Cryptex','development code-signing policy']}
    (output/'overlay.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=OUT)
    parser.add_argument('--launchd-cache',type=Path,
                        help='firmware launchd cache to merge and sign with the staged service')
    args=parser.parse_args()
    print(prepare(args.output,args.launchd_cache)['overlay'])
