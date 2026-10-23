"""Run the real raw entry, PIE relocations, FDT selection and Mach-O handoff."""
from pathlib import Path
import select
import shutil
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('qemu-system-aarch64'), 'QEMU is optional')
class RawBridgeTest(unittest.TestCase):
    def test_generic_arm64_handoff(self):
        self.run_handoff(console=True)

    def test_explicit_no_uart_handoff(self):
        self.run_handoff(console=False)

    def run_handoff(self, console):
        out = ROOT/'build/bridge'/('qemu-probe' if console else 'qemu-probe-no-uart')
        out.mkdir(parents=True, exist_ok=True)
        subprocess.run(['clang', '--target=aarch64-none-elf', '-c', str(ROOT/'tests/arm64_handoff_probe.S'), '-o', str(out/'probe.o')], check=True)
        subprocess.run(['ld.lld', '--image-base=0', '-Ttext=0', '-e', 'probe_entry', '-o', str(out/'probe.elf'), str(out/'probe.o')], check=True)
        subprocess.run(['llvm-objcopy', '-O', 'binary', str(out/'probe.elf'), str(out/'probe.bin')], check=True)
        code = (out/'probe.bin').read_bytes(); blob = bytearray(4096)+code
        va = 0xfffffff007004000
        struct.pack_into('<8I', blob, 0, 0xfeedfacf, 0x100000c, 0, 2, 2, 360, 0, 0)
        struct.pack_into('<II16sQQQQIIII', blob, 32, 0x19, 72, b'__TEXT_EXEC', va, 4096, 4096, len(code), 7, 5, 0, 0)
        struct.pack_into('<4I', blob, 104, 5, 288, 6, 68); struct.pack_into('<Q', blob, 376, va)
        (out/'probe.macho').write_bytes(blob)
        dts = ROOT/'device_trees/qemu-virt.dts'
        if not console:
            contents = dts.read_text().replace('serial-device = <&uart>;', '')
            contents = contents.replace('chosen {', 'chosen { xnu,allow-no-uart = <1>;')
            dts = out/'no-uart.dts'
            dts.write_text(contents)
        subprocess.run(['python3', str(ROOT/'scripts/build-bridge.py'), '--kernel', str(out/'probe.macho'), '--dts', str(dts), '--arena-min', '0x40000000', '--watchdog-seconds', '0', '--output', str(out)], check=True)
        for virtualization in ['off', 'on']:
            with self.subTest(virtualization=virtualization):
                proc = subprocess.Popen(['qemu-system-aarch64', '-machine', 'virt,gic-version=3,virtualization='+virtualization, '-cpu', 'cortex-a57', '-m', '1024', '-smp', '1', '-nographic', '-monitor', 'none', '-no-reboot', '-kernel', str(out/'Image')], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                log = bytearray(); end = time.monotonic()+10
                try:
                    while time.monotonic()<end and proc.poll() is None:
                        if select.select([proc.stdout], [], [], 0.1)[0]: log.extend(proc.stdout.read1(65536))
                        if b'[ARM64 probe] Mach-O entry / boot_args OK' in log: break
                finally:
                    proc.terminate()
                    try: proc.wait(timeout=3)
                    except subprocess.TimeoutExpired: proc.kill(); proc.wait()
                    proc.stdout.close()
                    (out/('uart-el'+('2' if virtualization == 'on' else '1')+'.txt')).write_bytes(log)
                if console:
                    self.assertIn(b'[XNU bridge] ARM64 Image entry', log)
                else:
                    self.assertNotIn(b'[XNU bridge]', log)
                self.assertIn(b'[ARM64 probe] Mach-O entry / boot_args OK', log)
if __name__ == '__main__': unittest.main()
