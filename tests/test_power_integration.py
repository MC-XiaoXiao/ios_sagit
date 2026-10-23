"""Power descriptors and the actual cross-linked userland ABI; no device access."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('power_toolchain', ROOT/'scripts/toolchain.py')
toolchain = importlib.util.module_from_spec(spec)
spec.loader.exec_module(toolchain)

class PowerIntegrationTests(unittest.TestCase):
    def test_generated_descriptors_are_reproducible(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as tmp:
            output = Path(tmp)
            for name in ('QcomMMCC','QcomGPUCC'): (output/'clock'/name).mkdir(parents=True)
            old = toolchain.DRIVERS_ROOT
            try:
                toolchain.DRIVERS_ROOT = output
                toolchain.collect_power_clocks()
            finally:
                toolchain.DRIVERS_ROOT = old
            for name in ('QcomMMCC','QcomGPUCC'):
                self.assertEqual((output/'clock'/name/'ClockTables.h').read_bytes(),
                                 (ROOT/'drivers/clock'/name/'ClockTables.h').read_bytes())

    def test_reference_correction_has_qualcomm_evidence(self):
        reference = ROOT/'docs/power/reference'
        sources = json.loads((reference/'sources.json').read_text())
        for name in ('clock-local2.c','clock-mmss-8998.c','clock-osm.c'):
            self.assertEqual(hashlib.sha256((reference/name).read_bytes()).hexdigest(),sources[name]['sha256'])
            self.assertIn('e60e904d4e7457386748f5e80fe8b56da7a79bd1',sources[name]['url'])
        local = (reference/'clock-local2.c').read_text()
        self.assertIn('writel_relaxed(nf->n_val, N_REG(rcg))',local)
        self.assertIn('n_regval >> 8',local)

    def test_user_client_links_real_ios_frameworks(self):
        path = ROOT/'build/output/powerctl'
        self.assertTrue(path.exists(),'build with make power-client before offline integration checks')
        data = path.read_bytes()
        report = json.loads(Path(str(path)+'.json').read_text())
        self.assertEqual(report['sha256'],hashlib.sha256(data).hexdigest())
        self.assertEqual(report['source_sha256'],hashlib.sha256((ROOT/'tools/powerctl/powerctl.cpp').read_bytes()).hexdigest())
        self.assertFalse(report['hardware_tested'])
        magic,cpu,subtype,kind,ncmds,size,flags,reserved = struct.unpack_from('<8I',data)
        self.assertEqual((magic,cpu,kind),(0xfeedfacf,0x100000c,2))
        libraries=[]; ios=False; pos=32
        for _ in range(ncmds):
            cmd,n=struct.unpack_from('<II',data,pos)
            self.assertGreaterEqual(n,8);self.assertLessEqual(pos+n,len(data))
            if cmd==0xc:
                offset=struct.unpack_from('<I',data,pos+8)[0]
                libraries.append(data[pos+offset:pos+n].split(b'\0')[0].decode())
            if cmd==0x32: ios=struct.unpack_from('<I',data,pos+8)[0]==2
            pos+=n
        self.assertEqual(pos,32+size);self.assertTrue(ios)
        self.assertIn('/usr/lib/libSystem.B.dylib',libraries)
        self.assertTrue(any('IOKit.framework' in name for name in libraries))
        self.assertTrue(any('CoreFoundation.framework' in name for name in libraries))

if __name__ == '__main__': unittest.main()
