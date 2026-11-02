"""Read IPSW identity without extracting components or selecting an adaptation."""
import argparse
import json
from pathlib import Path
import plistlib
from zipfile import ZipFile


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, epilog='Input: an original IPSW. Output: identity JSON on stdout; no files are written.')
    parser.add_argument('ipsw', type=Path)
    args = parser.parse_args(argv)
    with ZipFile(args.ipsw) as archive:
        manifest = plistlib.loads(archive.read('BuildManifest.plist'))
    identities = []
    for identity in manifest.get('BuildIdentities', []):
        identities.append({key: identity.get('Info', {}).get(key) for key in ('DeviceClass', 'Variant', 'RestoreBehavior')})
    print(json.dumps({'version': manifest.get('ProductVersion'), 'build': manifest.get('ProductBuildVersion'),
                      'products': manifest.get('SupportedProductTypes', []), 'identities': identities}, indent=2))
    return 0
