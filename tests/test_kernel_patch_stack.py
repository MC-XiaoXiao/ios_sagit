import difflib
import hashlib
import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from kernel_patch_stack import PatchStackError, ensure_kernel_patch_stack, order_kernel_patches


class KernelPatchStackTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "kernel"
        self.source.mkdir()
        self.archive = self.root / "upstream.tar.gz"
        self.original = b"first\nsecond\nthird\n"
        with tarfile.open(self.archive, "w:gz") as bundle:
            member = tarfile.TarInfo("xnu/source.c")
            member.size = len(self.original)
            member.mode = 0o644
            bundle.addfile(member, io.BytesIO(self.original))
        self.checksum = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        (self.source / "source.c").write_bytes(self.original)
        self.first = b"first\nchanged once\nthird\n"
        self.final = b"first\nchanged twice\nthird\n"
        self.patches = [
            self.patch("01.patch", self.original, self.first),
            self.patch("02.patch", self.first, self.final),
        ]

    def patch(self, name, old, new, filename="source.c"):
        patch = self.root / name
        patch.write_text("".join(difflib.unified_diff(
            old.decode().splitlines(True), new.decode().splitlines(True),
            fromfile="a/" + filename if old else "/dev/null",
            tofile="b/" + filename if new else "/dev/null")))
        return patch

    def apply(self):
        return ensure_kernel_patch_stack(self.source, self.patches, self.archive, self.checksum)

    def test_overlapping_stack_and_idempotence(self):
        result = self.apply()
        self.assertEqual(result["matched_prefix"], 0)
        self.assertEqual((self.source / "source.c").read_bytes(), self.final)
        result = self.apply()
        self.assertEqual(result["matched_prefix"], 2)
        self.assertEqual(result["updated_files"], [])

    def test_existing_complete_prefix(self):
        (self.source / "source.c").write_bytes(self.first)
        self.assertEqual(self.apply()["matched_prefix"], 1)
        self.assertEqual((self.source / "source.c").read_bytes(), self.final)

    def test_unexpected_edit_preserved(self):
        local = self.original + b"user change\n"
        (self.source / "source.c").write_bytes(local)
        with self.assertRaisesRegex(PatchStackError, "local changes preserved"):
            self.apply()
        self.assertEqual((self.source / "source.c").read_bytes(), local)

    def test_late_patch_failure_leaves_all_inputs_untouched(self):
        self.patches.append(self.patch("03.patch", b"does not exist\n", b"bad\n"))
        with self.assertRaisesRegex(PatchStackError, "failed in isolation"):
            self.apply()
        self.assertEqual((self.source / "source.c").read_bytes(), self.original)

    def test_created_and_deleted_file(self):
        self.patches.append(self.patch("03.patch", b"", b"header\n", "new.h"))
        self.patches.append(self.patch("04.patch", self.final, b""))
        self.apply()
        self.assertFalse((self.source / "source.c").exists())
        self.assertEqual((self.source / "new.h").read_bytes(), b"header\n")
        self.assertEqual(self.apply()["updated_files"], [])

    def test_checksum_and_symlink_refused(self):
        self.checksum = "0" * 64
        with self.assertRaisesRegex(PatchStackError, "checksum mismatch"):
            self.apply()
        self.checksum = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        target = self.root / "external.c"
        target.write_bytes(self.original)
        (self.source / "source.c").unlink()
        (self.source / "source.c").symlink_to(target)
        with self.assertRaisesRegex(PatchStackError, "symlink"):
            self.apply()
        self.assertEqual(target.read_bytes(), self.original)

    def test_mixed_prefix_refused(self):
        # Each file is a known version, but their combined state is not a
        # complete prefix. Never guess that a partly interrupted patch is safe.
        self.patches.append(self.patch("03.patch", b"", b"header\n", "new.h"))
        (self.source / "new.h").write_bytes(b"header\n")
        with self.assertRaisesRegex(PatchStackError, "complete patch-stack prefix"):
            self.apply()
        self.assertEqual((self.source / "source.c").read_bytes(), self.original)

    def test_old_patch_dependency_precedes_its_consumer(self):
        trace = Path("zzzzzzzzzzzzzzzz-development-iokit-open-trace.patch")
        consumer = Path("xnu-iokit-development.patch")
        other = Path("a.patch")
        self.assertEqual(order_kernel_patches([trace, other, consumer]),
                         [other, trace, consumer])
        with self.assertRaisesRegex(PatchStackError, "requires"):
            order_kernel_patches([consumer])


if __name__ == "__main__":
    unittest.main()
