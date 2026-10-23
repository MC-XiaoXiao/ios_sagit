#!/usr/bin/env python3
"""Adapt existing wallpaper metadata to the port's verified iPhone geometry.

Input is a small, read-only extraction of /Library/Wallpaper plist files.
Animation packages and collection identifiers stay in place: no asset copies,
new UUIDs, executable replacements, or changes to the system display scale.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import plistlib

STRIPE = '2939274C-F444-4AA8-806E-6E3FAFBA79B1'
IPADOS17 = '62879BFA-F700-4384-83E1-7AD1CC0D081E'
LAKE = 'B18891A2-4DB9-43A8-8C97-39C9CC7322AE'
BOKEH = 'D4894D14-DB0F-4D04-AB02-07C39C5E5466'


def prepare(source, output, product='iPhoneMi6,1', logical_class='360w-640h@3x~iphone'):
    if product != 'iPhoneMi6,1' or logical_class != '360w-640h@3x~iphone':
        raise ValueError('this profile is only verified for the Mi 6 geometry')
    root = source / 'Library/Wallpaper'
    collections = {}
    patches = {}
    wallpapers = {}
    changes = []
    for path in sorted((root / 'Collections').glob('*.wallpaperCollection/WallpaperCollection.plist')):
        data = plistlib.loads(path.read_bytes())
        identifier = data['identifier']
        if identifier in collections:
            raise ValueError('duplicate collection identifier: ' + identifier)
        collections[identifier] = data
        entries = []
        for wallpaper in sorted((path.parent / 'Wallpapers').glob('*.wallpaper/Wallpaper.plist')):
            original = plistlib.loads(wallpaper.read_bytes())
            entry = copy.deepcopy(original)
            if entry['logicalScreenClass'] != 'any':
                if entry['logicalScreenClass'] != '810w-1080h@2x~ipad':
                    raise ValueError('unexpected wallpaper geometry: ' + str(wallpaper))
                # Match the native generic collections: screen class is a
                # compatibility gate, not the CAML scaling transform. UIKit
                # utility clients can have no UIScreen even with valid BKS
                # geometry. Keep the exact product gate for this conversion.
                entry['logicalScreenClass'] = 'any'
                entry['productTypes'] = [product]
                patches[wallpaper.relative_to(source).as_posix()] = entry
                changes.append({'path': wallpaper.relative_to(source).as_posix(),
                    'before': original['logicalScreenClass'], 'after': 'any', 'productTypes':[product]})
            entries.append(entry['identifier'])
        if set(entries) != set(data['order']) or len(entries) != len(set(entries)):
            raise ValueError('collection contains missing/duplicate wallpapers: ' + identifier)
        wallpapers[identifier] = entries
        if '~ipad' in path.parent.name:
            data = copy.deepcopy(data)
            data['supportedDeviceClasses'] = ['iPhone']
            patches[path.relative_to(source).as_posix()] = data
    if set(collections) != {STRIPE, IPADOS17, LAKE, BOKEH}:
        raise ValueError('unexpected collection inventory; review before converting')
    selected = {'wallpaperIdentifier': 7570, 'collectionIdentifier': IPADOS17}
    if selected['wallpaperIdentifier'] not in wallpapers[IPADOS17]:
        raise ValueError('default wallpaper missing')
    patches['Library/Wallpaper/DefaultWallpapers~iphone.plist'] = {
        'default': selected, product: {'default': selected}}
    patches['Library/Wallpaper/Collections/Collections~iphone.plist'] = {
        'order': [IPADOS17, LAKE, STRIPE, BOKEH]}
    # Retire the tablet lookup profiles; the existing collections are reused
    # in place and retain their identifiers, including persisted references.
    patches['Library/Wallpaper/DefaultWallpapers~ipad.plist'] = {}
    patches['Library/Wallpaper/Collections/Collections~ipad.plist'] = {'order': []}
    patches['Library/Wallpaper/Wallpapers~ipad.plist'] = []
    if output.exists():
        raise ValueError('output must be a new directory')
    for relative, value in patches.items():
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(plistlib.dumps(value, fmt=plistlib.FMT_BINARY, sort_keys=False))
        target.chmod(0o644)
    report = {'schema': 1, 'product': product, 'logical_screen_class': logical_class,
        'wallpaper_compatibility_screen_class':'any', 'screen_selection_gated_by_product':True,
        'screen': {'pixels': [1080, 1920], 'points': [360, 640], 'scale': 3},
        'default': selected, 'collections': {k: wallpapers[k] for k in patches[
            'Library/Wallpaper/Collections/Collections~iphone.plist']['order']},
        'converted_wallpapers': changes, 'ipad_lookup_profiles_retired': True,
        'animation_geometry_modified': False, 'assets_copied': False,
        'files': {p: {'sha256': hashlib.sha256((output/p).read_bytes()).hexdigest(),
                      'bytes': (output/p).stat().st_size} for p in patches}}
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    report = prepare(args.source, args.output)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f"Prepared {len(report['files'])} metadata files; "
          f"{sum(map(len, report['collections'].values()))} wallpapers, no image copies")


if __name__ == '__main__':
    main()
