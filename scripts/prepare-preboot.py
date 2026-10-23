#!/usr/bin/env python3
"""Stage Cryptex metadata/references; personalized same-volume graft is separate."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import zipfile

from bridge_trustcache import raw_module, module_info


def prepare(report_path, output):
    report = json.loads(report_path.read_text())
    staged = {}
    with zipfile.ZipFile(report['ipsw']) as archive:
        manifest = plistlib.loads(archive.read('BuildManifest.plist'))
        if (manifest['ProductVersion'], manifest['ProductBuildVersion']) != (report['product_version'], report['build']):
            raise ValueError('firmware identity differs from prepared filesystem')
        identity = next(b for b in manifest['BuildIdentities']
                        if b['Info']['DeviceClass'] == report['board'] and b['Info']['RestoreBehavior'] == 'Erase')
        # These are the OS/App object names consumed by libignition, paired
        # with BuildManifest component types, independent of device/build IDs.
        for kind, component_prefix in (('os', 'System'), ('app', 'App')):
            for suffix, component_suffix in (('root_hash', 'Volume'), ('trustcache', 'TrustCache')):
                component = identity['Manifest']['Cryptex1,' + component_prefix + component_suffix]
                path = component['Info']['Path']
                if archive.getinfo(path).file_size > 16 * 1024 * 1024:
                    raise ValueError('unexpectedly large Cryptex metadata component')
                data = archive.read(path)
                digest = component['Digest']
                algorithm = {20: 'sha1', 32: 'sha256', 48: 'sha384'}.get(len(digest))
                if not algorithm or hashlib.new(algorithm, data).digest() != digest:
                    raise ValueError('BuildManifest digest mismatch: ' + path)
                record = {'zip_entry': path, 'manifest_digest': digest.hex(),
                          'sha256': hashlib.sha256(data).hexdigest()}
                if suffix == 'trustcache':
                    record['module'] = module_info(raw_module(data))
                staged[f'{kind}.dmg.{suffix}'] = (data, record)
    # Validate everything before creating an overlay; never replace an existing
    # staging tree that may already belong to an installed delta.
    output.mkdir(parents=True, exist_ok=False)
    objects = output/'cryptex1/current'
    objects.mkdir(parents=True)
    records = {}
    for name, (data, record) in staged.items():
        (objects/name).write_bytes(data)
        records[name] = record
    for kind, image in (('os', 'SystemOS'), ('app', 'AppOS')):
        target = '/usr/standalone/firmware/Cryptex1/' + image + '.dmg'
        (objects/(kind + '.dmg')).symlink_to(target)
        (output/'Cryptexes'/('OS' if kind == 'os' else 'App')).mkdir(parents=True)
        records[kind + '.dmg'] = {'symlink': target}
    result = {'board': report['board'], 'build': report['build'], 'objects': records,
              'personalized_manifest_provided': False, 'full_image_created': False,
              'ready_for_native_graft': False, 'requires_same_volume_dmg_for_graft': True}
    output.with_name(output.name + '.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware-report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.firmware_report, args.output)
