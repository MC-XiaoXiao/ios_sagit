// SPDX-License-Identifier: BSD-2-Clause
// Compile-only native ABI probe. Never linked or matched as a controller.
#include <IOKit/usb/AppleUSBHostPort.h>
class USBHostPortABIProbe : public AppleUSBHostPort {
    OSDeclareDefaultStructors(USBHostPortABIProbe);
public:
    UInt32 getPortType() APPLE_KEXT_OVERRIDE { return 0; }
    IOReturn powerOff() APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
    IOReturn powerOn() APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
    IOReturn suspend() APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
    IOReturn resume() APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
    IOUSBHostDevice *resetAndCreateDevice(UInt32) APPLE_KEXT_OVERRIDE { return nullptr; }
    IOReturn getPortStatusGated(UInt32 &) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
};
OSDefineMetaClassAndStructors(USBHostPortABIProbe,AppleUSBHostPort);
static_assert(sizeof(USBHostPortABIProbe)==920,"native host port probe adds no state");
static_assert(__is_same(decltype(static_cast<IOReturn (AppleUSBHostPort::*)(UInt32,UInt32)>(&AppleUSBHostPort::reset)),
    IOReturn (AppleUSBHostPort::*)(UInt32,UInt32)),"native reset supplies timeout and options");
static_assert(__is_same(decltype(&AppleUSBHostPort::createDevice),
    IOUSBHostDevice *(AppleUSBHostPort::*)(UInt32)),"native port delegates device creation to controller");
static_assert(__is_same(decltype(&AppleUSBHostPort::initBusCurrent),
    IOReturn (AppleUSBHostPort::*)()),"Native slot214 initializes current before port admission");
static_assert(__is_same(decltype(&AppleUSBHostPort::getCompanionPortGated),
    IOReturn (AppleUSBHostPort::*)(UInt32,AppleUSBHostPort *&)),"Native slot219 owns the companion cache retain");

#include <IOKit/usb/AppleUSBHostController.h>
static_assert(__is_same(decltype(&AppleUSBHostController::getCompanionPort),
    AppleUSBHostPort *(AppleUSBHostController::*)(AppleUSBHostPort *,UInt32)),"Native slot235 returns a borrowed port");
class USBHostControllerABIProbe : public AppleUSBHostController {
    OSDeclareDefaultStructors(USBHostControllerABIProbe);
public:
    IOUSBHostDevice *createDevice(UInt32 speed, UInt32 location, UInt32 options) APPLE_KEXT_OVERRIDE { return nullptr; }
    void nativeSlot193(IOUSBHostDevice *) APPLE_KEXT_OVERRIDE {}
    IOReturn createPipeGated(IOUSBHostPipe::StandardUSBDescriptors &,IOUSBHostDevice *,IOUSBHostInterface *,IOUSBHostPipe *&) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
    IOReturn adjustPipeGated(IOUSBHostPipe *,IOUSBHostPipe::StandardUSBDescriptors &) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
    IOReturn closePipeGated(IOUSBHostPipe *) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
    IOReturn ioGated(AppleUSBHostRequest *) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
    IOReturn abortPipeGated(IOUSBHostPipe *,IOReturn,IOService *) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
    IOReturn clearPipeStallGated(IOUSBHostPipe *) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
    IOReturn suspendPipesGated(IOUSBHostDevice *) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
    IOReturn resumePipesGated(IOUSBHostDevice *) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
    IOReturn enableStreamsGated(IOUSBHostPipe *) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
    IOReturn disableStreamsGated(IOUSBHostPipe *) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
    IOReturn createStreamGated(IOUSBHostDevice *,IOUSBHostPipe *,UInt32,IOUSBHostStream *&) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
    IOReturn abortStreamGated(IOUSBHostStream *,IOReturn) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
    IOReturn closeStreamGated(IOUSBHostStream *) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
    IODMACommand *getDMACommand(UInt32 alignment,UInt32 options) APPLE_KEXT_OVERRIDE { return nullptr; }
    bool filterOccurred(IOFilterInterruptEventSource *) APPLE_KEXT_OVERRIDE { return true; }
    void interruptOccurred(IOInterruptEventSource *,int) APPLE_KEXT_OVERRIDE {  }
    UInt64 getRawMicroFrameCount() APPLE_KEXT_OVERRIDE { return 0; }
    UInt64 makeFrameNumber(UInt64, UInt64) const APPLE_KEXT_OVERRIDE { return 0; }
};
OSDefineMetaClassAndStructors(USBHostControllerABIProbe,AppleUSBHostController);
static_assert(sizeof(USBHostControllerABIProbe)==1384,"host controller probe adds no state");
static_assert(__is_same(decltype(&AppleUSBHostController::powerDown),
    IOReturn (AppleUSBHostController::*)(unsigned long)),"Native slot182 receives the decremented 64-bit power state");
static_assert(__is_same(decltype(&AppleUSBHostController::powerUp),
    IOReturn (AppleUSBHostController::*)(unsigned long)),"Native slot183 receives the next 64-bit power state");
static_assert(__is_same(decltype(&AppleUSBHostController::registerHubGated),
    IOReturn (AppleUSBHostController::*)(IOUSBHostDevice *,const StandardUSB::HubDescriptor *,tRegisterHubFlags)),
    "Native slot198 receives descriptor and 32-bit multi-TT flags");
static_assert(__is_same(decltype(&AppleUSBHostController::registerSuperSpeedHubGated),
    IOReturn (AppleUSBHostController::*)(IOUSBHostDevice *,const StandardUSB::SuperSpeedHubDescriptor *)),
    "Native slot199 receives the USB3 hub descriptor");
static_assert(__is_same(decltype(&AppleUSBHostController::unregisterHubGated),
    IOReturn (AppleUSBHostController::*)(IOUSBHostDevice *)),"Native slot200 returns an unregister result");
static_assert(__builtin_offsetof(StandardUSB::HubDescriptor,wHubCharacteristics)==3 &&
    __builtin_offsetof(StandardUSB::SuperSpeedHubDescriptor,wHubDelay)==8,"packed Native hub descriptor offsets");
IOReturn USBHostNativeHubCompile(AppleUSBHostController *controller,IOUSBHostDevice *device,
    const StandardUSB::HubDescriptor *usb2,const StandardUSB::SuperSpeedHubDescriptor *usb3) {
    if (usb2) return controller->AppleUSBHostController::registerHub(device,usb2,kRegisterHubFlagsMultiTT);
    if (usb3) return controller->AppleUSBHostController::registerHub(device,usb3);
    return controller->AppleUSBHostController::unregisterHub(device);
}
static_assert(__is_same(decltype(&AppleUSBHostController::suspendPipesGated),
    IOReturn (AppleUSBHostController::*)(IOUSBHostDevice *)),"Native slot221 suspends one device's pipes");
static_assert(__is_same(decltype(&AppleUSBHostController::resumePipesGated),
    IOReturn (AppleUSBHostController::*)(IOUSBHostDevice *)),"Native slot222 resumes one device's pipes");
static_assert(__is_same(decltype(&AppleUSBHostController::getFrameNumber),
    UInt64 (AppleUSBHostController::*)(UInt64 *)),"Native frame number is a 64-bit return value");
static_assert(__is_same(decltype(&AppleUSBHostController::getFrameRollover),
    UInt64 (AppleUSBHostController::*)(UInt64 *)),"Native rollover returns the first frame in the epoch");
UInt64 USBHostNativeFrameCompile(AppleUSBHostController *controller,UInt64 *stamp,bool rollover) {
    return rollover ? controller->AppleUSBHostController::getFrameRollover(stamp) :
        controller->AppleUSBHostController::getFrameNumber(stamp);
}
IOUSBHostDevice *USBHostNativeAllocationCompile(AppleUSBHostController *controller,UInt8 speed,UInt16 address) {
    return controller->AppleUSBHostController::nativeSlot194(speed,address);
}

#include <IOKit/usb/AppleUSBHostDMACommand.h>
class USBHostDMAABIProbe : public AppleUSBHostDMACommand {
    OSDeclareDefaultStructors(USBHostDMAABIProbe);
};
OSDefineMetaClassAndStructors(USBHostDMAABIProbe,AppleUSBHostDMACommand);
static_assert(sizeof(USBHostDMAABIProbe)==144,"Host DMA probe adds no state");

#include <IOKit/usb/AppleUSBHostRequest.h>
class USBRequestABIProbe : public AppleUSBRequest {
    OSDeclareDefaultStructors(USBRequestABIProbe);
};
OSDefineMetaClassAndStructors(USBRequestABIProbe,AppleUSBRequest);
static_assert(sizeof(USBRequestABIProbe)==56,"Common request probe adds no state");
class USBHostRequestABIProbe : public AppleUSBHostRequest {
    OSDeclareDefaultStructors(USBHostRequestABIProbe);
};
OSDefineMetaClassAndStructors(USBHostRequestABIProbe,AppleUSBHostRequest);
static_assert(sizeof(USBHostRequestABIProbe)==64,"Host request probe adds no state");
static_assert(__is_same(decltype(&AppleUSBRequest::update),
    IOReturn (AppleUSBRequest::*)()),"Native slot19 publishes one ISO frame");
static_assert(__is_same(decltype(&AppleUSBRequest::getPreparedBuffer),
    IOMemoryDescriptor *(AppleUSBRequest::*)() const),"iOS18 slot24 returns the prepared MD");
static_assert(__is_same(decltype(&AppleUSBRequest::getCompletionTimeout),
    UInt32 (AppleUSBRequest::*)() const),"iOS18 slot25 returns milliseconds");
static_assert(__is_same(decltype(&AppleUSBRequest::getIOLength),
    UInt64 (AppleUSBRequest::*)() const),"iOS18 slot26 preserves the 64-bit I/O length");
static_assert(__is_same(decltype(&AppleUSBRequest::getClient),
    IOService *(AppleUSBRequest::*)() const),"iOS18 slot27 returns the retained control client");
static_assert(__is_same(decltype(&AppleUSBRequest::getFrameList),
    IOUSBHostIsochronousTransaction *(AppleUSBRequest::*)() const),"Native slot30 returns 28-byte ISO transactions");
static_assert(__is_same(decltype(IOUSBHostIsochronousTransaction::status),IOReturn),"Native ISO status width");
static_assert(__is_same(decltype(IOUSBHostIsochronousTransaction::timeStamp),AbsoluteTime),"Native ISO timestamp width");

#include <IOKit/usb/AppleUSBRequestCompleter.h>
class USBRequestCompleterABIProbe : public AppleUSBRequestCompleter {
    OSDeclareDefaultStructors(USBRequestCompleterABIProbe);
};
OSDefineMetaClassAndStructors(USBRequestCompleterABIProbe,AppleUSBRequestCompleter);
static_assert(sizeof(USBRequestCompleterABIProbe)==176,"Completer probe adds no state");

// Exercise the original Family request interface without exposing its private
// AppleUSBIORequest fields. The runtime adapter must release its hardware DMA
// and copy IN data before handing the original request to its native queue.
IOReturn USBHostRequestCompletionCompile(AppleUSBHostRequest *request,IOReturn status,UInt32 bytes,UInt64 stamp) {
    auto *completer=request->getRequestCompleter();
    if (!completer) return kIOReturnNotReady;
    request->setStatus(status);request->setBytesTransferred(bytes);request->setTimeStamp(stamp);
    return completer->enqueueRequest(request);
}

class USBHostIOSourceABIProbe : public IOUSBHostIOSource {
    OSDeclareDefaultStructors(USBHostIOSourceABIProbe);
};
OSDefineMetaClassAndStructors(USBHostIOSourceABIProbe,IOUSBHostIOSource);
static_assert(sizeof(USBHostIOSourceABIProbe)==96,"native IO source probe");
class USBHostPipeABIProbe : public IOUSBHostPipe {
    OSDeclareDefaultStructors(USBHostPipeABIProbe);
};
OSDefineMetaClassAndStructors(USBHostPipeABIProbe,IOUSBHostPipe);
static_assert(sizeof(USBHostPipeABIProbe)==184,"native pipe probe");
static_assert(__is_same(decltype(&IOUSBHostPipe::clearStall),
    IOReturn (IOUSBHostPipe::*)(bool)),"Native slot43 schedules gated pipe recovery");
static_assert(__is_same(decltype(&IOUSBHostPipe::clearStallGated),
    IOReturn (IOUSBHostPipe::*)(bool)),"Native slot49 selects CLEAR_FEATURE after HCD clear");
static_assert(__is_same(decltype(&AppleUSBHostPort::clearPipeStall),
    IOReturn (AppleUSBHostPort::*)(IOUSBHostPipe *)),"Native slot170 preserves original pipe through HubPort");
static_assert(__is_same(decltype(&AppleUSBHostController::closePipeGated),
    IOReturn (AppleUSBHostController::*)(IOUSBHostPipe *)),"native command gate receives close result");
static_assert(__is_same(decltype(&IOUSBHostPipe::initWithDescriptorsAndOwners),
    bool (IOUSBHostPipe::*)(const StandardUSB::EndpointDescriptor *,
    const StandardUSB::SuperSpeedEndpointCompanionDescriptor *,AppleUSBHostController *,
    IOUSBHostDevice *,IOUSBHostInterface *,UInt8,UInt16)),"native pipe initialization arguments");

#include "QcomUSBHost.h"
class USBHostStartABIProbe : public QcomUSBHost {
    OSDeclareDefaultStructors(USBHostStartABIProbe);
};
OSDefineMetaClassAndStructors(USBHostStartABIProbe,QcomUSBHost);
static_assert(sizeof(USBHostStartABIProbe)==1456,"Host adapter instance ABI");

#include "QcomUSBHostPort.h"
class USBHostPortRuntimeABIProbe : public QcomUSBHostPort {
    OSDeclareDefaultStructors(USBHostPortRuntimeABIProbe);
};
OSDefineMetaClassAndStructors(USBHostPortRuntimeABIProbe,QcomUSBHostPort);
static_assert(sizeof(USBHostPortRuntimeABIProbe)==936,"runtime port adds no extra probe state");

#include <IOKit/usb/IOUSBHostDevice.h>
IOService *USBHostNativeDeviceCompile(IOUSBHostDevice *device,UInt16 &address,UInt8 &speed) {
    address=device->getDeviceAddress();speed=device->getSpeed();return device->service();
}
static_assert(sizeof(StandardUSB::DeviceRequest)==8 &&
    __builtin_offsetof(StandardUSB::DeviceRequest,wValue)==2 &&
    __builtin_offsetof(StandardUSB::DeviceRequest,wIndex)==4 &&
    __builtin_offsetof(StandardUSB::DeviceRequest,wLength)==6,"Native USB setup packet");
static_assert(__is_same(decltype(&IOUSBHostDevice::internalDeviceRequest),
    IOReturn (IOUSBHostDevice::*)(IOService *,StandardUSB::DeviceRequest &,void *,IOMemoryDescriptor *,
    UInt32 *,IOUSBHostCompletion *,UInt32)),"Native slot207 uses x1-x7 for internal device request");
IOReturn USBHostNativeTTCompile(IOUSBHostDevice *hub,StandardUSB::DeviceRequest &setup,UInt32 *bytes,
    IOUSBHostPipe *pipe,AppleUSBHostPort *port) {
    if (hub) return hub->internalDeviceRequest(hub->service(),setup,nullptr,nullptr,bytes,nullptr,500);
    if (port) return port->AppleUSBHostPort::clearPipeStall(pipe);
    return pipe->IOUSBHostPipe::clearStall(false);
}
