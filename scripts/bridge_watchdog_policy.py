"""Adapt the audited watchdogd service allowlist to the Qualcomm backend.

The native daemon, IOWatchdogUserClient rule and all privilege/entitlement
checks remain intact. Only the exact hardware service class literal changes.
Both class names occupy 21 bytes, so the compiled regex, pool offsets and
profile decision nodes keep their original layout.
"""
import hashlib
import importlib.util
from pathlib import Path

APPLE_CLASS = b'AppleARMWatchdogTimer'
PLATFORM_CLASS = b'QualcommWatchdogTimer'
SERVICE_RECORD = b'\x1a\0\x54' + APPLE_CLASS + b'\x0f\0\x0f\x0a' + bytes(4)
PROFILE_CONTEXT = (
    b'\x17\0/usr/libexec/watchdogd\0' + bytes(7)
    + b'\x19\0\x53IOWatchdogUserClient\x0f\0\x0f\x0a' + bytes(5)
)


def patch_watchdog_profile(pool):
    needle = PROFILE_CONTEXT + SERVICE_RECORD
    if len(APPLE_CLASS) != len(PLATFORM_CLASS) or pool.count(needle) != 1:
        raise ValueError('unsupported watchdogd service policy record')
    offset = pool.index(needle) + len(PROFILE_CONTEXT) + 3
    result = pool[:offset] + PLATFORM_CLASS + pool[offset + len(APPLE_CLASS):]
    return result, offset


def prepare_watchdog_service_policy(source, destination):
    spec = importlib.util.spec_from_file_location('watchdog_policy_verifier',
        Path(__file__).with_name('verify-kernelcache.py'))
    verifier = importlib.util.module_from_spec(spec); spec.loader.exec_module(verifier)
    data = source.read_bytes(); outer = verifier.Image(data)
    _, kexts = verifier.inventory.inspect(source)
    sandbox = [k for k in kexts if k['CFBundleIdentifier'] == 'com.apple.security.sandbox']
    backend = [k for k in kexts if k['CFBundleIdentifier'] == 'org.darwin.driver.QcomWatchdog']
    if len(sandbox) != 1 or len(backend) != 1:
        raise ValueError('expected native Sandbox and Qualcomm watchdog modules')
    classes = {p.get('IOClass') for p in backend[0].get('IOKitPersonalities', {}).values()}
    if PLATFORM_CLASS.decode() not in classes:
        raise ValueError('watchdog policy class does not match the installed backend')
    image = verifier.Image(data, outer.offset(sandbox[0]['_PrelinkExecutableLoadAddr'], 32), outer=outer)
    pools = [s for s in image.sections if s['segment'] == '__TEXT' and s['name'] == '__const']
    if len(pools) != 1 or pools[0]['size'] != 1818849:
        raise ValueError('unsupported Sandbox profile layout')
    pool = pools[0]; start = image.offset(pool['vm'], pool['size'])
    replacement, relative = patch_watchdog_profile(data[start:start + pool['size']])
    result = data[:start] + replacement + data[start + pool['size']:]
    destination.write_bytes(result)
    return {'policy': 'development-watchdog-service-class',
            'profile': 'watchdogd', 'from_class': APPLE_CLASS.decode(),
            'to_class': PLATFORM_CLASS.decode(), 'user_client': 'IOWatchdogUserClient',
            'source': str(source), 'source_sha256': hashlib.sha256(data).hexdigest(),
            'output_sha256': hashlib.sha256(result).hexdigest(),
            'literal_address': hex(pool['vm'] + relative), 'file_offset': start + relative,
            'before': APPLE_CLASS.hex(), 'after': PLATFORM_CLASS.hex()}
