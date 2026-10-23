#!/usr/bin/env python3
"""Inventory the local iOS 18 haptics providers. Offline, no device access."""
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    native = ROOT/'build/haptics/native'
    native.mkdir(parents=True, exist_ok=True)
    archive = ROOT/'firmware/iPhone14,6_18.6.2_22G100_Restore.ipsw'
    packed = native/'kernelcache.release.iphone14c'
    with zipfile.ZipFile(archive) as z:
        manifest = plistlib.loads(z.read('BuildManifest.plist'))
        data = z.read(packed.name)
    if not packed.exists() or packed.read_bytes() != data:
        packed.write_bytes(data)
    subprocess.run([str(ROOT/'tools/prefix/bin/ipsw'), 'kernel', 'dec', str(packed),
                    '-o', str(native/'kernelcache.macho')], check=True)
    phone = native/'kernelcache.macho/kernelcache.release.iphone14c.decompressed'
    spec = importlib.util.spec_from_file_location('haptics_kc', ROOT/'scripts/analyze-kernelcache.py')
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    report = {'schema': 1, 'hardware_tested': False, 'installed': False,
              'comparison_firmware': {'path': str(archive.relative_to(ROOT)),
                  'ProductVersion': manifest['ProductVersion'], 'ProductBuildVersion': manifest['ProductBuildVersion'],
                  'SupportedProductTypes': manifest['SupportedProductTypes'],
                  'packed_kernel_sha256': hashlib.sha256(data).hexdigest()}, 'providers': []}
    for role, path in [('runtime-arm64', ROOT/'build/ios18-kext-analysis/kernelcache.release.ipad7c.macho'),
                       ('comparison-arm64e', phone)]:
        meta, kexts = reader.inspect(path)
        selected = [{k:v for k,v in item.items() if k in
                     ('CFBundleIdentifier', 'CFBundleVersion', 'DTSDKName', 'DTPlatformBuild',
                      'IOKitPersonalities', 'OSBundleLibraries')}
                    for item in kexts if 'haptic' in item.get('CFBundleIdentifier', '').lower()]
        report['providers'].append({'role':role, 'path':str(path.relative_to(ROOT)),
                                   'metadata':meta, 'haptics_kexts':selected})
    report['findings'] = [
        'The runtime arm64 iPad provider has no haptics hardware Kext.',
        'The iPhone comparison is arm64e; it is evidence, not a loadable arm64 dependency.',
        'AppleAOPHaptics matches AppleAOPAudioDeviceNode / aop-audio-haptic and uses AppleAOPAudioDeviceProvider.',
        'function-haptics_session notifies session start/stop; it is not a duration/amplitude playback API.',
        'AppleHapticsSupportLEAP has a com.apple.ahs.userclientaccess-gated client and firmware/calibration support.',
        'PMI8998Haptics exposes a project diagnostic ABI; UIKit/Core Haptics/mediaserverd bridging is not implemented.'
    ]
    out = ROOT/'docs/haptics/ios18-model.json'
    out.write_text(json.dumps(report, indent=2)+'\n')
    print(out.relative_to(ROOT))


if __name__ == '__main__':
    main()
