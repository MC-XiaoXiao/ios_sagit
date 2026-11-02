#!/usr/bin/env python3
"""Pinned APFS writer bootstrap and iPad IPSW preparation (no device writes)."""
from project import ROOT
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request
import zipfile

from project.firmware import load_firmware_report

SPEC=json.loads((ROOT/'tools/apfs-tools.json').read_text())
OUT=ROOT/'build/cache/apfs'
GO=ROOT/'tools/prefix/libexec/go/bin/go'
IPSW=ROOT/'tools/prefix/libexec/ipsw/ipsw'


def run(*args,**kwargs):
    subprocess.run([str(a) for a in args],check=True,**kwargs)


def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()


def archive(url,expected,destination):
    path=ROOT/'tools/cache'/url.rsplit('/',1)[-1];path.parent.mkdir(parents=True,exist_ok=True)
    if not path.exists():
        temporary=path.with_suffix(path.suffix+'.download')
        urllib.request.urlretrieve(url,temporary);temporary.rename(path)
    if sha(path)!=expected:raise ValueError(f'archive hash mismatch: {path}')
    destination.mkdir(parents=True,exist_ok=True)
    with tarfile.open(path) as t:t.extractall(destination,filter='data')


def bootstrap():
    if not GO.exists():archive(SPEC['go']['url'],SPEC['go']['sha256'],GO.parents[2])
    if not IPSW.exists():archive(SPEC['ipsw']['url'],SPEC['ipsw']['archive_sha256'],IPSW.parent)
    if sha(IPSW)!=SPEC['ipsw']['binary_sha256']:raise ValueError('ipsw binary hash mismatch')
    for name,spec in SPEC['sources'].items():
        source=ROOT/'tools/src'/name
        if not source.exists():
            source.mkdir(parents=True);run('git','init',source)
            run('git','-C',source,'remote','add','origin',spec['url'])
            run('git','-C',source,'fetch','--depth','1','origin',spec['commit'])
            run('git','-C',source,'checkout','--detach','FETCH_HEAD')
        commit=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
        if commit!=spec['commit']:raise ValueError(f'wrong source commit: {source}')
    source=ROOT/'tools/src/go-apfs'
    # Later patches can change the context of an earlier patch. Compare the
    # complete expected stack using a private index before checking individual
    # patches; never reset a source checkout to make bootstrap succeed.
    with tempfile.TemporaryDirectory(prefix='apfs-patch-index-') as temporary:
        env=dict(os.environ,GIT_INDEX_FILE=str(Path(temporary)/'index'))
        run('git','-C',source,'read-tree','HEAD',env=env)
        for name in SPEC['patches']:
            run('git','-C',source,'apply','--cached',ROOT/'tools/patches'/name,env=env)
        paths=subprocess.check_output(['git','-C',str(source),'diff','--cached','--name-only','HEAD'],env=env,text=True).splitlines()
        complete=subprocess.run(['git','-C',str(source),'diff','--quiet','--',*paths],env=env).returncode==0
    if not complete:
        for name in SPEC['patches']:
            patch=ROOT/'tools/patches'/name
            if subprocess.run(['git','-C',str(source),'apply','--reverse','--check',str(patch)],capture_output=True).returncode:
                run('git','-C',source,'apply','--check',patch);run('git','-C',source,'apply',patch)
    checker_source=ROOT/'tools/src/apfsprogs'
    checker_patch=ROOT/'tools/patches/apfsprogs-ios18-native-metadata.patch'
    if subprocess.run(['git','-C',str(checker_source),'apply','--reverse','--check',str(checker_patch)],capture_output=True).returncode:
        run('git','-C',checker_source,'apply','--check',checker_patch)
        run('git','-C',checker_source,'apply',checker_patch)
    run('make','-C',checker_source/'apfsck')
    env=dict(os.environ,GOTOOLCHAIN='local')
    # Check COW/checkpoint writes before replacing the writer used by image flows.
    run(GO,'test','-count=1','.',cwd=ROOT/'tools/apfs-image',env=env)
    run(GO,'build','-o',OUT/'apfs-image','.',cwd=ROOT/'tools/apfs-image',env=env)
    # A read-only virtual image lets independent apfsck inspect COW updates
    # without materializing another full-sized firmware image.
    fuse=subprocess.check_output(['pkg-config','--cflags','--libs','fuse3'],text=True).split()
    run('cc','-O2','-Wall','-Wextra','-Werror',ROOT/'tools/apfs-image/cow-view/main.c',
        '-o',OUT/'apfs-cow-view',*fuse)


def prepare(ipsw,board,output=OUT):
    ipsw,output=Path(ipsw).resolve(),Path(output).resolve()
    destination=output/'firmware';destination.mkdir(parents=True,exist_ok=True)
    ipsw_hash=sha(ipsw)
    previous=output/'prepared-firmware.json'
    if previous.exists():
        report=load_firmware_report(previous)
        if report['ipsw_sha256'] != ipsw_hash or report['board'] != board:
            raise ValueError('output belongs to different firmware; choose a new output directory')
        for component in report['components'].values():
            if sha(Path(component['image'])) != component['sha256']:
                raise ValueError('prepared firmware changed: '+component['image'])
        return report
    with zipfile.ZipFile(ipsw) as z:
        manifest=plistlib.loads(z.read('BuildManifest.plist'))
        identities=[b for b in manifest['BuildIdentities'] if b['Info']['DeviceClass']==board and b['Info']['RestoreBehavior']=='Erase']
        if len(identities)!=1:raise ValueError('expected exactly one Erase BuildIdentity for '+board)
        identity=identities[0]
        components={}
        for key in ('OS','Cryptex1,SystemOS','Cryptex1,AppOS'):
            component=identity['Manifest'][key]
            entry=component['Info']['Path'];target=destination/Path(entry).name
            plain=target.with_suffix('') if target.suffix=='.aea' else target
            if plain.exists():raise ValueError('unverified existing component: '+str(plain))
            with tempfile.TemporaryDirectory(prefix='.extract-',dir=destination) as temporary:
                stage=Path(temporary);extracted=stage/target.name
                expected=component['Digest'];algorithm={20:'sha1',32:'sha256',48:'sha384'}.get(len(expected))
                if not algorithm:raise ValueError('unsupported BuildManifest digest: '+entry)
                h=hashlib.new(algorithm)
                with z.open(entry) as src,extracted.open('xb') as dst:
                    while chunk:=src.read(4*1024*1024):dst.write(chunk);h.update(chunk)
                # AEA BuildManifest digests identify the decrypted DMG, not
                # its encrypted archive member. Plain components hash directly.
                if target.suffix!='.aea' and h.digest()!=expected:
                    raise ValueError('BuildManifest digest mismatch: '+entry)
                if target.suffix=='.aea':run(IPSW,'fw','aea','--output',stage,extracted)
                candidates=list(stage.rglob(plain.name))
                if len(candidates)!=1:raise ValueError('expected one decrypted image for '+entry)
                if target.suffix=='.aea':
                    with candidates[0].open('rb') as decoded:
                        if hashlib.file_digest(decoded,algorithm).digest()!=expected:
                            raise ValueError('decrypted BuildManifest digest mismatch: '+entry)
                candidates[0].rename(plain)
            components[key]={'zip_entry':entry,'image':str(plain.relative_to(output)),'sha256':sha(plain)}
    report={'path_base':'report','ipsw':os.path.relpath(ipsw,output),'ipsw_sha256':ipsw_hash,'board':board,'product_version':manifest['ProductVersion'],
            'build':manifest['ProductBuildVersion'],'system_min_mib':identity['Info']['MinimumSystemPartition'],
            'components':components}
    previous.write_text(json.dumps(report,indent=2)+'\n')
    return load_firmware_report(previous)


def main():
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='command',required=True)
    sub.add_parser('bootstrap')
    a=sub.add_parser('firmware');a.add_argument('ipsw',type=Path);a.add_argument('--board',default='j171ap')
    a.add_argument('--output-directory',type=Path,default=OUT)
    a=sub.add_parser('image');a.add_argument('--firmware-report',type=Path,default=OUT/'prepared-firmware.json');a.add_argument('--output',type=Path,default=OUT/'ios.apfs');a.add_argument('--data-free-mib',type=int,default=2048)
    a.add_argument('--initialize-persona', action='store_true', help='seed absent iOS 18 persona metadata for reconstructed unencrypted Data')
    a.add_argument('--development-setup-complete', action='store_true', help='seed PurpleBuddy completion preferences in Data for this development port')
    args=p.parse_args();OUT.mkdir(parents=True,exist_ok=True)
    if args.command=='bootstrap':bootstrap()
    elif args.command=='firmware':prepare(args.ipsw.resolve(),args.board,args.output_directory)
    else:
        fw=load_firmware_report(args.firmware_report);c=fw['components']
        for item in c.values():
            if sha(ROOT/item['image'])!=item['sha256']:raise ValueError('prepared firmware changed')
        with tempfile.TemporaryDirectory(prefix='data-policy-') as temporary:
            options = []
            if args.initialize_persona or args.development_setup_complete:
                from project.firmware.data_policy import prepare as prepare_data_policy
                directory = Path(temporary) / 'data-policy'
                prepare_data_policy(Path(c['OS']['image']), directory,
                                    persona=args.initialize_persona,
                                    setup_complete=args.development_setup_complete)
                options = ['--data-overlay', directory / 'manifest.json']
            run(OUT/'apfs-image','--source',c['OS']['image'],'--cryptex-os',c['Cryptex1,SystemOS']['image'],
                '--cryptex-app',c['Cryptex1,AppOS']['image'],'--system-min-mib',fw['system_min_mib'],
                *options, '--data-free-mib',args.data_free_mib,'--output',args.output)



if __name__=='__main__':main()
