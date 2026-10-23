import importlib.util
import os
from pathlib import Path
import pty
import select
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('usb_storage_control', ROOT/'scripts/usb-storage.py')
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)


class StorageSerialControlTests(unittest.TestCase):
    def setUp(self):
        self.master, self.slave = pty.openpty()
        self.fd = control.open_uart(os.ttyname(self.slave))

    def tearDown(self):
        os.close(self.fd); os.close(self.slave); os.close(self.master)

    def peer(self, reply):
        received = bytearray()
        def run():
            while not received.endswith(b'\r') or not received.strip(b'\r\n'):
                if not select.select([self.master], [], [], 2)[0]: return
                received.extend(os.read(self.master, 1024))
            for part in reply:
                os.write(self.master, part)
        thread = threading.Thread(target=run)
        thread.start()
        return thread, received

    def test_status_requires_exporter_acknowledgement(self):
        thread, sent = self.peer([b'[mi6-storage] sta', b'tus: exporting UFS; serial-control=v1\r\n'])
        try:
            result = control.exchange(self.fd, 'status', '[mi6-storage] status: exporting UFS; serial-control=v1', timeout=1)
            self.assertIn(b'serial-control=v1', result)
        finally:
            thread.join(2)
        self.assertEqual(sent, b'\rstatus\r')

    def test_flush_failure_does_not_count_as_reboot_success(self):
        thread, sent = self.peer([b'[mi6-storage] ERROR: storage flush failed; reboot cancelled\r\n'])
        try:
            with self.assertRaisesRegex(RuntimeError, 'flush failed'):
                control.exchange(self.fd, 'reboot', '[mi6-storage] flushed;', timeout=1)
        finally:
            thread.join(2)
        self.assertEqual(sent, b'\rreboot\r')

    def test_command_echo_is_not_an_acknowledgement(self):
        thread, _ = self.peer([b'status\r\n'])
        try:
            with self.assertRaises(TimeoutError):
                control.exchange(self.fd, 'status', '[mi6-storage] status:', timeout=0.2)
        finally:
            thread.join(2)


if __name__ == '__main__':
    unittest.main()
