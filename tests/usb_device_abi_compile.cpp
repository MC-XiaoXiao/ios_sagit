// SPDX-License-Identifier: BSD-2-Clause
// Compile-only ABI probe. Never linked into a driver or registered.
#include <IOKit/usb/IOUSBDeviceController.h>
static_assert(sizeof(IOUSBDeviceIsocFrame)==24 && alignof(IOUSBDeviceIsocFrame)==8,
    "native isochronous request frame pointer uses 24-byte aligned elements");
#include <IOKit/usb/IOUSBDeviceControlRequest.h>
#include <IOKit/usb/IOUSBDeviceInterface.h>
#include "QcomUSBDevice.h"
class USBDeviceABIProbe : public IOUSBDeviceController {
    OSDeclareDefaultStructors(USBDeviceABIProbe);
public:
    void nativeSlot175(UInt32) APPLE_KEXT_OVERRIDE {}
    IOReturn goOnBus(bool) APPLE_KEXT_OVERRIDE { return {}; }
    bool connectedWithAccessoryCable() APPLE_KEXT_OVERRIDE { return {}; }
    void handleStallEndpoint(int, bool) APPLE_KEXT_OVERRIDE {}
    void handleAbortEndpoint(int) APPLE_KEXT_OVERRIDE {}
    void handleSetAddressPreStatusPhase(int) APPLE_KEXT_OVERRIDE {}
    void handleSetAddressPostStatusPhase(int) APPLE_KEXT_OVERRIDE {}
    UInt32 getConnectionSpeed() APPLE_KEXT_OVERRIDE { return {}; }
    IOReturn provideEndpointIDsForConfiguration(OSArray *, UInt8 *) APPLE_KEXT_OVERRIDE { return {}; }
    void prepareConfiguration(OSArray *) APPLE_KEXT_OVERRIDE {}
    IOReturn activateEndpoint(int, int, int, int, tIOUSBDeviceInterfacePipeProperty) APPLE_KEXT_OVERRIDE { return {}; }
    void deactivateEndpoint(int) APPLE_KEXT_OVERRIDE {}
    IOBufferMemoryDescriptor *createIOBuffer(UInt32, UInt64) APPLE_KEXT_OVERRIDE { return nullptr; }
    bool isEndpointStalled(int) APPLE_KEXT_OVERRIDE { return {}; }
    void resetEndpointDataToggle(int) APPLE_KEXT_OVERRIDE {}
    void nativeSlot199() APPLE_KEXT_OVERRIDE {}
    IOReturn doRemoteWakeGated() APPLE_KEXT_OVERRIDE { return {}; }
    IOReturn nativeSlot203(UInt32) APPLE_KEXT_OVERRIDE { return {}; }
    IOReturn nativeSlot233(UInt32, UInt32) APPLE_KEXT_OVERRIDE { return {}; }
    IOReturn ioGated(_IOUSBDeviceIORequestTag *) APPLE_KEXT_OVERRIDE { return {}; }
};
OSDefineMetaClassAndStructors(USBDeviceABIProbe, IOUSBDeviceController);
static_assert(sizeof(USBDeviceABIProbe) == 560, "device ABI probe adds no state");
static_assert(__is_same(decltype(&IOUSBDeviceController::activateEndpoint),
    IOReturn (IOUSBDeviceController::*)(int, int, int, int, tIOUSBDeviceInterfacePipeProperty)),
    "native activatePipeGated supplies all five endpoint arguments");
static_assert(__is_same(decltype(&QcomUSBDevice::createPipeGated),
    IOReturn (QcomUSBDevice::*)(tCreatePipeParameters &,IOUSBDevicePipe *&)),
    "pipe admission preserves the Family factory ABI");
static_assert(__is_same(decltype(&IOUSBDeviceController::nativeSlot175),
    void (IOUSBDeviceController::*)(UInt32)), "native panic dispatch supplies its action");
static_assert(__is_same(decltype(&IOUSBDeviceController::prepareConfiguration),
    void (IOUSBDeviceController::*)(OSArray *)), "native configuration hook takes interface array");
static_assert(__is_same(decltype(&IOUSBDeviceController::doRemoteWakeGated),
    IOReturn (IOUSBDeviceController::*)()), "native remote wake returns status");
static_assert(__is_same(decltype(&IOUSBDeviceController::nativeSlot203),
    IOReturn (IOUSBDeviceController::*)(UInt32)), "native function wake supplies its selected interface");
static_assert(__is_same(decltype(&IOUSBDeviceController::nativeSlot233),
    IOReturn (IOUSBDeviceController::*)(UInt32, UInt32)), "native link policy supplies speed and U1/U2 selector");
static_assert(__is_same(decltype(&IOUSBDeviceController::nativeSlot199),
    void (IOUSBDeviceController::*)()), "native forced-disconnect hook has no consumed status");

class USBControlRequestABIProbe : public IOUSBDeviceControlRequest {
    OSDeclareDefaultStructors(USBControlRequestABIProbe);
};
OSDefineMetaClassAndStructors(USBControlRequestABIProbe, IOUSBDeviceControlRequest);
static_assert(sizeof(USBControlRequestABIProbe) == 160, "control request ABI probe adds no state");
class USBControlRequestStateProbe : public IOUSBDeviceControlRequest {
public:
    UInt32 adapterState;
};
static_assert(sizeof(USBControlRequestStateProbe) == 168, "adapter fields must follow all 160 native bytes");

// Force references to the audited nonvirtual entry points. These compile-only
// functions never execute and are never linked into a loadable extension.
void USBDeviceABICompletionReference(IOUSBDeviceController *controller, _IOUSBDeviceIORequestTag *request) {
    controller->readyForHost();
    controller->completeEndpointIO(request);
}
void USBDeviceABIControlReference(IOUSBDeviceController *controller, UInt8 *setup, UInt8 *data, UInt64 length) {
    auto *tracker = IOUSBDeviceControlRequest::withControllerOptions(controller, 128, 0, 0);
    if (!tracker) return;
    tracker->inputSetupPacket(setup);
    tracker->processSetupPacket();
    tracker->inputDataPacket(data, length);
    tracker->reset();
    tracker->stop();
    tracker->release();
}

void USBDeviceABIInterfaceReference(OSObject *object) {
    auto *interface = IOUSBDeviceInterface::fromObject(object);
    if (!interface || !interface->getAlternateSettingCount()) return;
    if (!interface->getPipeCountForAlternateSetting(0)) return;
    auto *pipe = interface->getPipeForAlternateSetting(0, 0);
    if (pipe) (void)pipe->getDirection();
}

// This compile-only subclass checks the concrete adapter's actual hooks,
// including the integrated ioGated entry and both remote-wake dispatches.
class USBDeviceTransportABIProbe : public QcomUSBDevice {
    OSDeclareDefaultStructors(USBDeviceTransportABIProbe);
};
OSDefineMetaClassAndStructors(USBDeviceTransportABIProbe, QcomUSBDevice);
static_assert(sizeof(QcomUSBDevice) == 576, "native bytes end before adapter state");
