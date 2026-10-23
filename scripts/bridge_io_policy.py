"""Select XNU's existing unthrottled I/O mode for development boots."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import subprocess


def prepare_unthrottled_io(source, destination):
    spec = importlib.util.spec_from_file_location(
        'io_policy_verifier', Path(__file__).with_name('verify-kernelcache.py'))
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    data = source.read_bytes()
    image = verifier.Image(data)
    symbols = subprocess.check_output(
        ['llvm-nm', '--defined-only', '--format=posix', str(source)], text=True)
    matches = [line.split() for line in symbols.splitlines()
               if line.split()[0] == '_lowpri_throttle_enabled']
    if len(matches) != 1 or matches[0][1] not in ('D', 'd'):
        raise ValueError('expected one writable XNU lowpri_throttle_enabled symbol')
    address = int(matches[0][2], 16)
    section = next((s for s in image.sections
                    if s['vm'] <= address and address + 4 <= s['vm'] + s['size']), None)
    if not section or section['name'] != '__data' or not image.owner(address, 4)['prot'] & 2:
        raise ValueError('I/O throttle setting is not in writable initialized data')
    offset = image.offset(address, 4)
    if struct.unpack_from('<I', data, offset)[0] != 1:
        raise ValueError('expected enabled native I/O throttle setting')
    result = data[:offset] + bytes(4) + data[offset + 4:]
    destination.write_bytes(result)
    return {
        'policy': 'development-unthrottled-io',
        'symbol': '_lowpri_throttle_enabled', 'address': hex(address),
        'file_offset': offset, 'before': 1, 'after': 0,
        'runtime_sysctl': 'debug.lowpri_throttle_enabled',
        'source': str(source), 'source_sha256': hashlib.sha256(data).hexdigest(),
        'output_sha256': hashlib.sha256(result).hexdigest(),
    }
