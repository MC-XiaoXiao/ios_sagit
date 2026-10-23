# MI6 iOS

An XNU and iOS port for the Xiaomi Mi 6 (`sagit`, Snapdragon 835 / MSM8998).
The project includes a Linux cross-compilation toolchain, platform support,
Qualcomm IOKit drivers, an EFI boot bridge, and APFS image and installation tools.

The device boots from UFS/APFS to the SpringBoard home screen, with app icons,
the Dock, and physical touch input. Development is ongoing.

```text
Stock Android bootloader
  -> EDK2 in the boot partition
  -> XnuBridge.efi (kernel, device tree, and boot payloads)
  -> XNU + native Apple Kexts + Qualcomm drivers
  -> UFS / APFS -> launchd -> SpringBoard
```

## Device support

Hardware status is based on validation through October 22, 2026.

| Feature | Status | Details |
| --- | --- | --- |
| Kernel / SMP | Working | 2, 4, and 8 CPU cores tested; 4 cores by default |
| UFS / APFS | Working | Native root boot, System and Data mounts, and Data reads and writes |
| Home screen | Working | SpringBoard, app icons, and Dock with development Setup configuration |
| Display / backlight | Partial | 1080×1920 output, display submission, and backlight takeover; cold panel initialization, automatic brightness, and suspend/resume remain incomplete |
| Touch | Working | Native HID / BackBoard / UIKit path; physical swipes and taps tested; full multitouch coverage remains unverified |
| Adreno 540 / Metal | Partial | Firmware authentication, hardware submission, Compute / Render, IOSurface readback, and system compositing tested; API coverage and long-term stability remain incomplete |
| USB device mode | Working | High-Speed enumeration, AppleUSBDeviceMux, device information queries, and reconnect; Host / OTG, USB 3, and full pairing services remain unverified |
| Battery | Partial | Read-only telemetry and charging connection UI; charging control and full powerd integration remain incomplete |
| Buttons / Home | Working | Input events; fingerprint authentication is unavailable |
| Wi-Fi | Unavailable | Scan, association, and data transfer are not operational |
| Bluetooth | Unavailable | Native stack integration and pairing remain incomplete |
| Cellular | Unavailable | No working SIM, calls, or mobile data |
| Audio | Unavailable | Driver components exist; speaker playback has not been validated |
| Camera / biometrics | Unavailable | No usable camera or native fingerprint authentication |
| Haptics / gyroscope | Unverified | Offline driver implementations; CoreHaptics / CoreMotion integration remains incomplete |
| Power management | Incomplete | DVFS, thermal management, and system suspend/resume remain unverified |

Android dual boot, normal activation, App Store, DRM, and general application
compatibility are not validated. The current system uses development policies
for operation without an Apple SEP.

Validation details: [home screen and touch](docs/apfs/setup-skip-hardware-20261022.md),
[SMP](docs/smp/README.md), [display](docs/display/README.md),
[GPU](docs/gpu/README.md), [USB](docs/usb/README.md), and
[battery](docs/battery/README.md).

## Repository layout

```text
kernel/          XNU and platform code
drivers/         Drivers grouped by function
  abi-22H374/    Firmware-specific ABI contracts (*abi.json)
  platform/     Platform, interrupts, SMEM, and watchdog
  clock/        GCC, GPUCC, MMCC, and OSM
  bus/          GPIO, I2C, SPI, SPMI, SMMU, and interconnect
  power/        PMIC, RPM, power management, and battery
  security/     SCM and AES
  storage/      UFS
  usb/          USB
  display/      Display, DSI, and backlight
  gpu/          GPU Kext and Metal components
  input/        Touch, buttons, Home, gyroscope, and haptics
  audio/        Audio
  wifi/         WLAN Kext and DriverKit components
  bluetooth/    Bluetooth and userspace components
  baseband/     Modem components
  include/      Public interfaces
  shared/       Shared kernel and driver code
boot/            EFI bridge, UFS exporter, and EDK2 sources
device_trees/    Board device trees
scripts/         Build, packaging, analysis, and flashing tools
tests/           Host tests and cross-compilation checks
tools/           Toolchain, SDK, patches, and diagnostic clients
docs/            Technical documentation and validation records
firmware/        Local firmware inputs
build/           Build outputs, images, caches, and reports
```

See the [driver inventory](drivers/README.md). Paths in `drivers/Build.json`
are relative to `drivers/`. Modules can be selected by name, such as
`--driver QcomUSB`.

## Flashing with Fastboot

Requirements: an unlocked Mi 6 bootloader, a USB data cable, and `fastboot`
from Android platform-tools. No serial console is required.

The boot image and APFS system image are separate. Flashing `boot` does not
install the iOS filesystem. The APFS installation must match the root volume
UUID, drivers, and firmware configuration embedded in the bridge.

Enter stock Fastboot mode and run from the project directory:

```sh
python3 scripts/flash.py --image build/boot/boot-sagit.img --boot
```

The tool selects the connected device and checks `product=sagit`, boot
partition capacity, and the download limit before flashing. Use `--serial SERIAL`
when multiple devices are connected. Omit `--boot` to remain in Fastboot after
flashing.

A packaged image can also be flashed with standard Fastboot alone:

```sh
fastboot getvar product
fastboot -S 0 flash boot boot-sagit.img
fastboot reboot
```

Images built with `--autoboot` start EDK2, the bridge, and the kernel on each
power-on. The default countdown is five seconds. Press a volume key to cancel
and enter EDK2 USB Fastboot. Missing or rejected payloads and EFI startup errors
also return to Fastboot.

The combined image loader has passed EDK2 compilation and host tests.
Automatic boot from the combined image has not yet been validated on hardware.

For recovery, hold Volume Down + Power to enter stock Fastboot, then flash a
plain EDK2 image or the original system's `boot.img`.

## Build requirements

The validated build host is Linux x86_64. The kernel is `xnu-11417.140.69`
(Darwin 24.6.0), built for `arm64` with the iPhoneOS 18.6 SDK.

Install the following host dependencies:

- `make`, Python 3.11+, Clang with C++17 support, LLVM / LLD, `llvm-config`,
  `llvm-objcopy`, `llvm-ar`, CMake, Ninja, and Git.
- `flex`, `bison`, `m4`, Autoconf, Automake, Libtool, `patch`, Perl, Bash,
  `curl`, `dtc`, and `pkg-config`.
- tinyxml2, Capstone, Keystone, and LLVM development libraries for the native
  kernelcache tools. The GPU compiler tools currently require LLVM 23.
- FUSE 3 development files for APFS tools. Their bootstrap prepares pinned
  versions of Go and `ipsw`.
- GCC, an AArch64 GNU cross-compiler, `iasl`, NASM, and `uv` for EDK2.
  The compiler wrappers use `/usr/bin/gcc` and
  `/usr/bin/aarch64-none-linux-gnu-gcc`; adjust `scripts/edk2-*-gcc.sh` for
  different installation paths.

## Building the kernel

```sh
make doctor
make bootstrap JOBS=3
make collect-sdm835
make build TARGET=SDM835 JOBS=3
make verify TARGET=SDM835
```

Bootstrap downloads the SDK, tools, and pinned sources listed in
`tools/dependencies.json`. An internet connection is required on the first run.

The repository includes the build scripts, kernelcache patcher, GPU compiler,
IIG sources, compatibility headers, and dependency patches. Downloaded SDKs,
tool binaries, caches, and build outputs are regenerated by bootstrap.

Outputs:

```text
build/output/mach.development.sdm835
build/output/mach.development.sdm835.unstripped
build/output/verification-sdm835.json
```

## Firmware and drivers

The native Apple Kexts and userspace are based on:

| Input | Value |
| --- | --- |
| IPSW | `firmware/iPad_10.2_18.7.10_22H374_Restore.ipsw` |
| System version | iPadOS 18.7.10 |
| Build | `22H374` |
| Board | `j171ap` |
| Native kernelcache | `kernelcache.release.ipad7c` |
| Port device identifier | `iPhoneMi6,1` |

ABI contracts are stored in `drivers/abi-22H374/`. A different native provider
requires corresponding ABI analysis and contracts.

Prepare the firmware and decompress the native kernelcache:

```sh
python3 scripts/build-apfs.py bootstrap
python3 scripts/build-apfs.py firmware firmware/iPad_10.2_18.7.10_22H374_Restore.ipsw
python3 - <<'PY'
from pathlib import Path
from zipfile import ZipFile
import shutil
out = Path('build/ios18-kext-analysis')
out.mkdir(parents=True, exist_ok=True)
with ZipFile('firmware/iPad_10.2_18.7.10_22H374_Restore.ipsw') as archive:
    with archive.open('kernelcache.release.ipad7c') as source, (out / 'kernelcache.release.ipad7c').open('wb') as target:
        shutil.copyfileobj(source, target)
PY
tools/prefix/libexec/ipsw/ipsw kernel dec \
  build/ios18-kext-analysis/kernelcache.release.ipad7c \
  -o build/ios18-kext-analysis/kernelcache.release.ipad7c.macho
```

Build the drivers and APFS kernel collection:

```sh
make drivers JOBS=3
make verify-drivers
make apfs-kernelcache JOBS=3
```

Outputs are `build/output/Extensions/` and
`build/output/kernelcache.sdm835.apfs`. To build an individual module, use
`make drivers DRIVER_FLAGS='--driver QcomUSB'`.

The graphics collection is built with
`make kernelcache COLLECTION=graphics-runtime`. Metal userspace components
also require `make gpu-shaders gpu-metal gpu-client`.
See [Kext and ABI analysis](docs/sdm835/ios18-kext-analysis.md) and
[Metal support](docs/gpu/metal.md).

## Building APFS and the boot image

Create the System and Data image with at least 2048 MiB of shared APFS free space:

```sh
python3 scripts/build-apfs.py image --initialize-persona
```

Add `--development-setup-complete` to seed development Setup completion
preferences. A working desktop also requires matching GPU / Metal components,
the Drivers volume, trust caches, Cryptex configuration, and userspace changes.
See [APFS boot configuration](docs/apfs/README.md).

Build a UFS/APFS bridge with development policies for operation without SEP:

```sh
python3 scripts/build-bridge.py \
  --kernel build/output/kernelcache.sdm835.apfs \
  --apfs-report build/apfs/ios.apfs.json \
  --firmware-report build/apfs/prepared-firmware.json \
  --ufs-handoff --full-memory --cpus 4 \
  --development-unsealed-root --development-no-sep \
  --development-legacy-data --development-hactivation \
  --development-no-apple-pmgr --development-watchdog-policy \
  --development-unthrottled-io --disable-system-sleep \
  --output build/apfs/bridge
```

The output is `build/apfs/bridge/XnuBridge.efi`, with the kernel and device tree
embedded. `make bridge` uses a bare kernel by default for early bring-up.

Build EDK2 and package an automatic boot image:

`make edk2-sagit` prepares the exact dependency revisions in
`tools/edk2-sagit.json` and applies the complete local source snapshots under
`tools/patches/edk2-sagit/`. The EDK2 checkouts are downloaded on the first run.

```sh
make edk2-sagit
make boot-image BRIDGE_EFI=build/apfs/bridge/XnuBridge.efi AUTOBOOT=1
```

Or invoke the packer directly:

```sh
python3 scripts/build-boot.py \
  --edk2 build/edk2-sagit/boot-sagit.img \
  --bridge build/apfs/bridge/XnuBridge.efi \
  --autoboot --boot-delay 5 \
  --output build/boot/boot-sagit.img
```

Omit `--autoboot`, or set `AUTOBOOT=0`, to stay in USB Fastboot by default.
The packer requires EDK2 with the payload loader, preserves its firmware and
Android DTB, compresses the bridge, verifies the packed contents, and enforces
a 64 MiB image limit. The build report is saved alongside the image as `.json`.

Automatic boot reads the payload from the **boot partition**. Temporary launch
of the combined image with `fastboot boot` is unsupported. To start an EFI once
from an existing EDK2 USB Fastboot session:

```sh
python3 scripts/flash.py --payload build/apfs/bridge/XnuBridge.efi --boot
```

This uses `stage`, device-side CRC32 verification, and `oem run-efi`.
See [Fastboot boot configuration](docs/sdm835/fastboot-boot.md).

## Installing APFS

Flash plain EDK2 and start the USB storage exporter:

```sh
python3 scripts/flash.py --image build/edk2-sagit/boot-sagit.img --boot
make usb-storage
python3 scripts/usb-storage.py start --no-uart
lsblk -o NAME,SIZE,MODEL,PARTLABEL,PARTUUID
```

Identify the entire UFS LUN containing `userdata`. Replace `DEVICE` below with
its stable device path. Block device operations require appropriate permissions.

**Installation erases Android userdata and modifies GPT. A GPT backup does not
restore user files.** Review the generated plan before running `install`.

```sh
python3 scripts/apfs-storage.py plan \
  --device DEVICE --image-report build/apfs/ios.apfs.json \
  --output build/apfs/device-plan.json

python3 scripts/apfs-storage.py install \
  --device DEVICE --plan build/apfs/device-plan.json \
  --image build/apfs/ios.apfs --backup build/apfs/device-gpt-backup \
  --erase-userdata

python3 scripts/apfs-storage.py verify-device-hash \
  --device DEVICE --plan build/apfs/device-plan.json \
  --output build/apfs/device-hash.json

python3 scripts/usb-storage.py flush --no-uart --device DEVICE
```

After installation and flushing, restart using the power button, enter stock
Fastboot, and flash the combined boot image. The exporter has no USB reboot
command, so a physical restart is required without a serial console.

## Validation

```sh
make test-fastboot
make verify TARGET=SDM835
make verify-drivers
```

Fastboot tests cover image packing, device and capacity checks, download
verification, and the EDK2 payload loader and LZMA decoder. Individual driver
host tests can be selected with `make test-drivers TEST_FLAGS='--test NAME'`.

## References and licenses

Boot image packaging draws on [UniXNU bootimg](https://github.com/UniXNU/UniXNU/blob/141d16a43bfe46859487f32c0519c428cc473a49/scripts/unixnu/bootimg.py)
and its [ramdisk payload design](https://github.com/UniXNU/UniXNU-sheng/blob/251b291adf941b9953fe3ae66a808b9b626e164d/scripts/package-sheng-xnu-boot.py).
The port uses XnuBridge for Mi 6 hardware handoff.

XNU, EDK2, Linux-derived components, and other third-party code retain their
respective licenses and attribution. Dependency versions and patches are under
`tools/`; driver notices are included in their module directories.
