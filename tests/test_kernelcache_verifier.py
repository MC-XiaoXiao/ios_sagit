import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location(
    "verifier", Path(__file__).parents[1] / "scripts/verify-kernelcache.py")
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)


def image(segments=1):
    data = bytearray(0x3000)
    struct.pack_into("<8I", data, 0, 0xfeedfacf, 0x100000c, 0, 2, segments, segments * 72, 1, 0)
    for n in range(segments):
        struct.pack_into("<II16sQQQQiiII", data, 32 + n * 72, 0x19, 72,
                         b"__TEXT" if n == 0 else b"__DATA", 0xfffffff007004000 + n * 4096,
                         4096, n * 4096, 4096, 5 if n == 0 else 3, 5 if n == 0 else 3, 0, 0)
    return data


class VerifierTests(unittest.TestCase):
    def test_vm_protections_do_not_share_large_pages(self):
        from types import SimpleNamespace
        segments = [dict(name='code', vm=0x4000, vmsize=0x1000, prot=5),
                    dict(name='data', vm=0x5000, vmsize=0x1000, prot=3)]
        kernel = SimpleNamespace(segments=segments)
        with self.assertRaisesRegex(ValueError, 'different protections share'):
            verifier.check_vm_protection_boundaries(kernel)
        segments[1]['vm'] = 0x8000
        verifier.check_vm_protection_boundaries(kernel)
        segments[1]['vm'] = 0x5000
        segments[1]['prot'] = 5
        verifier.check_vm_protection_boundaries(kernel)

    def test_command_count_and_size_are_checked(self):
        good = image()
        verifier.Image(good)
        struct.pack_into("<I", good, 16, 2)
        with self.assertRaisesRegex(ValueError, "outside sizeofcmds"):
            verifier.Image(good)

    def test_overlapping_segments_are_rejected(self):
        bad = image(2)
        struct.pack_into("<Q", bad, 32 + 72 + 24, 0xfffffff007004000)
        with self.assertRaisesRegex(ValueError, "overlap"):
            verifier.Image(bad)

    def test_invalid_ranges_and_permissions_are_rejected(self):
        bad = image()
        struct.pack_into("<Q", bad, 32 + 40, 0x4000)
        with self.assertRaisesRegex(ValueError, "outside cache"):
            verifier.Image(bad)
        bad = image()
        struct.pack_into("<ii", bad, 32 + 56, 7, 7)
        with self.assertRaisesRegex(ValueError, "writable executable"):
            verifier.Image(bad)




class KcgenWalkTests(unittest.TestCase):
    def fixture(self, second=128):
        from types import SimpleNamespace
        data = bytearray(512)
        for off in (0, second): struct.pack_into('<I', data, off, 0xfeedfacf)
        kernel = SimpleNamespace(
            sections=[dict(segment='__PRELINK_TEXT',name='__text',vm=0x1000,size=512)],
            segments=[dict(vm=0x1000,off=0,size=512)], offset=lambda addr,size=1: addr-0x1000)
        images = {str(off): SimpleNamespace(base=off, segments=[], header=(0,0,0,0,2,96,0,0),
                                           label=str(off), commands=[]) for off in (0,second)}
        return data,kernel,images

    def test_resource_header_stride_matches_xnu(self):
        verifier.check_kcgen_walk(*self.fixture())

    def test_interior_zero_gap_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'interior zero gap'):
            verifier.check_kcgen_walk(*self.fixture(192))

    def test_split_marker_is_required_for_executable_kexts(self):
        data,kernel,images = self.fixture()
        images['0'].segments = [dict(name='__TEXT',size=128)]
        with self.assertRaisesRegex(ValueError, 'split-segment marker'):
            verifier.check_kcgen_walk(data,kernel,images)

if __name__ == "__main__":
    unittest.main()
