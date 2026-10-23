import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from bridge_watchdog_policy import (APPLE_CLASS, PLATFORM_CLASS, PROFILE_CONTEXT,
    SERVICE_RECORD, patch_watchdog_profile)


class WatchdogPolicyTests(unittest.TestCase):
    def test_only_exact_service_literal_changes(self):
        pool = b'prefix' + PROFILE_CONTEXT + SERVICE_RECORD + b'suffix'
        result, offset = patch_watchdog_profile(pool)
        self.assertEqual(len(result), len(pool))
        self.assertEqual(result[:offset], pool[:offset])
        self.assertEqual(result[offset:offset+len(PLATFORM_CLASS)], PLATFORM_CLASS)
        self.assertEqual(result[offset+len(APPLE_CLASS):], pool[offset+len(APPLE_CLASS):])
        self.assertIn(b'IOWatchdogUserClient', result)

    def test_unknown_or_ambiguous_profiles_are_rejected(self):
        record = PROFILE_CONTEXT + SERVICE_RECORD
        for pool in (b'', SERVICE_RECORD, record + record,
                     record.replace(b'IOWatchdogUserClient', b'XXWatchdogUserClient'),
                     record.replace(b'\x1a\0\x54', b'\x1a\0\x55')):
            with self.assertRaises(ValueError):
                patch_watchdog_profile(pool)


if __name__ == '__main__': unittest.main()
