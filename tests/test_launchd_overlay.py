import importlib.util
from pathlib import Path
import plistlib
import tempfile
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))

spec = importlib.util.spec_from_file_location('launchd_overlay', Path(__file__).resolve().parents[1]/'scripts/prepare-launchd-overlay.py')
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)


class LaunchdOverlayTests(unittest.TestCase):
    def test_explicit_jobs_merge_without_losing_firmware_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            p = Path(temporary); source = p/'source.plist'; overlay = p/'overlay'
            original = {'VersionNumber':7, 'LaunchDaemons':{'/original.plist':{'Label':'original'}},'AppExtensions':{'retained':True}}
            source.write_bytes(plistlib.dumps(original))
            job = overlay/'System/Library/LaunchDaemons/org.example.test.plist'; job.parent.mkdir(parents=True)
            job.write_bytes(plistlib.dumps({'Label':'org.example.test','ProgramArguments':['/usr/bin/test']}))
            paths = module.merge_cache(source, overlay)
            result = plistlib.loads((overlay/'System/Library/xpc/launchd.plist').read_bytes())
            cache_path = overlay/'System/Library/xpc/launchd.plist'
            self.assertEqual(len(module.verify_data(cache_path.read_bytes(), cache_path.with_suffix('.plist.sig').read_bytes())), 1)
            self.assertEqual(paths, ['/System/Library/LaunchDaemons/org.example.test.plist'])
            self.assertEqual(result['AppExtensions'], original['AppExtensions'])
            self.assertEqual(result['LaunchDaemons']['/original.plist'], original['LaunchDaemons']['/original.plist'])
            self.assertEqual(plistlib.loads(source.read_bytes()), original)


if __name__ == '__main__': unittest.main()
