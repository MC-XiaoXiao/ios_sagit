#!/usr/bin/env python3
"""Internal command dispatch and compiler multicall entry."""
import sys
from pathlib import Path
if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import argparse
import json
import os
from pathlib import Path
import re
import sys
from project.build import context as build_state


def main():
    from project.build import clients as _component_clients, drivers as _component_drivers, gpu as _component_gpu, kernel as _component_kernel, native as _component_native, radio as _component_radio, resources as _component_resources, services as _component_services
    name = Path(sys.argv[0]).name
    if name != 'toolchain.py':
        _component_native.shim(name, sys.argv[1:])
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['analyze-baseband', 'prepare-baseband', 'bootstrap', 'build', 'verify', 'doctor', 'package', 'collect-sdm835',
                                          'build-drivers', 'verify-drivers', 'pack-drivers', 'verify-collection', 'collect-power', 'build-power-client', 'build-usb-client', 'build-battery-client', 'build-haptics-client', 'build-input-client', 'build-wifi-client', 'prepare-wifi', 'build-wifi-driverkit', 'verify-wifi-driverkit', 'build-bluetooth-client', 'prepare-bluetooth', 'prepare-bluetooth-stack', 'verify-bluetooth-stack', 'build-display-client', 'build-wallpaper-client', 'build-gyro-client', 'prepare-display-services', 'prepare-keybag-policy', 'verify-display-readback', 'build-gpu-client', 'build-gpu-metal', 'build-gpu-shaders', 'prepare-input', 'analyze-usb-resources'])
    parser.add_argument('-j', '--jobs', type=int, default=build_state.JOBS)
    parser.add_argument('--target', choices=['SDM835', 'SM8150', 'VMAPPLE'], default=build_state.TARGET)
    parser.add_argument('--kernelcache', '--provider-image', type=Path,
                        help='external provider image for the configured ABI contracts')
    parser.add_argument('--drivers-root', type=Path, default=build_state.DRIVERS_ROOT,
                        help='Kext source root; discovers child directories containing Info.plist')
    parser.add_argument('--driver', action='append', default=[],
                        help='select a source directory, executable name or bundle identifier; repeatable')
    parser.add_argument('--driver-output', type=Path, default=build_state.DRIVER_OUTPUT,
                        help='destination directory for compiled Kext bundles')
    parser.add_argument('--collection', help='collection name declared in the driver build configuration')
    parser.add_argument('--output', type=Path, help='override the configured collection output path')
    parser.add_argument('--local-kernel', type=Path,
                        help='unstripped local kernel for pack-drivers; snapshot this exact build')
    parser.add_argument('--capture-prefix', type=Path, help='host prefix of exported displayctl validate-scanout files')
    parser.add_argument('--migration-service', type=Path, help='original pinned iOS18 datamigrator executable')
    parser.add_argument('--system-app-migrator', type=Path, help='original pinned iOS18 system-app migration bundle executable')
    parser.add_argument('--springboard-migrator', type=Path, help='original pinned iOS18 SpringBoard migration bundle executable')
    parser.add_argument('--service-resource-fork', '--migration-resource-fork', dest='migration_resource_fork', type=Path,
                        help='optional original zlib resource fork for an allocation-preserving service update')
    parser.add_argument('--service-decmpfs', '--migration-decmpfs', dest='migration_decmpfs', type=Path,
                        help='original compression attribute accompanying --service-resource-fork')
    parser.add_argument('--development-skip-migration-unlock', action='store_true',
                        help='prepare a reversible native-service derivative that skips migration unlock on negative MKB query results')
    parser.add_argument('--development-fail-fast-system-app-installs', action='store_true',
                        help='fail missing-app placeholders while preserving the native install-group deadline and failure paths')
    parser.add_argument('--development-defer-launch-images', action='store_true',
                        help='defer native launch-image discovery and capture through the original post-migration request')
    parser.add_argument('--fairplay-launchd-plist', type=Path, help='original pinned iOS18 FairPlay launchd plist')
    parser.add_argument('--development-no-sep-service-readiness', action='store_true',
                        help='reset failed FairPlay Mach endpoints at close on development devices without SEP')
    parser.add_argument('--system-cryptex', type=Path, help='original raw SystemOS Cryptex for the pinned public MKB policy')
    parser.add_argument('--development-no-sep', action='store_true',
                        help='explicitly prepare public MKB data availability for unprotected Data without SEP')
    parser.add_argument('--launchd-cache', type=Path, help='current iOS18 cached launchd configuration')
    parser.add_argument('--launchd-cache-signature', type=Path, help='matching engineering detached launchd cache signature')
    parser.add_argument('--base-manifest', type=Path, help='additional bundle IDs from a kexts.txt manifest')
    parser.add_argument('--firmware-archive', type=Path, help='pinned input firmware archive for prepare-input')
    parser.add_argument('--firmware-directory', type=Path,
                        help='local stock firmware directory for offline USB resource extraction')
    parser.add_argument('--firmware-profile', type=Path, help='explicit controller identity, asset hashes and board policy')
    parser.add_argument('--bluetoothd-plist', type=Path, help='optional hash-pinned native launchd plist; prepare an offline H4/POSIX transport configuration')
    parser.add_argument('--bluetooth-reference',type=Path,default=build_state.BUILD/'obj/bluetooth/reference',help='hash-pinned native BlueTool/bluetoothd and launchd inputs')
    args = parser.parse_args()
    build_state.TARGET = args.target
    build_state.CONFIG = build_state.OBJ / ('DEVELOPMENT_ARM64_' + build_state.TARGET)
    build_state.KERNEL_FILE = 'mach.development.' + build_state.TARGET.lower()
    build_state.NATIVE_KERNELCACHE = args.kernelcache.resolve() if args.kernelcache else None
    build_state.DRIVERS_ROOT = args.drivers_root.resolve()
    build_state.DRIVER_FILTERS = args.driver
    build_state.DRIVER_OUTPUT = args.driver_output.resolve()
    if args.jobs < 1:
        parser.error('jobs must be positive')
    if args.command == 'bootstrap':
        _component_native.bootstrap(args.jobs)
    elif args.command in ('analyze-baseband', 'prepare-baseband'):
        from project.firmware import baseband as baseband
        report = (baseband.analyze(build_state.ROOT, build_state.NATIVE_KERNELCACHE, args.output)
                  if args.command == 'analyze-baseband' else baseband.prepare(build_state.ROOT, args.output))
        print(json.dumps(report, indent=2))
    elif args.command == 'doctor':
        print(f'Compiler: {_component_native.compiler()}')
        print(f'Host: {os.uname().machine}; SDK: {build_state.SDK}')
        for name in ('make', 'bison', 'flex', 'm4', 'curl', 'patch', 'perl', 'bash', 'sed', 'awk', 'install'):
            print(f'{name}: {_component_native.host(name)}')
    elif args.command == 'build':
        _component_kernel.build_kernel(args.jobs)
    elif args.command == 'package':
        _component_kernel.package()
    elif args.command == 'collect-sdm835':
        _component_kernel.collect_sdm835()
    elif args.command == 'collect-power':
        _component_resources.collect_power_clocks()
    elif args.command == 'analyze-usb-resources':
        if not args.firmware_directory or not args.output:
            parser.error('analyze-usb-resources requires --firmware-directory and --output')
        result = _component_resources.analyze_usb_resources(args.firmware_directory, args.output)
        print(json.dumps({'output': str(args.output.resolve()),
            'SMMU_devices': len(result['hyp_static_namespace']['devices']),
            'USB_assignment_matches': len(result['hyp_static_namespace']['reference_usb_matches']),
            'initial_AP_proxy_votes': sum(v['ee'] == 0 for v in result['rpm_initial_proxy_votes']['votes']),
            'complete_AP_baseline': False,
            'production_profile_created': False, 'hardware_tested': False}))
    elif args.command == 'prepare-input':
        from project.firmware.input import prepare_input_firmware
        prepared = prepare_input_firmware(args.output or build_state.BUILD/'obj/input/firmware', args.firmware_archive)
        print(json.dumps(prepared, indent=2))
    elif args.command == 'prepare-bluetooth':
        if not args.firmware_archive or not args.firmware_profile:
            parser.error('prepare-bluetooth requires --firmware-archive and --firmware-profile')
        print(json.dumps(_component_radio.prepare_bluetooth_firmware(args.firmware_archive,args.firmware_profile,
            args.output or build_state.BUILD / 'obj/bluetooth/firmware',args.bluetoothd_plist),indent=2))
    elif args.command == 'build-usb-client':
        _component_clients.build_iokit_client('usbctl', ['usbctl/usbctl.cpp'],
            headers=['include/IOKitUser.h'], adhoc_sign=True,
            extra_exports=['IORegistryEntryFromPath', 'IORegistryEntryGetRegistryEntryID',
                           'IORegistryEntryCreateCFProperty', 'IOObjectConformsTo',
                           'IORegistryCreateIterator', 'IORegistryEntryGetName',
                           'IORegistryEntrySetCFProperties'])
    elif args.command == 'build-power-client':
        _component_clients.build_power_client()
    elif args.command == 'build-input-client':
        _component_clients.build_iokit_client('inputctl', ['inputctl/inputctl.mm'],
            headers=['include/IOKitUser.h', '../drivers/include/TouchDiagnosticGesture.h'],
            extra_exports=['IORegistryEntryGetRegistryEntryID','IORegistryEntryCreateCFProperty',
                'IOHIDEventSystemClientCreateWithType','IOHIDEventSystemClientRegisterEventCallback',
                'IOHIDEventSystemClientScheduleWithRunLoop','IOHIDEventSystemClientUnscheduleWithRunLoop',
                'IOHIDEventSystemClientCopyServices','IOHIDEventSystemClientCopyProperty','IOHIDServiceClientCopyProperty',
                'IOHIDServiceClientGetRegistryID','IOHIDEventGetType','IOHIDEventGetSenderID',
                'IOHIDEventGetTimeStamp','IOHIDEventGetIntegerValue','IOHIDEventGetFloatValue','IOHIDEventGetChildren',
                'IOHIDEventCreateDigitizerEvent','IOHIDEventCreateDigitizerFingerEvent',
                'IOHIDEventSetIntegerValue','IOHIDEventSetFloatValue','IOHIDEventSetSenderID',
                'IOHIDEventAppendEvent','IOHIDEventSystemClientDispatchEvent'],
            frameworks=['Foundation'], entitlements={'com.apple.private.hid.client.event-monitor':True,
                'com.apple.private.hid.client.admin':True,'com.apple.private.hid.client.debug-tool':True,
                'com.apple.private.hid.client.event-dispatch':True,
                'com.apple.hid.system.user-access-service':True},
            signing_identifier='org.darwin.development.inputctl')
    elif args.command == 'build-battery-client':
        _component_clients.build_iokit_client('batteryctl', ['batteryctl/batteryctl.cpp'],
            extra_exports=['IORegistryEntryCreateCFProperties', 'IOPSCopyPowerSourcesInfo',
                           'IOPSCopyPowerSourcesList', 'IOPSGetPowerSourceDescription'],
            headers=['include/IOKitUser.h'], adhoc_sign=True)
    elif args.command == 'build-haptics-client':
        _component_clients.build_iokit_client('hapticsctl', ['hapticsctl/hapticsctl.cpp'],
            headers=['include/IOKitUser.h'], protocol=['HapticsProtocol.h'], adhoc_sign=True)
    elif args.command == 'build-wifi-client':
        _component_clients.build_iokit_client('wifictl', ['wifictl/wifictl.cpp'],
            headers=['include/IOKitUser.h'], protocol=['WLANProtocol.h'], adhoc_sign=True)
    elif args.command == 'prepare-wifi':
        if bool(args.firmware_archive)!=bool(args.firmware_profile):
            parser.error('Wi-Fi firmware preparation requires both --firmware-archive and --firmware-profile')
        prepared={'driverkit':_component_radio.prepare_wifi_driverkit(),'firmware_assets':'not-provided'}
        if args.firmware_profile:
            prepared['firmware_assets']=_component_radio.prepare_wifi_firmware(args.firmware_archive,args.firmware_profile,
                args.output or build_state.BUILD/'obj/wifi/firmware')
        print(json.dumps(prepared,indent=2))
    elif args.command == 'build-wifi-driverkit':
        _component_radio.build_wifi_driverkit()
    elif args.command == 'verify-wifi-driverkit':
        print(json.dumps(_component_radio.verify_wifi_driverkit(),indent=2))
    elif args.command == 'build-bluetooth-client':
        _component_clients.build_iokit_client('bluetoothctl', ['bluetoothctl/bluetoothctl.cpp', '../drivers/bluetooth/userspace/IOKitBluetoothTransport.cpp'],
            headers=['include/IOKitUser.h', '../drivers/bluetooth/userspace/IOKitBluetoothTransport.h'],
            include_dirs=[build_state.ROOT / 'drivers/bluetooth/userspace'], protocol=['BluetoothProtocol.h'], adhoc_sign=True)
        xpc=(build_state.ROOT / 'tools/sdk/iPhoneOS18.6.sdk/usr/lib/system/libxpc.tbd').read_text()
        if not re.search(r'\b_xpc_connection_copy_entitlement_value\b',xpc):
            raise RuntimeError('SDK lacks audited XPC peer entitlement SPI')
        _component_clients.build_iokit_client('bluetoothbootstrap',['../drivers/bluetooth/userspace/BlueToolService.cpp',
            '../drivers/bluetooth/userspace/IOKitBluetoothTransport.cpp'],
            headers=['include/IOKitUser.h','../drivers/bluetooth/userspace/IOKitBluetoothTransport.h',
                     '../drivers/bluetooth/userspace/BlueToolBootstrap.h','../drivers/bluetooth/QcomBluetooth/BluetoothControl.h',
                     '../drivers/bluetooth/QcomBluetooth/BluetoothFirmware.h','../drivers/bluetooth/QcomBluetooth/BluetoothH4.h'],
            protocol=['BluetoothProtocol.h'], include_dirs=[build_state.ROOT/'drivers/bluetooth/userspace',build_state.DRIVERS_ROOT/'bluetooth/QcomBluetooth'],
            blocks=True,entitlements={'org.darwin.bluetooth.bootstrap':True},
            signing_identifier='org.darwin.development.bluetoothbootstrap')
    elif args.command == 'prepare-bluetooth-stack':
        print(json.dumps(_component_radio.prepare_bluetooth_stack(args.bluetooth_reference,args.output or build_state.BUILD/'output/Bluetooth'),indent=2))
    elif args.command == 'verify-bluetooth-stack':
        print(json.dumps(_component_radio.verify_bluetooth_stack(args.output or build_state.BUILD/'output/Bluetooth'),indent=2))
    elif args.command == 'verify-display-readback':
        if not args.capture_prefix:
            parser.error('verify-display-readback requires --capture-prefix')
        print(json.dumps(_component_services.verify_display_readback(args.capture_prefix), indent=2))
    elif args.command == 'prepare-keybag-policy':
        if not args.system_cryptex or not args.output or not args.development_no_sep:
            parser.error('prepare-keybag-policy requires --system-cryptex, --output and --development-no-sep')
        from project.firmware.patches.keybag import prepare
        print(json.dumps(prepare(args.system_cryptex, args.output), indent=2))
    elif args.command == 'prepare-display-services':
        policies = [(args.migration_service, args.development_skip_migration_unlock, _component_services.prepare_display_services),
                    (args.system_app_migrator, args.development_fail_fast_system_app_installs, _component_services.prepare_system_app_migrator),
                    (args.springboard_migrator, args.development_defer_launch_images, _component_services.prepare_springboard_migrator),
                    (args.fairplay_launchd_plist, args.development_no_sep_service_readiness, _component_services.prepare_no_sep_service_readiness)]
        selected = [(source, prepare) for source, enabled, prepare in policies if source and enabled]
        if (not args.output or len(selected) != 1 or
                any(bool(source) != enabled for source, enabled, _ in policies)):
            parser.error('prepare-display-services requires --output and exactly one pinned source with its development policy flag')
        if bool(args.migration_resource_fork) != bool(args.migration_decmpfs):
            parser.error('--service-resource-fork and --service-decmpfs must be supplied together')
        source, prepare = selected[0]
        cache_options = {}
        if args.launchd_cache or args.launchd_cache_signature:
            if prepare != _component_services.prepare_no_sep_service_readiness or not (args.launchd_cache and args.launchd_cache_signature):
                parser.error('launchd cache inputs require the no-SEP readiness policy and both cache/signature')
            cache_options = {'cache': args.launchd_cache, 'cache_signature': args.launchd_cache_signature}
        elif prepare == _component_services.prepare_no_sep_service_readiness:
            parser.error('iOS18 readiness policy requires --launchd-cache and --launchd-cache-signature')
        print(json.dumps(prepare(source, args.output,
            args.migration_resource_fork, args.migration_decmpfs, **cache_options), indent=2))
    elif args.command == 'build-display-client':
        _component_clients.build_iokit_client('displayctl', ['displayctl/displayctl.mm'],
            extra_exports=['IORegistryEntryGetRegistryEntryID', 'IORegistryEntryCreateCFProperty', 'IORegistryEntrySetCFProperties'],
            headers=['include/IOKitUser.h'], frameworks=['Foundation', 'QuartzCore', 'CoreGraphics', 'ImageIO', 'IOSurface', 'Metal'],
            entitlements={'com.apple.QuartzCore.displayable-context': True,
                          'com.apple.QuartzCore.secure-mode': True,
                          'com.apple.QuartzCore.display-state': True,
                          'com.apple.QuartzCore.global-capture': True,
                          'com.apple.QuartzCore.secure-capture': True},
            signing_identifier='org.darwin.development.displayctl')
        _component_clients.build_iokit_client('displayctl', ['displayctl/displayctl.mm'],
            extra_exports=['IORegistryEntryGetRegistryEntryID', 'IORegistryEntryCreateCFProperty', 'IORegistryEntrySetCFProperties'],
            headers=['include/IOKitUser.h'], frameworks=['Foundation', 'QuartzCore', 'CoreGraphics', 'ImageIO', 'IOSurface', 'Metal'], adhoc_sign=True, loadable=True)
    elif args.command == 'build-wallpaper-client':
        _component_clients.build_iokit_client('wallpaperctl', ['wallpaperctl/wallpaperctl.mm'],
            frameworks=['Foundation', 'UIKit'], adhoc_sign=True)
    elif args.command == 'build-gyro-client':
        _component_clients.build_iokit_client('gyroctl', ['gyroctl/gyroctl.mm'],
            frameworks=['Foundation', 'CoreMotion'], adhoc_sign=True)
    elif args.command == 'build-gpu-client':
        _component_gpu.build_gpu_client()
    elif args.command == 'build-gpu-metal':
        _component_gpu.build_gpu_metal()
    elif args.command == 'build-gpu-shaders':
        _component_gpu.build_gpu_shaders()
    elif args.command == 'build-drivers':
        collection = args.collection or (build_state.COLLECTION if not args.driver else None)
        sources = _component_drivers.collection_sources(collection) if collection else None
        _component_drivers.build_drivers(args.jobs, sources)
    elif args.command == 'verify-drivers':
        collection = args.collection or (build_state.COLLECTION if not args.driver else None)
        sources = _component_drivers.collection_sources(collection) if collection else None
        _component_drivers.verify_drivers(sources)
    elif args.command == 'pack-drivers':
        _component_drivers.pack_drivers(args.jobs, args.collection, args.output, args.base_manifest, args.local_kernel)
    elif args.command == 'verify-collection':
        _component_drivers.verify_collection(args.collection, args.output)
    else:
        _component_kernel.verify_kernel()

if __name__ == "__main__":
    main()
