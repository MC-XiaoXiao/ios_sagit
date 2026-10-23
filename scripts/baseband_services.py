"""Read-only sagit service/source evidence. This never executes modem code."""
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def analyze(root=ROOT, output=None):
    root = Path(root)
    output = (Path(output) if output else root / 'docs/baseband/remote-services.json').resolve()
    if (output.is_relative_to((root / 'firmware').resolve()) or
            output.is_relative_to((root / 'docs/baseband/reference').resolve()) or
            output == (root / 'docs/baseband/firmware-sources.json').resolve()):
        raise ValueError('service evidence cannot overwrite its input sources')
    pin = json.loads((root / 'docs/baseband/firmware-sources.json').read_text())
    path = root / 'firmware/sagit-baseband/modem.mbn'
    raw = path.read_bytes()
    measured = {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}
    if any(measured[k] != pin['files']['modem.mbn'][k] for k in measured):
        raise ValueError('modem bytes differ from the pinned extraction')
    references = root / 'docs/baseband/reference'
    manifest = json.loads((references / 'sources.json').read_text())
    names = ('qcom_sysmon.c', 'qcom_glink_ssr.c', 'qcom_q6v5_mss.c', 'qcom_q6v5_pas.c', 'qcom_common.c')
    verified = {}
    for name in names:
        data = (references / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != manifest[name]['sha256']:
            raise ValueError('service source differs from its pinned reference: ' + name)
        verified[name] = manifest[name]
    pas = (references / 'qcom_q6v5_pas.c').read_text()
    descriptors = {}
    for kind, compatible, descriptor in (
            ('ADSP', 'qcom,msm8998-adsp-pas', 'msm8996_adsp_resource'),
            ('SLPI', 'qcom,msm8998-slpi-pas', 'msm8996_slpi_resource_init')):
        binding = re.search(r'\{\s*\.compatible\s*=\s*"' + compatible + r'"\s*,\s*\.data\s*=\s*&' + descriptor + r'\s*\}', pas)
        block = re.search(r'static const struct adsp_data ' + descriptor + r'\s*=\s*\{(.*?)\n\};', pas, re.S)
        if not binding or not block:
            raise ValueError('MSM8998 peer descriptor is missing: ' + kind)
        fields = {field: re.search(r'\.' + field + r'\s*=\s*"([^"]+)"', block[1])[1]
                  for field in ('ssr_name', 'sysmon_name')}
        fields['ssctl_instance'] = int(re.search(r'\.ssctl_id\s*=\s*(0x[0-9a-f]+|\d+)', block[1])[1], 0)
        fields.update({'compatible': compatible, 'descriptor': descriptor})
        descriptors[kind] = fields
    observations = []
    for text in ('IPCRTR', 'sys_mon', 'glink_ssr', 'ssr:shutdown', 'ssr:ack', 'lpass', 'dsps', 'wcnss'):
        needle = text.encode() + b'\0'
        start, offsets = 0, []
        while True:
            pos = raw.find(needle, start)
            if pos < 0: break
            offsets.append(pos); start = pos + 1
        observations.append({'literal': text, 'includes_trailing_nul': True, 'file_offsets': offsets})
    report = {'schema': 1, 'source': str(path.relative_to(root)), 'source_kind': pin['source_kind'],
              **measured, 'read_only': True, 'references': verified, 'msm8998_peer_descriptors': descriptors,
              'literal_observations': observations,
              'ssctl_reference': {'service': 43, 'modem_instance': 18, 'preferred_version': 2,
                                   'fallback_version': 1, 'fallback_instance': 0,
                                   'shutdown_request': 33, 'shutdown_ready_indication': 33, 'subsystem_event': 35},
              'scope': 'Static literal locations and pinned Linux source bindings; no disassembly-based protocol proof or device trace.',
              'runtime_channel_names_proven': False, 'runtime_qmi_version_proven': False,
              'foreign_stop_events_connected': False, 'hardware_tested': False}
    output.parent.mkdir(parents=True, exist_ok=True)
    pending = output.with_suffix(output.suffix + '.pending')
    pending.write_text(json.dumps(report, indent=2) + '\n'); pending.replace(output)
    return report


if __name__ == '__main__':
    result = analyze()
    print(json.dumps({'status': 'static-service-evidence', 'sha256': result['sha256'],
                      'peer_descriptors': result['msm8998_peer_descriptors'], 'hardware_tested': False}))
