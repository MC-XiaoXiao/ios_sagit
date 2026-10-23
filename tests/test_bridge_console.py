"""Shell acknowledgements split by actual GPU observer output."""
import importlib.util
from pathlib import Path
import unittest
import os
import pty
import tty
import json
import subprocess
import tempfile
import time
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('bridge_console', ROOT/'scripts/test-bridge.py')
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class ConsoleReplyTests(unittest.TestCase):
    def test_request_separates_unterminated_output_and_keeps_failure_status(self):
        request=bridge.console_request('printf "kernel diagnostic without newline"; false',3,b'fresh')
        result=subprocess.run(['bash','-c',request.decode().rstrip('\r')],capture_output=True,check=True)
        self.assertIn(b'without newline\nDEV_CONSOLE_RESULT',result.stdout)
        self.assertEqual(bridge.console_reply(result.stdout,3,b'fresh')[0][1],b'1')
        self.assertIsNone(bridge.console_reply(request,3,b'fresh')[0])

    def test_actual_split_reply(self):
        # Exact observed failure shape from round132; no inferred status.
        raw = (b'\rDEV_CONSOLE_RESUQcomGPU: cpu-inherited-clock cluster=1 ticks=1451133930 stable=1 rcgr-cmd=2\r\n'
               b'LT index=0 statuQcomGPU: clock-observation sample=10 ticks=1451133930 gpu-status=0\r\n'
               b's=0\r\r\n')
        match, removed = bridge.console_reply(raw, 0)
        self.assertEqual(match[1], b'0')
        self.assertEqual(removed, 2)
        self.assertIn(b'gpu-status=0', raw)

    def test_raw_failure_status_and_index(self):
        raw = b'\rDEV_CONSOLE_RESULT index=7 status=1\r\n'
        match, removed = bridge.console_reply(raw, 7)
        self.assertEqual((match[1], removed), (b'1', 0))
        self.assertIsNone(bridge.console_reply(raw, 8)[0])

    def test_echo_and_incomplete_rows_are_not_replies(self):
        self.assertIsNone(bridge.console_reply(b'echo DEV_CONSOLE_RESULT index=0 status=$?\r\n', 0)[0])
        self.assertIsNone(bridge.console_reply(b'\rDEV_CONSOLE_RESUQcomGPU: clock-observation sample=1', 0)[0])

    def test_fault_rows_are_not_removed(self):
        raw = b'\rDEV_CONSOLE_RESUQcomGPU: fault recoverable=0 jobs=16\r\nLT index=0 status=0\r\n'
        self.assertEqual(bridge.console_reply(raw, 0), (None, 0))

    def test_previous_capture_reply_cannot_finish_new_command(self):
        raw = b'\rDEV_CONSOLE_RESULT index=0 status=0 token=old\r\n'
        self.assertIsNone(bridge.console_reply(raw, 0, b'new')[0])
        fresh = raw + b'DEV_CONSOLE_RESULT index=0 status=1 token=new\r\n'
        self.assertEqual(bridge.console_reply(fresh, 0, b'new')[0][1], b'1')

    def test_bash_requires_current_run_and_actual_expansion(self):
        raw = b'\rDEV_BASH_VERIFIED version=5.2.21 uid=0 pid=94 cwd=/ token=old\r\n'
        self.assertFalse(bridge.bash_reply(raw, b'new'))
        self.assertFalse(bridge.bash_reply(b'echo DEV_BASH_VERIFIED version=$BASH_VERSION uid=$EUID pid=$$ cwd=$PWD token=new\r\n', b'new'))
        self.assertTrue(bridge.bash_reply(raw.replace(b'token=old', b'token=new'), b'new'))

    def test_bash_reply_split_by_observer(self):
        raw = b'\rDEV_BASH_VERIFIED version=5.2.21 uid=QcomGPU: clock-observation sample=1\r\n0 pid=94 cwd=/ token=new\r\n'
        self.assertTrue(bridge.bash_reply(raw, b'new'))


class UARTTransportTests(unittest.TestCase):
    def test_quiet_and_readable_do_not_consume_bytes(self):
        master, slave = pty.openpty()
        try:
            tty.setraw(slave)
            self.assertFalse(bridge.uart_readable(slave, 0.001))
            os.write(master, b'GPU_UART_DATA')
            self.assertTrue(bridge.uart_readable(slave, 0.05))
            self.assertEqual(os.read(slave, 100), b'GPU_UART_DATA')
        finally:
            os.close(master)
            os.close(slave)

    def test_peer_disconnect_is_not_a_silent_readable_loop(self):
        master, slave = pty.openpty()
        try:
            tty.setraw(slave)
            os.close(master)
            with self.assertRaises(bridge.UARTTransportError):
                bridge.uart_readable(slave, 0.05)
        finally:
            os.close(slave)

    def test_closed_descriptor_is_rejected(self):
        master, slave = pty.openpty()
        os.close(slave)
        try:
            with self.assertRaises(bridge.UARTTransportError):
                bridge.uart_readable(slave, 0)
        finally:
            os.close(master)

    def test_capture_saves_transport_failure_without_gpu_timeout(self):
        master, slave = pty.openpty()
        try:
            with tempfile.TemporaryDirectory() as temporary:
                folder = Path(temporary)
                profile = folder/'profile.json'
                profile.write_text(json.dumps({'uart': os.ttyname(slave)}))
                output = folder/'capture'
                process = subprocess.Popen([sys.executable, str(ROOT/'scripts/test-bridge.py'),
                    '--observe-only', '--profile', str(profile), '--output', str(output), '--seconds', '5'],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                try:
                    deadline = time.monotonic()+2
                    while not (output/'uart.bin').exists() and time.monotonic()<deadline:
                        os.write(master, b'prior diagnostic bytes\r\n')
                        time.sleep(0.02)
                    self.assertTrue((output/'uart.bin').exists())
                    os.close(master)
                    master = None
                    process.communicate(timeout=2)
                    self.assertNotEqual(process.returncode, 0)
                    report = json.loads((output/'report.json').read_text())
                    self.assertTrue(report['uart_transport_disconnected'])
                    self.assertTrue(report['console_stopped_after_failure'])
                    self.assertFalse(report['console_command_timeout'])
                    self.assertFalse(report['panic_detected'])
                    self.assertIn(b'prior diagnostic bytes', (output/'uart.bin').read_bytes())
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.communicate()
        finally:
            if master is not None:
                os.close(master)
            os.close(slave)


class ConsoleMirrorTests(unittest.TestCase):
    def test_only_mirror_argument_changes(self):
        spec = importlib.util.spec_from_file_location('console_bridge_build', ROOT/'scripts/build-bridge.py')
        build = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(build)
        chosen = {'name':'chosen','props':{'bootargs':b'-v serial=19 mirror-console=1 gpu-pipe=6 mirror-console=1\0','other':b'unchanged'}}
        build.disable_console_mirror({'children':[chosen]})
        self.assertEqual(chosen['props'], {'bootargs':b'-v serial=19 gpu-pipe=6 mirror-console=0\0','other':b'unchanged'})


if __name__ == '__main__':
    unittest.main()
