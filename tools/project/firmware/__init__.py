"""Resolve firmware input paths independently of the caller's working directory."""
from project import ROOT
import json
from pathlib import Path

def load_firmware_report(path):
    path = Path(path).resolve()
    report = json.loads(path.read_text())
    base = path.parent if report.get('path_base') == 'report' else ROOT
    report['ipsw'] = str((base / report['ipsw']).resolve())
    for component in report.get('components', {}).values():
        component['image'] = str((base / component['image']).resolve())
    return report
