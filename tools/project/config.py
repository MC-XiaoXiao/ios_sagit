"""Board, SoC and Apple firmware selection. Paths are relative to their owner."""
import json
import subprocess
import tomllib
from pathlib import Path

from project import ROOT

CURRENT = ROOT / '.project.json'


def available() -> dict:
    return {
        'boards': [str(p.parent.relative_to(ROOT / 'boards')) for p in sorted((ROOT / 'boards').glob('*/*/board.toml'))],
        'firmware': [str(p.parent.relative_to(ROOT / 'firmware')) for p in sorted((ROOT / 'firmware').glob('*/*/firmware.toml'))],
    }


def resolve(board: str, firmware: str, profile: str, workspace: str | None = None) -> dict:
    board_dir = ROOT / 'boards' / board
    firmware_dir = ROOT / 'firmware' / firmware
    board_data = tomllib.loads((board_dir / 'board.toml').read_text())
    platform_dir = ROOT / 'platforms' / board_data['platform']
    platform_data = tomllib.loads((platform_dir / 'platform.toml').read_text())
    firmware_data = tomllib.loads((firmware_dir / 'firmware.toml').read_text())
    profile_data = board_data['profiles'][profile]
    if firmware_data['architecture'] != 'arm64':
        raise ValueError('this port requires arm64 firmware')
    out = Path(workspace).expanduser().resolve() if workspace else ROOT / 'build' / board / Path(firmware).name / profile
    return {
        'board': board, 'firmware': firmware, 'profile': profile,
        'workspace': str(out), 'board_dir': str(board_dir),
        'platform_dir': str(platform_dir), 'firmware_dir': str(firmware_dir),
        'device_tree': str(board_dir / board_data['device_tree']),
        'target': platform_data['kernel_target'],
        'board_config': board_data, 'platform_config': platform_data,
        'firmware_config': firmware_data, 'profile_config': profile_data,
    }


def select(board=None, firmware=None, profile=None, workspace=None) -> dict:
    saved = json.loads(CURRENT.read_text()) if CURRENT.exists() else {}
    board = board or saved.get('board')
    firmware = firmware or saved.get('firmware')
    if not board or not firmware:
        raise ValueError('select --board and --firmware, or run project.sh configure first')
    # A new board/firmware choice must not inherit another combination's output path.
    same = board == saved.get('board') and firmware == saved.get('firmware') and (profile is None or profile == saved.get('profile'))
    return resolve(board, firmware, profile or (saved.get('profile') if same else None) or 'platform',
                   workspace or (saved.get('workspace') if same else None))


def write_selection(config: dict) -> None:
    keys = ('board', 'firmware', 'profile', 'workspace')
    CURRENT.parent.mkdir(parents=True, exist_ok=True)
    CURRENT.write_text(json.dumps({key: config[key] for key in keys}, indent=2) + '\n')


def record(config: dict) -> Path:
    directory = Path(config['workspace'])
    for name in ('obj', 'logs', 'output', 'runs'):
        (directory / name).mkdir(parents=True, exist_ok=True)
    revision = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = bool(subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain'], text=True))
    path = directory / 'config.json'
    path.write_text(json.dumps({**config, 'source': {'revision': revision, 'dirty': dirty}}, indent=2) + '\n')
    return path


def filesystem_profile(config: dict) -> dict:
    firmware = config['firmware_config']
    path = Path(config['firmware_dir']) / firmware['filesystem']
    result = json.loads(path.read_text())
    result['patches'] += config['profile_config'].get('filesystem_patches', [])
    result['firmware'] = firmware['identity']
    result['name'] = '/'.join(config[key] for key in ('board', 'firmware', 'profile'))
    board = config['board_config']
    result['product'] = board['product']
    result['logical_screen_class'] = board['logical_screen_class']
    result['bridge_flags'] = config['profile_config']['bridge_flags'] + firmware.get('bridge_flags', [])
    result['bridge_flags'] += config['profile_config'].get('bridge_options', [])
    def portable(value):
        path = Path(value)
        return str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path)
    result['bridge_paths'] = {'board': portable(config['board_dir']), 'workspace': portable(config['workspace'])}
    result['boot_args'] = config['profile_config'].get('boot_args', [])
    if manifest := config['profile_config'].get('firmware_files'):
        result['file_overlay_manifest'] = portable(Path(config['board_dir']) / manifest)
        result['artifact_workspace'] = portable(config['workspace'])
    return result
