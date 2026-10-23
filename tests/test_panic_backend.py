"""Compile the production XNU character sink with both reporting modes."""
import json
import importlib.util
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPILE = ROOT/'build/obj/DEVELOPMENT_ARM64_SDM835/osfmk/DEVELOPMENT/printf.o.json'
spec = importlib.util.spec_from_file_location('test_bridge_device', ROOT/'scripts/test-bridge.py')
device_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(device_test)


class PanicEvidenceTest(unittest.TestCase):
    def test_early_fallback_does_not_pass_as_complete_report(self):
        text = ('software panic output self-test: value=42 hex=0x835 @arm_init.c:661\n'
                'kernel panicked before debug and trace infrastructure initialized!\nspinning forever...\n')
        self.assertFalse(device_test.panic_evidence(text)['software_panic'])

    def test_truncated_backtrace_does_not_pass_as_complete_report(self):
        text = ('panic(cpu 0 caller 0xfffffff007309718): software panic output self-test: value=42 hex=0x835\n'
                'Debugger message: panic\nKernel version: Darwin Kernel Version 24.6.0\nPanicked thread: 0xfffffff0\n'
                + 'lr: 0xfffffff007309718 fp: 0xfffffff967c83f20\n' * 10)
        self.assertFalse(device_test.panic_evidence(text)['software_panic'])
        self.assertTrue(device_test.panic_evidence(text + '** Stackshot Succeeded ** Bytes Traced 1749\n')['software_panic'])


@unittest.skipUnless(COMPILE.exists(), 'build SDM835 kernel first')
class PanicBackendTest(unittest.TestCase):
    def test_macro_changes_sink_without_replacing_panic_formatter(self):
        record = json.loads(COMPILE.read_text().strip().rstrip(','))
        srcargs = record['arguments'][1:]; args = []; i = 0
        while i < len(srcargs):
            flag = srcargs[i]
            if flag in ('-o', '-MF', '-MT', '-MQ', '-MJ'): i += 2; continue
            if flag in ('-MD', '-MMD') or flag.startswith('-DXNU_SOFTWARE_PANIC='): i += 1; continue
            args.append(flag); i += 1
        out = ROOT/'build/bridge'; out.mkdir(parents=True, exist_ok=True)
        for mode in (0, 1):
            with self.subTest(mode=mode):
                obj = out/f'panic-sink-{mode}.o'
                with (out/f'panic-sink-{mode}.log').open('w') as log:
                    subprocess.run([str(ROOT/'tools/prefix/bin/clang-arm64'), *args, f'-DXNU_SOFTWARE_PANIC={mode}', '-o', str(obj)], cwd=record['directory'], stdout=log, stderr=subprocess.STDOUT, check=True)
                symbols = subprocess.check_output([str(ROOT/'tools/prefix/bin/arm-apple-darwin11-nm'), '-u', str(obj)], text=True)
                self.assertEqual('_vc_software_panic_putc' in symbols, mode == 1)
                self.assertEqual('_uart_putc' in symbols, mode == 1)
                self.assertIn('_debug_putc', symbols)
                self.assertNotIn(b'[software panic]', obj.read_bytes())
        source = (ROOT/'kernel/osfmk/kern/debug.c').read_text()
        self.assertIn('paniclog_append_noflush("panic(cpu %u caller 0x%lx): "', source)
        self.assertNotIn('software_panic_stop', source)

if __name__ == '__main__': unittest.main()
