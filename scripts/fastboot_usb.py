"""Bounded USB Fastboot operations. No serial console or device profile required."""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import re
import subprocess
import time
import zlib

from mi6_boot import MAX_IMAGE, check_efi


@dataclass
class Fastboot:
    executable: str = 'fastboot'
    timeout: float = 90
    commands: list[dict] = field(default_factory=list)

    def run(self, *words: str, serial: str | None = None, timeout: float | None = None) -> str:
        argv = [self.executable] + (['-s', serial] if serial else []) + list(map(str, words))
        record = {'argv': argv}
        self.commands.append(record)
        try:
            result = subprocess.run(argv, capture_output=True, text=True,
                                    timeout=self.timeout if timeout is None else timeout)
        except subprocess.TimeoutExpired as error:
            record['error'] = 'timeout; operation outcome unknown'
            raise TimeoutError(f'Fastboot timed out: {" ".join(words)}; check the device before retrying') from error
        output = result.stdout + result.stderr
        record.update(returncode=result.returncode, output=output)
        if result.returncode:
            raise RuntimeError(f'Fastboot {" ".join(words)} failed:\n{output.strip()}')
        return output

    def devices(self) -> list[str]:
        output = self.run('devices', timeout=5)
        return [parts[0] for line in output.splitlines()
                if len(parts := line.split()) >= 2 and parts[1] == 'fastboot']

    def select(self, serial: str | None = None, *, wait: bool = False) -> str:
        deadline = time.monotonic() + (self.timeout if wait else 0)
        while True:
            devices = self.devices()
            candidates = [item for item in devices if serial is None or item == serial]
            if len(candidates) > 1:
                raise ValueError('multiple Fastboot devices match; connect one device or pass --serial')
            if candidates:
                return candidates[0]
            if time.monotonic() >= deadline:
                raise TimeoutError(f'no USB Fastboot device found{": " + serial if serial else ""}')
            time.sleep(0.25)

    def getvar(self, serial: str, name: str) -> str:
        output = self.run('getvar', name, serial=serial, timeout=min(self.timeout, 10))
        match = re.search(r'^(?:\(bootloader\)\s*)?' + re.escape(name) + r':\s*([^\r\n]*)', output, re.M)
        if not match or not match[1].strip():
            raise ValueError(f'Fastboot did not return {name}')
        return match[1].strip()

    def require_sagit(self, serial: str) -> str:
        product = self.getvar(serial, 'product')
        if product != 'sagit':
            raise ValueError(f'expected Mi 6 / sagit, found {product!r}; no flash performed')
        return product

    def require_edk2(self, serial: str) -> None:
        self.require_sagit(serial)
        version = self.getvar(serial, 'version-bootloader')
        if version != 'edk2-fastboot-1':
            raise ValueError('EFI launch requires EDK2 USB Fastboot; flash EDK2 first')

    def limit(self, serial: str, name: str) -> int:
        value = self.getvar(serial, name)
        try:
            limit = int(value, 16) if value.lower().startswith('0x') or re.search('[a-fA-F]', value) else int(value)
        except ValueError as error:
            raise ValueError(f'invalid Fastboot {name}: {value!r}') from error
        if limit <= 0:
            raise ValueError(f'invalid Fastboot {name}: {value!r}')
        return limit

    def flash_boot(self, serial: str, image: Path) -> None:
        size = image.stat().st_size
        partition = self.limit(serial, 'partition-size:boot')
        download = self.limit(serial, 'max-download-size')
        if size > min(partition, download, MAX_IMAGE):
            raise ValueError(f'image is {size} bytes; boot/download limit is {min(partition, download, MAX_IMAGE)}')
        # EDK2 accepts raw Android v0 only. Disable the host's sparse conversion.
        self.run('-S', '0', 'flash', 'boot', str(image.resolve()), serial=serial)

    def launch_efi(self, serial: str, payload: Path) -> int:
        data = payload.read_bytes()
        check_efi(data)
        self.require_edk2(serial)
        if len(data) > min(self.limit(serial, 'max-download-size'), MAX_IMAGE):
            raise ValueError('EFI exceeds the Fastboot download limit; use a compressed boot image')
        self.run('stage', str(payload.resolve()), serial=serial)
        crc = zlib.crc32(data)
        output = self.run('oem', 'crc32', serial=serial)
        if not re.search(r'crc32=' + f'{crc:08x}' + r'\b', output.lower()):
            raise ValueError('downloaded EFI CRC32 differs from host; EFI was not started')
        self.run('oem', 'run-efi', serial=serial)
        return crc
