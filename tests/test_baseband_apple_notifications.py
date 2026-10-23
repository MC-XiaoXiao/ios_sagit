"""Verify measured iPad functions/branches and strict function-start parsing."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from baseband_apple_notifications import analyze, starts_from_uleb


class AppleNotifications(unittest.TestCase):
    def test_function_starts_bounds(self):
        self.assertEqual(starts_from_uleb(b'\4\10\0\0', 0x1000, 0x100), [0x1004, 0x100c])
        for raw in (b'', b'\4', b'\x80' * 12, b'\x80' * 9 + b'\x02', b'\1\0', b'\0\4', b'\x80\x02\0'):
            with self.subTest(raw=raw):
                with self.assertRaises(ValueError): starts_from_uleb(raw, 0x1000, 0x100)

    def test_report_is_reproducible_from_the_ipad_image(self):
        saved = json.loads((ROOT / 'docs/baseband/ipad-apple-notifications.json').read_text())
        with tempfile.TemporaryDirectory() as directory:
            actual = analyze(ROOT, Path(directory) / 'notifications.json')
        self.assertEqual(actual, saved)
        self.assertEqual(saved['build'], '22H374')
        self.assertEqual(saved['interest']['text'], 'IOGeneralInterest')
        self.assertFalse(saved['apple_driver_notification_abi_implemented'])
        self.assertFalse(saved['hardware_tested'])
        self.assertEqual({m['message_type'] for m in saved['messages']}, {hex(0xe3ff8000 + i) for i in range(18)})
        labels = {m['message_type']: m['diagnostic_label']['text'] for m in saved['messages']}
        self.assertIn('wake-asserted', labels['0xe3ff8007'])
        self.assertIn('coredump ready', labels['0xe3ff800d'])
        self.assertEqual(set(saved['reset_internal_special_cases']), {'0xe3ff8007', '0xe3ff800d'})
        for row in saved['registrations'].values():
            for field in ('wrapper', 'implementation', 'callback', 'callback_invoke'):
                ev = row[field]; raw = b''.join(bytes.fromhex(i['bytes']) for i in ev['instructions'])
                self.assertEqual(len(raw), ev['bytes'])
                self.assertEqual(hashlib.sha256(raw).hexdigest(), ev['sha256'])
            self.assertFalse(row['message_argument_payload_layout_proven'])
        # The complete LC_FUNCTION_STARTS range includes the success block
        # after the helper's first return, unlike a first-ret linear scan.
        ins = saved['registration_helper']['instructions']
        returns = [i for i, item in enumerate(ins) if item['mnemonic'] == 'ret']
        self.assertGreaterEqual(len(returns), 1)
        self.assertLess(returns[0], len(ins) - 1)

    def test_output_cannot_replace_evidence_or_firmware(self):
        source = ROOT / 'docs/baseband/ipad-runtime-libraries.json'
        before = hashlib.sha256(source.read_bytes()).hexdigest()
        for output in (source, ROOT / 'firmware/notification.json', ROOT / 'docs/baseband/reference/bad.json'):
            with self.assertRaisesRegex(ValueError, 'input sources'): analyze(ROOT, output)
        self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), before)


if __name__ == '__main__':
    unittest.main()
