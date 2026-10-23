// SPDX-License-Identifier: BSD-2-Clause
#include "usb_firmware_stop.h"
extern "C" {
#include "bridge.h"
extern const unsigned char usb_config_pin_blob[],usb_config_pin_end[];
}
namespace { BridgeUSB::FirmwareStop<> stopper; }
extern "C" int bridge_usb_firmware_stop(EFI_BOOT_SERVICES *services,void *tree) {
    uint32_t bytes=0,controllerBytes=0;
    auto *out=dt_property(tree,"chosen",USB::FirmwareStopProperty,&bytes);
    const auto *binding=dt_property(tree,"chosen",USB::FirmwareControllerProperty,&controllerBytes);
    USB::FirmwareController controller;
    const auto pinBytes=uintptr_t(usb_config_pin_end)-uintptr_t(usb_config_pin_blob);
    if (!USB::FirmwareController::decode(binding,controllerBytes,controller) || pinBytes>0x100000) return -1;
    if (stopper.prepare(services,controller,usb_config_pin_blob,unsigned(pinBytes),out,bytes)) return 0;
    bridge_puts("[USB firmware] retirement stage=");bridge_hex(stopper.diagnosticStage());
    bridge_puts(" preflight=");bridge_hex(stopper.diagnosticPreflight());bridge_puts("\n");
    bridge_puts("[USB firmware] override=");bridge_hex(stopper.diagnosticOverride());
    bridge_puts(" mode=");bridge_hex(stopper.diagnosticMode());
    bridge_puts(" handles=");bridge_hex(stopper.diagnosticHandles());bridge_puts("\n");
    return -1;
}
extern "C" int bridge_usb_firmware_stopped(void) {return stopper.mutated() ? 1:0;}
extern "C" int bridge_usb_firmware_stop_finish(void) {return stopper.finish() ? 0:-1;}
