# XNUProject

An XNU and iOS platform port, currently targeting the Xiaomi Mi 6
(`sagit`, Snapdragon 835 / MSM8998).
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

Hardware status is based on validation through October 23, 2026.

| Feature | Status | Details |
| --- | --- | --- |
| Kernel / SMP | Working | 2, 4, and 8 CPU cores tested; 4 cores by default |
| UFS / APFS | Working | Native root boot, System and Data mounts, and Data reads and writes |
| Home screen | Working | SpringBoard, app icons, and Dock with development Setup configuration |
| Display / backlight | Partial | 1080×1920 output, brightness control, backlight off/on, and EDR synchronization (SDR headroom 1) tested; cold panel initialization, automatic brightness, and full system suspend/resume remain incomplete |
| Touch | Working | Native HID / BackBoard / UIKit path; physical swipes and taps tested; full multitouch coverage remains unverified |
| Adreno 540 / Metal | Partial | Firmware authentication, hardware submission, Compute / Render, IOSurface readback, and system compositing tested; API coverage and long-term stability remain incomplete |
| USB device mode | Working | High-Speed enumeration, AppleUSBDeviceMux, device information queries, and reconnect; Host / OTG, USB 3, and full pairing services remain unverified |
| Battery | Partial | Telemetry, charging on/off, charge limits, and native iOS battery notifications tested; fast charging and sustained charging remain unverified |
| Buttons / Home | Working | Input events; fingerprint authentication is unavailable |
| Wi-Fi | Unavailable | Scan, association, and data transfer are not operational |
| Bluetooth | Unavailable | Native stack integration and pairing remain incomplete |
| Cellular | Unavailable | No working SIM, calls, or mobile data |
| Audio | Unavailable | Driver components exist; speaker playback has not been validated |
| Camera / biometrics | Unavailable | No usable camera or native fingerprint authentication |
| Haptics / gyroscope | Unverified | Offline driver implementations; CoreHaptics / CoreMotion integration remains incomplete |
| Power management | Partial | CPU DVFS validated with 4 and 8 cores; thermal management and system suspend/resume remain unverified |

Android dual boot, normal activation, App Store, DRM, and general application
compatibility are not validated. The current system uses development policies
for operation without an Apple SEP.

## Repository layout

```text
project.sh       Unified host command entry point
pyproject.toml   Python host package definition (Python 3.11+)
kernel/          XNU source and machine support
drivers/         Drivers grouped by function
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
boot/            EFI bridge and UFS storage exporter
platforms/       SoC descriptions, shared device trees, and handoff code
boards/          Board descriptions, device trees, EDK2 recipes, and handoff profiles
firmware/ios/    Apple firmware descriptions, ABI contracts, headers, and filesystem policy
tools/project/   Build, firmware, device, and analysis modules
tools/           Native tools, diagnostic clients, dependency manifests, and patches
```

Use `./project.sh` to build, prepare firmware, and flash the device.
Board settings live in `boards/xiaomi/sagit/board.toml`;
SoC settings live in `platforms/qcom/msm8998/platform.toml`. Apple-specific
ABI and filesystem settings live in `firmware/ios/22H374-j171ap/`.

## Flashing with Fastboot

Requirements: an unlocked Mi 6 bootloader, a USB data cable, and `fastboot`
from Android platform-tools. Installation and boot instructions use USB;
a direct UART connection is optional for low-level debugging.

The boot image and APFS system image are separate. Flashing `boot` does not
install the iOS filesystem. Use the APFS and boot images from the same release;
the bridge embeds the matching root volume UUID, kernelcache, and trust caches.
See [Installing APFS](#installing-apfs) for the filesystem installation.

Commands below assume the `desktop` selection described under
[Building the kernel](#building-the-kernel). From the project directory:

```sh
WORK=build/xiaomi/sagit/22H374-j171ap/desktop
./project.sh device flash --image "$WORK/output/boot.img" --boot
```

The tool selects the connected device and checks `product=sagit`, boot
partition capacity, and the download limit before flashing. With multiple
devices connected, use `--serial SERIAL` with the USB device identifier from
`fastboot devices`. Omit `--boot` to remain in Fastboot after flashing.

A packaged image can also be flashed with standard Fastboot alone:

```sh
fastboot getvar product
fastboot -S 0 flash boot "$WORK/output/boot.img"
fastboot reboot
```

Packaged images start EDK2 and the bundled bridge immediately by default.
Hold a volume key as EDK2 starts to enter USB Fastboot. The packer's
`--boot-delay 5` adds a five-second recovery countdown. Missing or rejected
payloads and EFI startup errors also return to Fastboot. The bridge has its
own boot menu and USB development service; see [XnuBridge](boot/bridge/README.md).

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
  `/usr/bin/aarch64-none-linux-gnu-gcc`; adjust the wrappers in
  `tools/project/build/edk2/` for different installation paths.
- zlib and libfdt development files for native tools and payload loader checks.

## Building the kernel

Select the board, Apple firmware, and build profile before building:

```sh
./project.sh config --list
./project.sh configure --board xiaomi/sagit --firmware ios/22H374-j171ap --profile desktop
./project.sh config
```

The selection is saved in `.project.json`. Use `desktop` for complete system
images, or `platform` for early platform bring-up.

With the board firmware inputs described below in place:

```sh
./project.sh bootstrap -j 3
./project.sh doctor
./project.sh build kernel -j 3
./project.sh verify kernel
```

Bootstrap downloads the SDK, tools, and pinned sources listed in
`tools/dependencies.json`, plus platform reference inputs pinned under
`platforms/`. Reference inputs are restored into `build/cache/references/`;
the build does not require a populated `local/` directory.
An internet connection is required on the first run.
`doctor` also checks the selected board's local firmware files and hashes.

Kernel outputs for this selection:

```text
build/xiaomi/sagit/22H374-j171ap/desktop/output/
  mach.development.sdm835
  mach.development.sdm835.unstripped
  verification-sdm835.json
```

Use `--workspace PATH` to select another output workspace. Build logs are saved
in its `logs/` directory.

## Firmware and drivers

The native Apple Kexts and userspace are based on:

| Input | Value |
| --- | --- |
| IPSW | `firmware/inputs/iPad_10.2_18.7.10_22H374_Restore.ipsw` |
| System version | iPadOS 18.7.10 |
| Build | `22H374` |
| Board | `j171ap` |
| Native kernelcache | `kernelcache.release.ipad7c` |
| Port device identifier | `iPhoneMi6,1` |

Firmware identity and original component hashes are pinned in
`firmware/ios/22H374-j171ap/firmware.toml`. ABI contracts and private headers
are stored in its `abi/` and `include/` directories. A different native provider
requires corresponding ABI analysis and contracts.

Place the original IPSW under `firmware/inputs/`. The Mi 6 board also requires:

- `firmware/inputs/sagit-stock/fw_sagit_miui_MI6Global_V11.0.6.0.PCAMIXM_47fd8ee4ef_9.0.zip`,
  the original archive used to extract input firmware.
- `firmware/inputs/sagit-gpu/` containing `a530_pfp.fw`, `a530_pm4.fw`,
  `a540_gpmu.fw2`, and `a540_zap.mbn`, matching
  `boards/xiaomi/sagit/gpu-firmware.json`.

Supply these files before running `doctor` or the complete build. Firmware
inputs are not included in the repository.

Inspect an IPSW without building:

```sh
./project.sh firmware inspect firmware/inputs/iPad_10.2_18.7.10_22H374_Restore.ipsw
```

After the complete build has prepared the provider, rebuild drivers or the
selected kernel collection separately:

```sh
./project.sh build drivers -j 3
./project.sh verify drivers
./project.sh build kernelcache -j 3
./project.sh verify kernelcache
```

Outputs are `$WORK/output/Extensions/` and
`$WORK/output/kernelcache.sdm835.ios-runtime`. To build an individual module,
use `./project.sh build drivers --driver QcomUSB`. Firmware-specific collection
roots and build settings are in `firmware/ios/22H374-j171ap/drivers.json`.

Metal userspace components have separate build commands:

```sh
./project.sh build gpu-shaders
./project.sh build gpu-metal
./project.sh build gpu-client
```

## Building APFS and the boot image

From the configured `desktop` selection, build the complete matched system:

```sh
./project.sh build complete -j 3 \
  --ipsw firmware/inputs/iPad_10.2_18.7.10_22H374_Restore.ipsw
```

This builds XNU, drivers, Metal components, APFS, the boot bridge, and EDK2.
The resulting images are ready for installation.

The image reserves at least 2048 MiB of shared APFS free space and includes
development Setup completion and the policies required to operate without an
Apple SEP. Filesystem settings are in
`firmware/ios/22H374-j171ap/filesystem.json` and the selected board profile.

Published files are:

```text
build/xiaomi/sagit/22H374-j171ap/desktop/output/
  ios.apfs
  ios.apfs.json
  XnuBridge.efi
  boot.img
  release.json
```

Use the images listed together in `release.json`. The plain EDK2 image used
for installation and recovery is `$WORK/obj/edk2/boot-sagit.img`.

For component work with the required kernel, drivers, GPU, and board firmware
already prepared, the image and packaging stages can be invoked separately:

```sh
WORK=build/xiaomi/sagit/22H374-j171ap/desktop
./project.sh firmware build \
  --ipsw firmware/inputs/iPad_10.2_18.7.10_22H374_Restore.ipsw
./project.sh firmware bridge \
  --kernel "$WORK/output/kernelcache.sdm835.ios-runtime" \
  -- --dts boards/xiaomi/sagit/board.dts --cpus 4
./project.sh build edk2
./project.sh firmware package \
  --runtime "$WORK/obj/filesystem/bridge" \
  --edk2 "$WORK/obj/edk2/boot-sagit.img"
```

Use a new workspace when changing the filesystem recipe or its inputs.
Existing images and releases are verified before reuse.

To add a five-second recovery countdown to the boot image:

```sh
./project.sh build boot \
  --edk2 "$WORK/obj/edk2/boot-sagit.img" \
  --bridge "$WORK/output/XnuBridge.efi" \
  --boot-delay 5 --output "$WORK/output/boot-delay5.img"
```

Use `--no-autoboot` to stay in EDK2 USB Fastboot. The boot image must fit the
64 MiB boot partition.

To start the combined image once from stock Fastboot without flashing:

```sh
fastboot boot "$WORK/output/boot.img"
```

To start an EFI once from an existing EDK2 USB Fastboot session:

```sh
./project.sh device flash --payload "$WORK/output/XnuBridge.efi" --boot
```

## Installing APFS

Flash plain EDK2 from stock Fastboot, then build and start the USB storage exporter:

```sh
WORK=build/xiaomi/sagit/22H374-j171ap/desktop
./project.sh device flash --image "$WORK/obj/edk2/boot-sagit.img" --boot
./project.sh build usb-storage
./project.sh device flash --payload build/usb-storage/LinuxSimpleMassStorage.efi --boot
lsblk -o NAME,SIZE,MODEL,PARTLABEL,PARTUUID
```

Identify the entire exported UFS LUN containing `userdata`. Set `DEVICE` to its
stable device path, such as the appropriate `/dev/disk/by-id/...` entry.
Do not select a partition or infer the phone from a host disk letter. Block
device operations require appropriate permissions and unmounted partitions.

**First installation erases Android userdata and modifies GPT. A GPT backup
does not restore user files. Replacing an existing APFS image also erases its
iOS System and Data contents.** Review the generated plan before installing.

For the first installation:

```sh
DEVICE=/dev/disk/by-id/REPLACE_WITH_EXPORTED_UFS_LUN
PLAN="$WORK/device-plan.json"
./project.sh device disk plan \
  --device "$DEVICE" --image-report "$WORK/output/ios.apfs.json" \
  --output "$PLAN"
cat "$PLAN"

./project.sh device install \
  --device "$DEVICE" --plan "$PLAN" \
  --image "$WORK/output/ios.apfs" --backup "$WORK/device-gpt-backup" \
  --erase-userdata
```

For replacement of an existing `ios-apfs` partition, use this pair instead:

```sh
PLAN="$WORK/device-replace-plan.json"
./project.sh device disk plan --replace-existing \
  --device "$DEVICE" --image-report "$WORK/output/ios.apfs.json" \
  --output "$PLAN"
cat "$PLAN"

./project.sh device install --replace-existing \
  --device "$DEVICE" --plan "$PLAN" \
  --image "$WORK/output/ios.apfs" --backup "$WORK/device-replace-backup"
```

The image must fit the existing partition. Add `--resumable` to the installation
command to allow USB reconnects during replacement without resizing.

After either installation path:

```sh
./project.sh device disk verify-device-hash \
  --device "$DEVICE" --plan "$PLAN" \
  --output "$WORK/device-hash.json"
./project.sh device storage flush --no-uart --device "$DEVICE"
```

The exporter has no USB reboot command. After flushing, restart using the
power button, enter stock Fastboot, and flash the matching combined image:

```sh
./project.sh device flash --image "$WORK/output/boot.img" --boot
```

## Validation

The default console transport is USB CDC, alongside the native Apple DeviceMux
interface. No direct UART connection is needed for logs or a running shell.
Start capture before boot to receive the buffered bridge and early kernel logs:

```sh
./project.sh device console --output build/usb-console.log
./project.sh device console --output build/usb-commands.log --seconds 120 \
  --require-bash 'printf "uid=%s bash=%s\n" "$EUID" "$BASH_VERSION"' 'mount'
./project.sh device console --output build/usb-interactive.log --interactive
```

The console discovers the device automatically; use `--serial` when several
devices are connected. Command batches wait for a unique completion marker and
stop on a nonzero result. A disconnected or timed-out command is not retried,
because its execution state may be unknown. `--wait` sets each command's timeout;
`--seconds` bounds enumeration and the entire batch (120 seconds by default
for commands). `--report PATH` saves machine-readable results. Use Ctrl-] to
leave an interactive session.

USB becomes available after the kernel USB stack starts. Earlier output is
replayed from bounded RAM. Boot failures before USB initialization, logs before
bridge entry, and panic-stop/KDP debugging are outside this transport's coverage.
Optional direct UART access requires `device console --uart --port PATH`.

Driver build and verification commands use the selected profile's collection
dependency closure. Use `--driver NAME` for an explicit component selection.

```sh
./project.sh verify kernel
./project.sh verify drivers
./project.sh verify kernelcache
./project.sh verify firmware
```

These checks validate build outputs and filesystem contents. Hardware support
is listed separately in [Device support](#device-support).

## References and licenses

Boot image packaging draws on [UniXNU bootimg](https://github.com/UniXNU/UniXNU/blob/141d16a43bfe46859487f32c0519c428cc473a49/scripts/unixnu/bootimg.py)
and its [ramdisk payload design](https://github.com/UniXNU/UniXNU-sheng/blob/251b291adf941b9953fe3ae66a808b9b626e164d/scripts/package-sheng-xnu-boot.py).
The port uses XnuBridge for Mi 6 hardware handoff.

XNU, EDK2, Linux-derived components, and other third-party code retain their
respective licenses and attribution. Dependency versions and patches are under
`tools/`; driver notices are included in their module directories.
