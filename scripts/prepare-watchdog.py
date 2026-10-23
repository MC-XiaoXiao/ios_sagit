#!/usr/bin/env python3
"""Make the audited watchdogd honor its explicit no-Apple-PMU boot argument.

Only the diagnostics-variant guard is removed. Native argument/DT checks,
watchdog monitoring, and user-client permissions remain. Output is an ad-hoc
engineering executable requiring its actual cdhash in the boot trust cache.
"""
import argparse
import hashlib
import json
from pathlib import Path
from macho_development_sign import resign_macho, signature_location, signature_blobs

SOURCE_SHA256 = '711f9a156411d8a51f65070284127de9e820f8af6cebe61bda31dba0bd1919d5'
GATE = 0xac98
FUNCTION = bytes.fromhex('f44fbea9fd7b01a9fd430091000000f000c41c91440600944001003417f3ff97010000f02104339102808052b9060094e00000b4c80000902900805209612839fd7b41a9f44fc2a8c0035fd6')


def patch_no_pmu_gate(source):
    if hashlib.sha256(source).hexdigest() != SOURCE_SHA256 or source[0xac80:0xaccc] != FUNCTION:
        raise ValueError('unsupported watchdogd source or PMU configuration function')
    result = bytearray(source); result[GATE:GATE+4] = bytes.fromhex('1f2003d5')
    return bytes(result)


def prepare(source, destination):
    original = source.read_bytes(); patched = patch_no_pmu_gate(original)
    result, report = resign_macho(patched, 'org.mi6.development.watchdogd')
    off, _, _, _ = signature_location(original)
    old, _ = signature_blobs(original[off:]); new, _ = signature_blobs(result[off:])
    if any(old[k] != new[k] for k in (2, 5, 7)):
        raise ValueError('watchdogd requirements or entitlements changed')
    if original[:GATE] != result[:GATE] or original[GATE+4:off] != result[GATE+4:off]:
        raise ValueError('watchdogd changes exceed the reviewed guard')
    destination.parent.mkdir(parents=True, exist_ok=True); destination.write_bytes(result)
    report.update(source_sha256=hashlib.sha256(original).hexdigest(),
        output_sha256=hashlib.sha256(result).hexdigest(), file_bytes=len(result),
        instruction_offset=GATE, instruction_address=hex(0x100000000+GATE),
        before=original[GATE:GATE+4].hex(), after=result[GATE:GATE+4].hex(),
        requires_boot_arg='-wdt_no_pmu', hardware_verified=False)
    destination.with_suffix('.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    if a.input.resolve() == a.output.resolve(): p.error('input must differ from output')
    print(json.dumps(prepare(a.input, a.output), indent=2))
