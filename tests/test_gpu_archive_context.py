"""Focused cache identity tests; never build or write production outputs."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('gpu_archive_context', ROOT / 'scripts/gpu_archive_context.py')
CONTEXT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONTEXT)


class ArchiveContextTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.files = {}
        cls.generated = ROOT / 'build/gpu-tile-origin-1.52/final'
        cls.runtime = cls.generated / 'QcomGPUCompiler.dylib'
        if not cls.runtime.is_file():
            raise unittest.SkipTest('Frozen 1.52 compiler fixture is unavailable')
        cls.baseline = cls.context()

    @classmethod
    def read(cls, path):
        if path not in cls.files:
            cls.files[path] = path.read_bytes()
        return cls.files[path]

    @classmethod
    def context(cls, changes=None, version='fixture clang'):
        changes = changes or {}
        return CONTEXT.build_context(ROOT, cls.runtime, cls.generated, version,
            lambda p: changes[p] if p in changes else cls.read(p))['context']

    def test_render_version_catalog_and_test_edits_reuse_cache(self):
        adapter = ROOT / CONTEXT.ADAPTER
        old = self.read(adapter)
        needle = b'_gmem=NO;'
        self.assertEqual(old.count(needle), 1)
        changes = {adapter: old.replace(needle, b'_gmem=_gmemSupported;')}
        for name in ('drivers/gpu/QcomMTL/Info.plist',
                     'tools/gpu-compiler/air-nir-prototype/Differential.cpp',
                     'tools/gpu-compiler/SystemShaders.json',
                     'build/drivers/generated/GPU/GPUSystemShaders.h'):
            changes[ROOT / name] = b'unrelated version/catalog/diagnostic change'
        self.assertEqual(self.baseline, self.context(changes))

    def test_build_line_numbers_and_reporting_do_not_invalidate(self):
        path = ROOT / 'scripts/toolchain.py'
        old = self.read(path)
        self.assertIn(b"'status':'cross-linked-offline'", old)
        new = b'\n\n' + old.replace(b"'status':'cross-linked-offline'", b"'status':'report-only-change'")
        self.assertEqual(self.baseline, self.context({path: new}))

    def test_real_compile_flags_invalidate(self):
        path = ROOT / 'scripts/toolchain.py'
        old = self.read(path)
        needle = b"'-Oz' if name == 'displayctl' else '-O2'"
        self.assertEqual(old.count(needle), 1)
        self.assertNotEqual(self.baseline, self.context({path: old.replace(needle, needle.replace(b'-O2', b'-O3'))}))
        self.assertNotEqual(self.baseline, self.context(version='changed clang'))

    def test_compiler_frontend_and_archive_abi_invalidate(self):
        names = ('tools/gpu-compiler/Compiler.cpp', 'tools/gpu-compiler/AIRSpecialization.h',
                 'drivers/include/GPUShaderInterface.h', 'drivers/include/GPUJITProtocol.h',
                 'drivers/gpu/QcomMTL/MetalArchive.h')
        for name in names:
            with self.subTest(name=name):
                path = ROOT / name
                self.assertNotEqual(self.baseline, self.context({path: self.read(path) + b'\n// code/ABI revision\n'}))
        self.assertNotEqual(self.baseline, self.context({self.runtime: self.read(self.runtime) + b'changed binary'}))

    def test_frontend_call_site_options_invalidate(self):
        path = ROOT / 'scripts/toolchain.py'
        old = self.read(path)
        needle = b"exception_sources=['MSL.cpp','AIRRuntime.cpp','Compiler.cpp']"
        self.assertEqual(old.count(needle), 1)
        self.assertNotEqual(self.baseline, self.context({path: old.replace(needle, b"exception_sources=['MSL.cpp']")}))

    def test_registry_and_adapter_constant_semantics_invalidate(self):
        path = self.generated / 'GPUPrograms.h'
        old = self.read(path)
        needle = b'0x28f66944d0b9aab4ULL'
        self.assertIn(needle, old)
        self.assertNotEqual(self.baseline, self.context({path: old.replace(needle, b'0x28f66944d0b9aab5ULL')}))
        path = ROOT / CONTEXT.ADAPTER
        old = self.read(path)
        needle = b'archiveWord(bytes,maxThreads);'
        self.assertIn(needle, old)
        self.assertNotEqual(self.baseline, self.context({path: old.replace(needle, b'archiveWord(bytes,maxThreads+1);')}))

    def test_new_frontend_include_is_followed(self):
        header = ROOT / 'tools/gpu-compiler/Compiler.h'
        added = ROOT / 'tools/gpu-compiler/AIRFailureCache.h'
        changes = {header: self.read(header) + b'\n#include "AIRFailureCache.h"\n'}
        before = self.context(changes)
        changes[added] = self.read(added) + b'\n// changed newly included dependency\n'
        self.assertNotEqual(before, self.context(changes))

    def test_moved_adapter_semantics_require_dependency_update(self):
        path = ROOT / CONTEXT.ADAPTER
        old = self.read(path)
        with self.assertRaisesRegex(ValueError, 'shader cache section'):
            self.context({path: old.replace(b'bool nativeFunctionConstantABI()', b'bool movedConstantABI()')})


if __name__ == '__main__':
    unittest.main()
