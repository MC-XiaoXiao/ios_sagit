"""Exercise production charge policy and the board's Apple DT permission cell."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from project import ROOT
from project.build import bridge


class BatteryControlTests(unittest.TestCase):
    def compile_and_run(self, name, shim=False):
        compiler = shutil.which('clang++') or shutil.which('g++')
        self.assertIsNotNone(compiler)
        folder = ROOT / 'drivers/power/QcomBattery'
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'charger-test'
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            *(['-I', str(folder / 'tests/shim')] if shim else []),
                            '-I', str(folder), str(folder / 'tests' / name),
                            '-o', str(output)], check=True)
            subprocess.run([str(output)], check=True)

    def test_charger_fault_injection(self):
        self.compile_and_run('charger_test.cpp')

    def test_production_iokit_adapter(self):
        self.compile_and_run('runtime_test.cpp', shim=True)

    def test_control_permission_survives_real_board_dt_conversion(self):
        with tempfile.TemporaryDirectory() as directory:
            dtb = Path(directory) / 'board.dtb'
            subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', str(dtb),
                            str(ROOT / 'boards/xiaomi/sagit/board.dts')], check=True)
            blob = bridge.apple_dt(bridge.parse_fdt(dtb.read_bytes()), bytes(range(64)))
        seen = []

        def visit(pos):
            props, children = struct.unpack_from('<II', blob, pos)
            pos += 8
            for _ in range(props):
                key = blob[pos:pos + 32].split(b'\0')[0]
                size, = struct.unpack_from('<I', blob, pos + 32)
                if key == b'qcom,battery-charge-control':
                    seen.append(blob[pos + 36:pos + 36 + size])
                pos += 36 + ((size + 3) & ~3)
            for _ in range(children):
                pos = visit(pos)
            return pos

        self.assertEqual(visit(0), len(blob))
        self.assertEqual(seen, [struct.pack('<I', 1)])


if __name__ == '__main__':
    unittest.main()
