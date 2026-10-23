.DEFAULT_GOAL := all
PYTHON ?= python3
JOBS ?= 3
TARGET ?= SDM835
CPUS ?= 4
KERNELCACHE ?= build/ios18-kext-analysis/kernelcache.release.ipad7c.macho
KEXT_SET ?= docs/sdm835/ios18/stages/02-platform
TARGET_NAME = $(if $(filter SM8150,$(TARGET)),sm8150,$(if $(filter VMAPPLE,$(TARGET)),vmapple,sdm835))
KERNELCACHE_OUTPUT ?= build/output/kernelcache.$(TARGET_NAME).$(COLLECTION)
COLLECTION ?= platform
TEST_FLAGS ?=
DRIVER_FLAGS ?=
BOOT_FLAGS ?=
BRIDGE_EFI ?= build/bridge/XnuBridge.efi
AUTOBOOT ?= 1
.PHONY: edk2-sagit boot-image test-fastboot
edk2-sagit:
	bash scripts/build-sagit.sh
boot-image:
	$(PYTHON) scripts/build-boot.py --bridge "$(BRIDGE_EFI)" $(if $(filter 1,$(AUTOBOOT)),--autoboot,--no-autoboot) $(BOOT_FLAGS)
test-fastboot:
	$(PYTHON) -m unittest discover -s tests -p 'test_fastboot*.py'
.PHONY: all bootstrap build verify doctor package test collect-sdm835 kernelcache-patcher test-patcher analyze-kexts analyze-abi kernelcache verify-kernelcache
all: build
.PHONY: analyze-baseband analyze-baseband-runtime prepare-baseband baseband-driver test-baseband test-baseband-sanitized baseband verify-baseband
BASEBAND_DRIVER_OUTPUT ?= build/baseband/Extensions
analyze-baseband:
	$(PYTHON) scripts/toolchain.py analyze-baseband --kernelcache "$(KERNELCACHE)"
analyze-baseband-runtime:
	$(PYTHON) scripts/baseband_dyld.py
	$(PYTHON) scripts/baseband_apple_abi.py
	$(PYTHON) scripts/baseband_apple_notifications.py
prepare-baseband:
	$(PYTHON) scripts/toolchain.py prepare-baseband
baseband-driver:
	$(PYTHON) scripts/toolchain.py build-drivers --target $(TARGET) --driver QcomBaseband --driver QcomSCM --driver QcomSMEM --driver GenericClockController --driver QcomPowerController --driver QcomGCC --driver QcomRPM --driver-output "$(BASEBAND_DRIVER_OUTPUT)" -j $(JOBS) --kernelcache "$(KERNELCACHE)"
test-baseband:
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test baseband-firmware --test baseband-transport --test baseband-q6 --test baseband-glink --test baseband-boot --test baseband-qmi --test baseband-smem-pool --test baseband-signals --test baseband-memory --test baseband-admission --test baseband-axi --test baseband-rpm --test baseband-resources --test baseband-rmtfs --test baseband-rmtfs-storage --test baseband-mux --test baseband-sysmon --test baseband-shutdown --test baseband-ssr --test baseband-services --test baseband-tftp
	$(PYTHON) -m unittest discover -s tests -p 'test_baseband_integration.py'
	$(PYTHON) -m unittest discover -s tests -p 'test_smem_handoff.py'
	$(PYTHON) -m unittest discover -s tests -p 'test_baseband_handoff.py'
	$(PYTHON) -m unittest discover -s tests -p 'test_baseband_stock.py'
	$(PYTHON) -m unittest discover -s tests -p 'test_baseband_apple_notifications.py'
test-baseband-sanitized:
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test baseband-firmware --test baseband-transport --test baseband-q6 --test baseband-glink --test baseband-boot --test baseband-qmi --test baseband-smem-pool --test baseband-signals --test baseband-memory --test baseband-admission --test baseband-axi --test baseband-rpm --test baseband-resources --test baseband-rmtfs --test baseband-rmtfs-storage --test baseband-mux --test baseband-sysmon --test baseband-shutdown --test baseband-ssr --test baseband-services --test baseband-tftp --sanitize
baseband:
	$(PYTHON) scripts/toolchain.py pack-drivers --target $(TARGET) --collection baseband --driver-output "$(BASEBAND_DRIVER_OUTPUT)" -j $(JOBS) --kernelcache "$(KERNELCACHE)"
verify-baseband:
	$(PYTHON) scripts/toolchain.py verify-collection --target $(TARGET) --collection baseband --driver-output "$(BASEBAND_DRIVER_OUTPUT)"
.PHONY: multicore-drivers multicore-bridge multicore test-multicore
multicore-drivers:
	$(PYTHON) scripts/toolchain.py build-drivers --target $(TARGET) --driver GenericARMPlatform --driver $(TARGET) --driver AppleARMGIC -j $(JOBS) --kernelcache "$(KERNELCACHE)"
test-multicore:
	$(PYTHON) scripts/toolchain.py test-platform --target $(TARGET)
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test arm-smp --test arm-platform-config --test gicv3
	$(PYTHON) -m unittest discover -s tests -p 'test_boot_bridge.py'
	$(PYTHON) -m unittest discover -s tests -p 'test_userland_evidence.py'
multicore: build test-multicore
	$(PYTHON) scripts/toolchain.py pack-drivers --target $(TARGET) -j $(JOBS) --collection apfs --output build/multicore/kernelcache --kernelcache "$(KERNELCACHE)"
multicore-bridge:
	$(PYTHON) scripts/build-bridge.py --kernel build/multicore/kernelcache --cpus $(CPUS) --output build/multicore/bridge-$(CPUS) $(BRIDGE_FLAGS)
.PHONY: drivers verify-drivers test-drivers
drivers:
	$(PYTHON) scripts/toolchain.py build-drivers --target $(TARGET) -j $(JOBS) --kernelcache "$(KERNELCACHE)" $(DRIVER_FLAGS)
verify-drivers:
	$(PYTHON) scripts/toolchain.py verify-drivers --target $(TARGET) --kernelcache "$(KERNELCACHE)" $(DRIVER_FLAGS)
test-drivers:
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) $(TEST_FLAGS)
.PHONY: spi-drivers gyro-drivers gyro-client test-gyro test-gyro-sanitized gyro verify-gyro
spi-drivers:
	$(PYTHON) scripts/toolchain.py build-drivers --target $(TARGET) --driver GenericSPI --driver QcomSPI -j $(JOBS) --kernelcache "$(KERNELCACHE)"
gyro-drivers:
	$(PYTHON) scripts/toolchain.py build-drivers --target $(TARGET) --driver GenericSPI --driver QcomSPI --driver SPIGyro -j $(JOBS) --kernelcache "$(KERNELCACHE)"
gyro-client:
	$(PYTHON) scripts/toolchain.py build-gyro-client --target $(TARGET)
test-gyro:
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test spi --test gyro
	$(PYTHON) -m unittest discover -s tests -p 'test_gyro_integration.py'
test-gyro-sanitized:
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test spi --test gyro --sanitize
gyro: gyro-client test-gyro
	$(PYTHON) scripts/toolchain.py pack-drivers --target $(TARGET) --collection gyro -j $(JOBS) --kernelcache "$(KERNELCACHE)"
verify-gyro:
	$(PYTHON) scripts/toolchain.py verify-collection --target $(TARGET) --collection gyro
.PHONY: battery-driver battery-client test-battery test-battery-sanitized battery verify-battery
battery-driver:
	$(PYTHON) scripts/toolchain.py build-drivers --target $(TARGET) --driver GenericClockController --driver QcomPowerController --driver QcomSPMI --driver QcomBattery -j $(JOBS) --kernelcache "$(KERNELCACHE)"
battery-client:
	$(PYTHON) scripts/toolchain.py build-battery-client --target $(TARGET)
test-battery: battery-client
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test battery --test battery-runtime
	$(PYTHON) -m unittest discover -s tests -p 'test_battery_integration.py'
test-battery-sanitized:
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test battery --test battery-runtime --sanitize
battery: battery-client test-battery
	$(PYTHON) scripts/toolchain.py pack-drivers --target $(TARGET) --collection battery -j $(JOBS) --kernelcache "$(KERNELCACHE)"
verify-battery:
	$(PYTHON) scripts/toolchain.py verify-collection --target $(TARGET) --collection battery
.PHONY: analyze-haptics haptics-driver haptics-client test-haptics test-haptics-sanitized haptics verify-haptics
analyze-haptics:
	$(PYTHON) scripts/analyze-haptics.py
haptics-driver:
	$(PYTHON) scripts/toolchain.py build-drivers --target $(TARGET) --driver GenericClockController --driver QcomPowerController --driver QcomSPMI --driver PMI8998Haptics -j $(JOBS) --kernelcache "$(KERNELCACHE)"
haptics-client:
	$(PYTHON) scripts/toolchain.py build-haptics-client --target $(TARGET)
test-haptics: haptics-client
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test haptics --test haptics-runtime
	$(PYTHON) -m unittest discover -s tests -p 'test_haptics_integration.py'
test-haptics-sanitized:
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test haptics --test haptics-runtime --sanitize
haptics: haptics-client test-haptics
	$(PYTHON) scripts/toolchain.py pack-drivers --target $(TARGET) --collection haptics -j $(JOBS) --kernelcache "$(KERNELCACHE)"
verify-haptics:
	$(PYTHON) scripts/toolchain.py verify-collection --target $(TARGET) --collection haptics
.PHONY: wifi-driver wifi-client wifi-driverkit prepare-wifi analyze-wifi test-wifi test-wifi-sanitized wifi verify-wifi
wifi-driver:
	$(PYTHON) scripts/toolchain.py build-drivers --target $(TARGET) --driver QcomWLAN -j $(JOBS) --kernelcache "$(KERNELCACHE)"
wifi-client:
	$(PYTHON) scripts/toolchain.py build-wifi-client --target $(TARGET)
prepare-wifi:
	$(PYTHON) scripts/toolchain.py prepare-wifi --target $(TARGET)
wifi-driverkit:
	$(PYTHON) scripts/toolchain.py build-wifi-driverkit --target $(TARGET)
analyze-wifi:
	$(PYTHON) scripts/analyze-kernelcache.py "$(KERNELCACHE)" --output build/wifi/native-model
test-wifi:
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test wlan --test wlan-firmware --test wlan-runtime
	$(PYTHON) -m unittest discover -s tests -p 'test_wlan_integration.py'
	$(PYTHON) -m unittest discover -s tests -p 'test_boot_bridge.py'
test-wifi-sanitized:
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test wlan --test wlan-firmware --test wlan-runtime --sanitize
wifi: wifi-client wifi-driverkit test-wifi
	$(PYTHON) scripts/toolchain.py pack-drivers --target $(TARGET) --collection wifi -j $(JOBS) --kernelcache "$(KERNELCACHE)"
verify-wifi:
	$(PYTHON) scripts/toolchain.py verify-collection --target $(TARGET) --collection wifi
	$(PYTHON) scripts/toolchain.py verify-wifi-driverkit --target $(TARGET)
.PHONY: bluetooth bluetooth-driver bluetooth-client bluetooth-stack prepare-bluetooth analyze-bluetooth test-bluetooth test-bluetooth-sanitized verify-bluetooth
BLUETOOTH_FLAGS ?=
prepare-bluetooth:
	$(PYTHON) scripts/toolchain.py prepare-bluetooth $(BLUETOOTH_FLAGS)
analyze-bluetooth:
	$(PYTHON) scripts/analyze-kernelcache.py "$(KERNELCACHE)" --output build/bluetooth/native-model
bluetooth-driver:
	$(PYTHON) scripts/toolchain.py build-drivers --target $(TARGET) --driver QcomBluetooth -j $(JOBS) --kernelcache "$(KERNELCACHE)"
bluetooth-client:
	$(PYTHON) scripts/toolchain.py build-bluetooth-client --target $(TARGET)
bluetooth-stack: bluetooth-client
	$(PYTHON) scripts/toolchain.py prepare-bluetooth-stack --target $(TARGET)
test-bluetooth:
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test gcc --test gpio --test bluetooth --test bluetooth-firmware --test bluetooth-runtime --test bluetooth-bootstrap
	$(PYTHON) -m unittest discover -s tests -p 'test_bluetooth_integration.py'
	$(PYTHON) -m unittest discover -s tests -p 'test_boot_bridge.py' -k bluetooth
test-bluetooth-sanitized:
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test gcc --test gpio --test bluetooth --test bluetooth-firmware --test bluetooth-runtime --test bluetooth-bootstrap --sanitize
bluetooth: bluetooth-stack
	$(PYTHON) scripts/toolchain.py pack-drivers --target $(TARGET) --collection bluetooth -j $(JOBS) --kernelcache "$(KERNELCACHE)"
verify-bluetooth:
	$(PYTHON) scripts/toolchain.py verify-collection --target $(TARGET) --collection bluetooth
	$(PYTHON) scripts/toolchain.py verify-bluetooth-stack --target $(TARGET)
.PHONY: usb-driver verify-usb-driver test-usb test-usb-sanitize test-usb-tools usb-kernelcache usb-bridge verify-usb analyze-usb-resources
USB_RESOURCE_PROFILE ?=
USB_BRIDGE_OUTPUT ?= build/usb/bridge
USB_FIRMWARE_DIRECTORY ?= build/usb/firmware/stock
USB_RESOURCE_EVIDENCE ?= build/usb/firmware-resource-extraction.json
USB_DRIVERS = --driver QcomGCC --driver QcomRPM --driver GenericClockController --driver QcomPowerController --driver QcomSPMI --driver QcomPMIC --driver QcomInterconnect --driver QcomUSB
USB_TESTS = --test usb-cdc --test usb-console --test gcc --test power --test usb --test xhci --test dwc3-transfer --test dwc3-control --test dwc3-iso --test usb-dma --test usb-requests --test usb-phy --test usb-role --test usb-connector --test usb-resources --test usb-interconnect --test usb-smmu --test usb-smmu-context --test usb-iort
usb-driver:
	$(PYTHON) scripts/toolchain.py build-drivers --target $(TARGET) $(USB_DRIVERS) --driver-output build/usb/Extensions -j $(JOBS) --kernelcache "$(KERNELCACHE)"
.PHONY: usb-client
usb-client:
	$(PYTHON) scripts/toolchain.py build-usb-client --target $(TARGET)
verify-usb-driver:
	$(PYTHON) scripts/toolchain.py verify-drivers --target $(TARGET) $(USB_DRIVERS) --driver-output build/usb/Extensions --kernelcache "$(KERNELCACHE)"
test-usb: usb-driver
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) $(USB_TESTS)
	$(MAKE) test-usb-tools
test-usb-sanitize: usb-driver
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) $(USB_TESTS) --sanitize
	$(MAKE) test-usb-tools
test-usb-tools:
	$(PYTHON) -m unittest discover -s tests -p 'test_boot_bridge.py'
	$(PYTHON) -m unittest discover -s tests -p 'test_driver_toolchain.py'
usb-kernelcache:
	$(PYTHON) scripts/toolchain.py pack-drivers --target $(TARGET) --collection usb -j $(JOBS) --kernelcache "$(KERNELCACHE)"
usb-bridge: usb-kernelcache
	$(MAKE) test-usb-tools
	$(PYTHON) scripts/build-bridge.py --kernel build/output/kernelcache.usb $(if $(strip $(USB_RESOURCE_PROFILE)),--usb-resource-profile "$(USB_RESOURCE_PROFILE)",--usb-topology-component '\_SB_.USB0' --usb-hardware-stop) --output "$(USB_BRIDGE_OUTPUT)"
verify-usb:
	$(PYTHON) scripts/toolchain.py verify-collection --target $(TARGET) --collection usb
analyze-usb-resources:
	$(PYTHON) scripts/toolchain.py analyze-usb-resources --firmware-directory "$(USB_FIRMWARE_DIRECTORY)" --output "$(USB_RESOURCE_EVIDENCE)"
bootstrap:
	$(PYTHON) scripts/toolchain.py bootstrap --target $(TARGET) -j $(JOBS)
build:
	$(PYTHON) scripts/toolchain.py build --target $(TARGET) -j $(JOBS)
verify:
	$(PYTHON) scripts/toolchain.py verify --target $(TARGET)
doctor:
	$(PYTHON) scripts/toolchain.py doctor --target $(TARGET)
package:
	$(PYTHON) scripts/toolchain.py package --target $(TARGET)
test:
	$(PYTHON) scripts/toolchain.py test --target $(TARGET)
.PHONY: test-iig
test-iig:
	$(PYTHON) -m unittest discover -s tests -p 'test_iig_compat.py'
collect-sdm835:
	$(PYTHON) scripts/toolchain.py collect-sdm835
kernelcache-patcher:
	cmake -S tools/kernelcache_patcher -B build/kernelcache_patcher -DCMAKE_BUILD_TYPE=Release
	cmake --build build/kernelcache_patcher -j $(JOBS)
test-patcher: kernelcache-patcher
	ctest --test-dir build/kernelcache_patcher --output-on-failure
	$(PYTHON) -m unittest discover -s tests -p 'test_kernelcache*.py'
analyze-kexts:
	$(PYTHON) scripts/analyze-kernelcache.py "$(KERNELCACHE)" --output docs/sdm835/ios18
analyze-abi: kernelcache-patcher
	build/kernelcache_patcher/kernelcache_patcher analyze build/output/mach.development.sdm835.unstripped "$(KERNELCACHE)" --native-only
kernelcache: test-patcher
	$(PYTHON) scripts/toolchain.py pack-drivers --target $(TARGET) -j $(JOBS) --collection "$(COLLECTION)" --kernelcache "$(KERNELCACHE)" --base-manifest "$(KEXT_SET)/kexts.txt" --output "$(KERNELCACHE_OUTPUT)"
verify-kernelcache:
	$(PYTHON) scripts/toolchain.py verify-collection --target $(TARGET) --collection "$(COLLECTION)" --output "$(KERNELCACHE_OUTPUT)"

.PHONY: sm8150 test-sm8150 sm8150-drivers sm8150-platform sm8150-bridge test-usb-cdc
SM8150_KERNEL ?= build/output/kernelcache.sm8150.platform
sm8150:
	$(MAKE) build verify TARGET=SM8150
test-sm8150:
	$(PYTHON) scripts/toolchain.py test-platform --target SM8150
	$(PYTHON) -m unittest discover -s tests -p 'test_sm8150_integration.py'
sm8150-drivers:
	$(PYTHON) scripts/toolchain.py build-drivers --target SM8150 --driver GenericARMPlatform --driver SM8150 --driver AppleARMGIC --driver-output build/sm8150/platform-Extensions -j $(JOBS) --kernelcache "$(KERNELCACHE)"
sm8150-platform:
	$(PYTHON) scripts/toolchain.py pack-drivers --target SM8150 --collection platform --driver-output build/sm8150/platform-Extensions -j $(JOBS) --kernelcache "$(KERNELCACHE)"
sm8150-bridge:
	$(PYTHON) scripts/build-bridge.py --dts device_trees/sm8150-generic.dts --kernel "$(SM8150_KERNEL)" --output build/sm8150/bridge --watchdog-seconds 0 $(BRIDGE_ARGS)
test-usb-cdc:
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test usb-cdc --test usb-console $(TEST_FLAGS)

.PHONY: power power-security verify-power analyze-power
.PHONY: display-drivers display test-display verify-display
display-drivers:
	$(PYTHON) scripts/toolchain.py build-drivers --target $(TARGET) -j $(JOBS) --driver GenericClockController --driver QcomPowerController --driver QcomSPMI --driver PMI8998WLED --driver QcomMIPIDSI --driver QcomDisplay --kernelcache "$(KERNELCACHE)"
test-display:
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test display --test mipi-dsi
	$(PYTHON) -m unittest discover -s tests -p 'test_boot_bridge.py' -k display
display:
	$(PYTHON) scripts/toolchain.py pack-drivers --target $(TARGET) -j $(JOBS) --collection display --kernelcache "$(KERNELCACHE)"
verify-display:
	$(PYTHON) scripts/toolchain.py verify-collection --target $(TARGET) --collection display

power:
	$(PYTHON) scripts/toolchain.py pack-drivers --target $(TARGET) -j $(JOBS) --collection power --kernelcache "$(KERNELCACHE)"
power-security:
	$(PYTHON) scripts/toolchain.py pack-drivers --target $(TARGET) -j $(JOBS) --collection power-security --kernelcache "$(KERNELCACHE)"
verify-power:
	$(PYTHON) scripts/toolchain.py verify-collection --collection power
analyze-power:
	$(PYTHON) scripts/analyze-kernelcache.py "$(KERNELCACHE)" --output docs/power/ios18 --audit-dependency com.apple.driver.ApplePMGR

.PHONY: bridge test-bridge test-bridge-device
BRIDGE_ARGS ?=
BRIDGE_TEST_ARGS ?=
bridge:
	$(PYTHON) scripts/build-bridge.py $(BRIDGE_ARGS)
test-bridge:
	$(PYTHON) -m unittest discover -s tests -p test_boot_bridge.py -v
	$(PYTHON) -m unittest discover -s tests -p test_bridge_hactivation.py -v
	$(PYTHON) -m unittest discover -s tests -p test_bridge_nvram.py -v
	$(PYTHON) -m unittest discover -s tests -p test_bridge_qemu.py -v
	$(PYTHON) -m unittest discover -s tests -p test_panic_backend.py -v
.PHONY: analyze-device-identity
analyze-device-identity:
	$(PYTHON) scripts/analyze-device-identity.py
test-bridge-device:
	$(PYTHON) scripts/test-bridge.py --iterations 2 $(BRIDGE_TEST_ARGS)

.PHONY: test-armv8
test-armv8: test-patcher
	$(PYTHON) scripts/test-armv8-compat.py

.PHONY: collect-power power-client
collect-power:
	$(PYTHON) scripts/toolchain.py collect-power
power-client:
	$(PYTHON) scripts/toolchain.py build-power-client

.PHONY: gpu gpu-driver gpu-client gpu-metal gpu-shaders test-gpu verify-gpu
gpu:
	$(PYTHON) scripts/toolchain.py pack-drivers --target $(TARGET) -j $(JOBS) --collection gpu --kernelcache "$(KERNELCACHE)"
gpu-driver:
	$(PYTHON) scripts/toolchain.py build-drivers --target $(TARGET) --driver QcomGPU -j $(JOBS) --kernelcache "$(KERNELCACHE)"
gpu-client:
	$(PYTHON) scripts/toolchain.py build-gpu-client
gpu-metal:
	$(PYTHON) scripts/toolchain.py build-gpu-metal
gpu-shaders:
	$(PYTHON) scripts/toolchain.py build-gpu-shaders
test-gpu: gpu-driver test-drivers gpu-client gpu-metal
	$(PYTHON) -m unittest discover -s tests -p test_gpu_integration.py -v
verify-gpu:
	$(PYTHON) scripts/toolchain.py verify-collection --collection gpu

.PHONY: apfs-kernelcache verify-apfs test-apfs
apfs-kernelcache:
	$(PYTHON) scripts/toolchain.py pack-drivers --target $(TARGET) -j $(JOBS) --collection apfs --kernelcache "$(KERNELCACHE)"
verify-apfs:
	$(PYTHON) scripts/toolchain.py verify-collection --collection apfs

.PHONY: input input-runtime input-drivers input-client test-input verify-input prepare-input
input-client:
	$(PYTHON) scripts/toolchain.py build-input-client --target $(TARGET)
prepare-input:
	$(PYTHON) scripts/toolchain.py prepare-input
input:
	$(PYTHON) scripts/toolchain.py pack-drivers --target $(TARGET) -j $(JOBS) --collection input --kernelcache "$(KERNELCACHE)" $(INPUT_FLAGS)
input-runtime:
	$(PYTHON) scripts/toolchain.py pack-drivers --target $(TARGET) -j $(JOBS) --collection input-runtime --kernelcache "$(KERNELCACHE)" $(INPUT_FLAGS)
input-drivers:
	$(PYTHON) scripts/toolchain.py build-drivers --target $(TARGET) -j $(JOBS) --driver QcomGPIO --driver QcomSPMI --driver QcomPMIC --driver QcomButtons --driver FingerprintHID --driver GoodixFingerprint --driver FPCFingerprint --driver QcomGCC --driver QcomSCM --driver GenericClockController --driver QcomPowerController --driver GenericI2C --driver QcomI2C --driver SynapticsTouch --kernelcache "$(KERNELCACHE)"
test-input:
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test gpio --test pmic-input --test buttons --test fingerprint-home --test i2c --test touch
	$(PYTHON) -m unittest discover -s tests -p 'test_input_integration.py'
	$(PYTHON) -m unittest discover -s tests -p 'test_touch_integration.py'
	$(PYTHON) -m unittest discover -s tests -p 'test_input_firmware.py'
	$(PYTHON) -m unittest discover -s tests -p 'test_boot_bridge.py'
	$(PYTHON) -m unittest discover -s tests -p 'test_driver_toolchain.py'
verify-input:
	$(PYTHON) scripts/toolchain.py verify-collection --collection input

.PHONY: touch-drivers test-touch test-touch-sanitized touch verify-touch
touch-drivers:
	$(PYTHON) scripts/toolchain.py build-drivers --target $(TARGET) -j $(JOBS) --driver GenericI2C --driver QcomI2C --driver SynapticsTouch --driver QcomGPIO --driver QcomGCC --driver GenericClockController --kernelcache "$(KERNELCACHE)"
test-touch:
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test i2c --test touch
	$(PYTHON) -m unittest discover -s tests -p 'test_touch_integration.py'
test-touch-sanitized:
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test i2c --test touch --sanitize
touch:
	$(PYTHON) scripts/toolchain.py pack-drivers --target $(TARGET) -j $(JOBS) --collection touch --kernelcache "$(KERNELCACHE)"
verify-touch:
	$(PYTHON) scripts/toolchain.py verify-collection --target $(TARGET) --collection touch
test-apfs:
	$(PYTHON) -m unittest discover -s tests -p 'test_apfs*.py' -v
	tools/prefix/libexec/go/bin/go -C tools/src/go-apfs test ./pkg/apfswrite
	tools/prefix/libexec/go/bin/go -C tools/apfs-image test -v .

.PHONY: usb-storage test-usb-storage
usb-storage:
	$(PYTHON) scripts/build-usb-storage.py build -j $(JOBS)
test-usb-storage:
	mkdir -p build/usb-storage
	$(CC) -std=c11 -Wall -Wextra -Werror tests/storage_serial_test.c -o build/usb-storage/storage_serial_test
	build/usb-storage/storage_serial_test
	$(PYTHON) -m unittest discover -s tests -p test_usb_storage.py -v

.PHONY: perfcontrol-drivers test-perfcontrol
perfcontrol-drivers:
	$(PYTHON) scripts/toolchain.py build-drivers --target $(TARGET) -j $(JOBS) --driver GenericClockController --driver QcomPowerController --driver QcomOSM --kernelcache "$(KERNELCACHE)"
test-perfcontrol: perfcontrol-drivers power-client
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test cpu-performance --test power --test power-clock
	$(PYTHON) -m unittest discover -s tests -p test_perfcontrol_integration.py -v
	$(PYTHON) -m unittest discover -s tests -p test_power_integration.py -v

.PHONY: test-osm test-osm-sanitized osm-bridge
OSM_TESTS = --test cprh-initialization --test osm-initialization --test osm-handoff --test osm-firmware --test power --test cpu-performance
test-osm:
	$(PYTHON) scripts/generate-cprh-tables.py --check
	$(PYTHON) scripts/test-cprh-calibration.py
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) $(OSM_TESTS)
	$(PYTHON) -m unittest discover -s tests -p test_osm_handoff.py -v
	$(PYTHON) -m unittest discover -s tests -p test_osm_tz_audit.py -v
test-osm-sanitized:
	$(PYTHON) scripts/test-cprh-calibration.py --sanitize
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) $(OSM_TESTS) --sanitize
osm-bridge:
	$(PYTHON) scripts/build-bridge.py --kernel "$(KERNELCACHE_OUTPUT)" --cpu-osm-handoff --cpus 8 --output build/osm-bringup/bridge $(BRIDGE_FLAGS)

.PHONY: audio-driver test-audio test-audio-sanitized audio verify-audio
audio-driver:
	$(PYTHON) scripts/toolchain.py build-drivers --target $(TARGET) -j $(JOBS) --driver QcomAudio --driver QcomSMEM --kernelcache "$(KERNELCACHE)"
test-audio:
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test audio-output --test audio-adsp --test baseband-smem-pool
	$(PYTHON) -m unittest discover -s tests -p test_audio_integration.py -v
	$(PYTHON) -m unittest discover -s tests -p test_adsp_integration.py -v
test-audio-sanitized:
	$(PYTHON) scripts/toolchain.py test-drivers --target $(TARGET) --test audio-output --test audio-adsp --test baseband-smem-pool --sanitize
audio:
	$(PYTHON) scripts/toolchain.py pack-drivers --target $(TARGET) -j $(JOBS) --collection audio --kernelcache "$(KERNELCACHE)"
verify-audio:
	$(PYTHON) scripts/toolchain.py verify-collection --target $(TARGET) --collection audio

.PHONY: edk2-judypn test-edk2-judypn verify-edk2-judypn
edk2-judypn:
	JOBS=$(JOBS) bash scripts/build-judypn.sh
test-edk2-judypn:
	bash scripts/test-edk2-judypn.sh
verify-edk2-judypn:
	$(PYTHON) scripts/verify-edk2-judypn.py
