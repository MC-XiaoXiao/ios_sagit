import importlib.util
from pathlib import Path
import plistlib
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('wallpaper_overlay', Path(__file__).resolve().parents[1]/'scripts/prepare-wallpaper-overlay.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class WallpaperOverlayTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root/'source'
        self.output = self.root/'overlay'
        self.wallpapers = []
        for name, identifier, ids in [('Stripe',module.STRIPE,[7390]),('Bokeh',module.BOKEH,[5010]),
                                      ('Lake~ipad',module.LAKE,[7560]),('iPadOS_17~ipad',module.IPADOS17,[7570])]:
            collection = self.source/'Library/Wallpaper/Collections'/(name+'.wallpaperCollection')
            collection.mkdir(parents=True)
            (collection/'WallpaperCollection.plist').write_bytes(plistlib.dumps({'identifier':identifier,'order':ids}))
            for wid in ids:
                path = collection/'Wallpapers'/f'{wid}.wallpaper/Wallpaper.plist'
                path.parent.mkdir(parents=True)
                path.write_bytes(plistlib.dumps({'identifier':wid,'logicalScreenClass':
                    '810w-1080h@2x~ipad' if '~ipad' in name else 'any',
                    'assets':{'lockAndHome':{'default':{'backgroundAnimationFileName':'original.ca'}}},
                    'adjustmentTraits':{'adjustmentTraitOffset':{'screenScaleModeZoomed':{'portrait':{'x':7,'y':9}}}}}))
                self.wallpapers.append(path)

    def test_preserves_animation_references_and_display_zoom_adjustments(self):
        old = plistlib.loads(self.wallpapers[-1].read_bytes())
        report = module.prepare(self.source,self.output)
        new = plistlib.loads((self.output/self.wallpapers[-1].relative_to(self.source)).read_bytes())
        self.assertEqual(old['assets'],new['assets'])
        self.assertEqual(old['adjustmentTraits'],new['adjustmentTraits'])
        self.assertEqual(new['logicalScreenClass'],'any')
        self.assertEqual(new['productTypes'],['iPhoneMi6,1'])
        self.assertEqual(report['logical_screen_class'],'360w-640h@3x~iphone')
        self.assertTrue(report['screen_selection_gated_by_product'])
        self.assertFalse(report['animation_geometry_modified'])
        self.assertFalse((self.output/self.wallpapers[0].relative_to(self.source)).exists())

    def test_missing_default_asset_refuses_output(self):
        self.wallpapers[-1].unlink()
        with self.assertRaisesRegex(ValueError,'missing/duplicate'):
            module.prepare(self.source,self.output)
        self.assertFalse(self.output.exists())

    def test_duplicate_wallpaper_identity_refuses_output(self):
        original=self.wallpapers[-1]
        extra=original.parent.parent/'duplicate.wallpaper/Wallpaper.plist'
        extra.parent.mkdir();extra.write_bytes(original.read_bytes())
        with self.assertRaisesRegex(ValueError,'missing/duplicate'):
            module.prepare(self.source,self.output)
        self.assertFalse(self.output.exists())

    def test_unreviewed_geometry_refuses_output(self):
        path=self.wallpapers[-1];data=plistlib.loads(path.read_bytes())
        data['logicalScreenClass']='1024w-1366h@2x~ipad'
        path.write_bytes(plistlib.dumps(data))
        with self.assertRaisesRegex(ValueError,'unexpected wallpaper geometry'):
            module.prepare(self.source,self.output)
        self.assertFalse(self.output.exists())

    def test_non_mi6_profile_refuses_output(self):
        with self.assertRaisesRegex(ValueError,'only verified'):
            module.prepare(self.source,self.output,product='iPhone15,2')
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
