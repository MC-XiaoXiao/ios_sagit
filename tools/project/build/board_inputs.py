"""Stage explicitly selected board firmware with pinned hashes."""
import hashlib
import json
from pathlib import Path
import shutil


def check(selected):
    board = Path(selected['board_dir'])
    settings = selected['board_config']
    if settings.get('gpu_firmware'):
        source = (board / settings['gpu_firmware']).resolve()
        spec = json.loads((board / settings['gpu_firmware_manifest']).read_text())
        if spec.get('schema') != 1:
            raise ValueError('unsupported GPU firmware manifest')
        for name, expected in spec['files'].items():
            if Path(name).name != name or name in ('.', '..'):
                raise ValueError('GPU firmware requires plain file names')
            data = (source / name).read_bytes()
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError('GPU firmware differs from the board manifest: ' + name)
    if settings.get('device_firmware'):
        from project.firmware.input import FIRMWARE_URL, FIRMWARE_SHA256
        archive = (board / settings['device_firmware']).resolve() / FIRMWARE_URL.rsplit('/', 1)[-1]
        with archive.open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != FIRMWARE_SHA256:
                raise ValueError('board input firmware archive hash mismatch')


def prepare(selected):
    check(selected)
    board = Path(selected['board_dir'])
    settings = selected['board_config']
    if settings.get('gpu_firmware'):
        source = (board / settings['gpu_firmware']).resolve()
        spec = json.loads((board / settings['gpu_firmware_manifest']).read_text())
        destination = Path(selected['workspace']) / 'obj/device-firmware/gpu'
        destination.mkdir(parents=True, exist_ok=True)
        for name in spec['files']:
            shutil.copy2(source / name, destination / name)
    if settings.get('device_firmware'):
        from project.firmware.input import prepare_input_firmware, FIRMWARE_URL
        archive = (board / settings['device_firmware']).resolve() / FIRMWARE_URL.rsplit('/', 1)[-1]
        prepare_input_firmware(Path(selected['workspace']) / 'obj/device-firmware/input', archive)
