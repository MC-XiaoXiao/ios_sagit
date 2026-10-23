#!/usr/bin/env python3
"""Add explicitly staged LaunchDaemons to the firmware's launchd cache."""
import argparse
import plistlib
from pathlib import Path
from detached_codesign import sign_data, verify_data


def merge_cache(source, overlay):
    return merge_cache_data(source.read_bytes(), overlay)


def merge_cache_data(data, overlay):
    cache = plistlib.loads(data)
    if not isinstance(cache, dict) or not isinstance(cache.get('LaunchDaemons'), dict):
        raise ValueError('firmware cache has no LaunchDaemons dictionary')
    paths = []
    for job_path in sorted((overlay/'System/Library/LaunchDaemons').glob('*.plist')):
        job = plistlib.loads(job_path.read_bytes())
        if not isinstance(job, dict) or not isinstance(job.get('Label'), str) or not job['Label']:
            raise ValueError('staged job has no Label: '+str(job_path))
        path = '/'+job_path.relative_to(overlay).as_posix()
        cache['LaunchDaemons'][path] = job; paths.append(path)
    target = overlay/'System/Library/xpc/launchd.plist'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(plistlib.dumps(cache, fmt=plistlib.FMT_BINARY, sort_keys=False))
    signature = sign_data(target.read_bytes(), 'org.mi6.development.launchd-cache')
    verify_data(target.read_bytes(), signature)
    target.with_suffix(target.suffix + '.sig').write_bytes(signature)
    return paths


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--overlay', type=Path, required=True)
    args = parser.parse_args()
    for path in merge_cache(args.cache, args.overlay): print(path)
