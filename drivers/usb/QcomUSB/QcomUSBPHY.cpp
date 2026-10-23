// SPDX-License-Identifier: BSD-2-Clause
#include "QcomUSBPHY.h"
#include "USBMMIO.h"
#include "USBPhy.h"
#include "USBPhyResources.h"
#include <ClockControllerProtocol.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSData.h>
#include <machine/machine_routines.h>

OSDefineMetaClassAndStructors(QcomUSBPHY, IOService);
namespace {
UInt32 cell(const UInt8 *p) { return UInt32(p[0]) | UInt32(p[1]) << 8 | UInt32(p[2]) << 16 | UInt32(p[3]) << 24; }
bool tuning(IOService *provider,const char *name,USBPHY::USB2Tuning &out,bool host) {
    auto *object=provider->getProperty(name);
    if (!object) return true;
    auto *data=OSDynamicCast(OSData,object);
    if (!data || !data->getLength() || data->getLength()%8 || data->getLength()>16*8) return false;
    const auto *bytes=static_cast<const UInt8 *>(data->getBytesNoCopy());
    out.count=data->getLength()/8;
    for (unsigned i=0;i<out.count;++i) out.settings[i]={cell(bytes+i*8),cell(bytes+i*8+4)};
    return out.valid(host);
}
USB::Result result(IOReturn r) {
    if (r == kIOReturnSuccess) return USB::Result::Success;
    if (r == kIOReturnTimeout) return USB::Result::Timeout;
    if (r == kIOReturnNotReady || r == kIOReturnNotAttached) return USB::Result::NotReady;
    if (r == kIOReturnUnsupported) return USB::Result::Unsupported;
    if (r == kIOReturnBusy) return USB::Result::Busy;
    if (r == kIOReturnBadArgument) return USB::Result::Invalid;
    return USB::Result::HardwareError;
}
IOReturn nativeResult(USB::Result r) {
    if (r == USB::Result::Success) return kIOReturnSuccess;
    if (r == USB::Result::Timeout) return kIOReturnTimeout;
    if (r == USB::Result::NotReady) return kIOReturnNotReady;
    if (r == USB::Result::Unsupported) return kIOReturnUnsupported;
    if (r == USB::Result::Busy) return kIOReturnBusy;
    if (r == USB::Result::Invalid) return kIOReturnBadArgument;
    return kIOReturnIOError;
}
}

struct QcomUSBPHY::State {
    IOService *provider, *bus, *client = nullptr, *leaseOwner = nullptr;
    IOLock *lock = nullptr;
    IOMemoryMap *maps[3]{};
    USB::MMIO *io = nullptr, *calibration = nullptr, *clampIO = nullptr;
    USBPHY::Resources<State> resources{*this};
    USBPHY::USB2<USB::MMIO, USBPHY::Resources<State>> *usb2 = nullptr;
    USBPHY::USB3<USB::MMIO, USBPHY::Resources<State>> *usb3 = nullptr;
    const OSSymbol *function = nullptr;
    UInt64 serial = 0, lease = 0;
    bool superSpeed = false, differential = false, opened = false, stopping = false;
    UInt32 consumer = 0;
    USB::Role role=USB::Role::Device;
    explicit State(IOService *nub) : provider(nub), bus(nub->getProvider()) {
        provider->retain(); bus->retain(); lock = IOLockAlloc();
        function = OSSymbol::withCStringNoCopy(ClockController::Function);
    }
    USB::Result request(ClockController::Controller controller, ClockController::Operation operation,
                     UInt32 id, UInt64 &value) {
        if (!function) return USB::Result::NotReady;
        auto r = ClockController::targeted(controller, operation, id, value, consumer);
        auto status = bus->callPlatformFunction(function, false, &r.request, nullptr, nullptr, nullptr);
        if (!ClockController::valid(r.request) || r.request.version!=2 || r.request.size!=sizeof(r) ||
            r.controller!=controller || r.consumer!=consumer || r.flags || r.request.operation!=operation || r.request.id!=id)
            return USB::Result::HardwareError;
        value = r.request.value; return result(status);
    }
    IOReturn release() {
        const auto stopped = superSpeed ? usb3->stop() : usb2->stop();
        if (stopped != USB::Result::Success) return nativeResult(stopped);
        return nativeResult(resources.release());
    }
    USB::Result clamp(bool enabled) {
        if (superSpeed || !clampIO || clampIO->failed()) return USB::Result::NotReady;
        const auto before=clampIO->read(0);
        if (before==UINT32_MAX) return USB::Result::HardwareError;
        const UInt32 next=enabled ? 1 : 0;
        clampIO->write(0,next);
        return !clampIO->failed() && clampIO->read(0)==next ? USB::Result::Success : USB::Result::HardwareError;
    }
    IOReturn enable(USBPHY::Orientation orientation) {
        const auto acquired = resources.acquire();
        if (acquired != USB::Result::Success) return nativeResult(acquired);
        if (superSpeed) {
            // TCSR VLS clamp is a separate, single-register DT resource. It
            // shares no RMW with unrelated TCSR users and is not a readiness
            // flag. Deep suspend/autonomous wake will own the inverse write.
            if (calibration->read(0) == UINT32_MAX) return kIOReturnIOError;
            calibration->write(0, 0);
            if (calibration->read(0) != 0) return kIOReturnIOError;
            return nativeResult(usb3->start(orientation));
        }
        const auto fuse = calibration ? calibration->read(0) : 0;
        // All data values, including an all-ones word/trim15, are legal OTP
        // contents. Unlike a controller status register, this is not a test
        // for an inaccessible bus. Check the mapping bounds instead.
        if (calibration && calibration->failed()) return kIOReturnIOError;
        return nativeResult(usb2->start((fuse >> 16) & 15,role));
    }
};

bool QcomUSBPHY::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    fStarted = true;
    auto *memory = provider->getDeviceMemoryWithIndex(0);
    if (!memory || !provider->getProvider()) goto fail;
    fState = new State(provider);
    if (!fState || !fState->lock || !fState->function) goto fail;
    {
        auto &s = *fState;
        s.superSpeed = memory->getPhysicalAddress() == 0x0c010000;
        if (!s.superSpeed && memory->getPhysicalAddress() != 0x0c012000) goto fail;
        const UInt64 bytes = s.superSpeed ? USBPHY::USB3<USB::MMIO, USBPHY::Resources<State>>::Bytes : USBPHY::USB2<USB::MMIO, USBPHY::Resources<State>>::Bytes;
        auto *second = provider->getDeviceMemoryWithIndex(1);
        auto *third=provider->getDeviceMemoryWithIndex(2);
        if (memory->getLength() != bytes || !second || second->getLength() != 4 ||
            second->getPhysicalAddress() != (s.superSpeed ? 0x01fcb244u : 0x01fcb24cu) ||
            (third && (s.superSpeed || third->getLength()!=4 || third->getPhysicalAddress()!=0x00784238u)) ||
            provider->getDeviceMemoryCount()!=(third ? 3u : 2u)) goto fail;
        auto *supplies = OSDynamicCast(OSData, provider->getProperty("qcom,usb-phy-supplies"));
        const unsigned railCount = supplies ? supplies->getLength()/8 : 0;
        USBPHY::Supply rails[4]{};
        if (!supplies || supplies->getLength()!=railCount*8 ||
            (s.superSpeed ? railCount!=2 : (railCount!=3 && railCount!=4))) goto fail;
        const auto *p = static_cast<const UInt8 *>(supplies->getBytesNoCopy());
        for (unsigned i = 0; i < railCount; ++i) {
            rails[i] = {cell(p + i * 8), cell(p + i * 8 + 4)};
        }
        if (!s.superSpeed) {
            if (auto *reference = OSDynamicCast(OSData, provider->getProperty("qcom,usb-phy-differential-ref"))) {
                if (reference->getLength() != 4 || cell(static_cast<const UInt8 *>(reference->getBytesNoCopy())) > 1) goto fail;
                s.differential = cell(static_cast<const UInt8 *>(reference->getBytesNoCopy()));
            } else if (provider->getProperty("qcom,usb-phy-differential-ref")) goto fail;
        }
        s.consumer = s.superSpeed ? ClockController::USB3PHYConsumer : ClockController::USB2PHYConsumer;
        if (!s.resources.configure(s.superSpeed, s.differential, rails, railCount)) goto fail;
        if (!provider->open(this)) goto fail;
        s.opened = true;
        for (unsigned i = 0; i < (third ? 3u:2u); ++i) {
            s.maps[i] = provider->mapDeviceMemoryWithIndex(i, kIOMapInhibitCache);
            if (!s.maps[i] || !s.maps[i]->getVirtualAddress() || s.maps[i]->getLength() < (i ? 4 : bytes)) goto fail;
        }
        s.io = new USB::MMIO(reinterpret_cast<void *>(s.maps[0]->getVirtualAddress()), bytes);
        if (s.superSpeed || third) s.calibration = new USB::MMIO(reinterpret_cast<void *>(s.maps[s.superSpeed ? 1 : 2]->getVirtualAddress()), 4);
        if (!s.superSpeed) s.clampIO=new USB::MMIO(reinterpret_cast<void *>(s.maps[1]->getVirtualAddress()),4);
        if (!s.io || ((s.superSpeed || third) && !s.calibration) || (!s.superSpeed && !s.clampIO)) goto fail;
        if (s.superSpeed) s.usb3 = new USBPHY::USB3<USB::MMIO, USBPHY::Resources<State>>(*s.io, s.resources);
        else s.usb2 = new USBPHY::USB2<USB::MMIO, USBPHY::Resources<State>>(*s.io, s.resources);
        if ((!s.superSpeed && !s.usb2) || (s.superSpeed && !s.usb3)) goto fail;
        if (!s.superSpeed) {
            USBPHY::USB2Tuning device{},host{};
            if (!tuning(provider,"qcom,usb-phy-device-init",device,false) ||
                !tuning(provider,"qcom,usb-phy-host-init",host,true) || !s.usb2->configure(device,host)) goto fail;
        }
        setProperty("USBPhyKind", s.superSpeed ? "QMP-USB3" : "QUSB2");
        setProperty("USBPhyReady", false);
        setProperty("SystemSuspendSupported", false);
    }
    registerService(); return true;
fail:
    stop(provider); return false;
}
void QcomUSBPHY::pin() {
    if (!fPinned) { retain(); fPinned = true; }
    setProperty("USBPhyFault", true);
}
bool QcomUSBPHY::handleOpen(IOService *client, IOOptionBits, void *) {
    if (!fState || !fState->lock || !client) return false;
    auto &s = *fState; IOLockLock(s.lock);
    const bool accepted = !s.stopping && !fPinned && (!s.client || s.client == client);
    if (accepted) s.client = client;
    IOLockUnlock(s.lock); return accepted;
}
void QcomUSBPHY::handleClose(IOService *client, IOOptionBits) {
    if (!fState || !fState->lock) return;
    auto &s = *fState; IOLockLock(s.lock);
    if (s.client == client && !s.lease) s.client = nullptr;
    IOLockUnlock(s.lock);
}
bool QcomUSBPHY::handleIsOpen(const IOService *client) const {
    if (!fState || !fState->lock) return false;
    auto &s = *fState; IOLockLock(s.lock);
    const bool opened = s.client && (!client || s.client == client);
    IOLockUnlock(s.lock); return opened;
}
IOReturn QcomUSBPHY::callPlatformFunction(const OSSymbol *name, bool wait, void *a, void *b, void *c, void *d) {
    if (!name || !name->isEqualTo(USBPHY::Function)) return IOService::callPlatformFunction(name, wait, a, b, c, d);
    auto *request = static_cast<USBPHY::Request *>(a); auto *client = static_cast<IOService *>(b);
    if (!request || !client || c || d || !USBPHY::valid(*request)) return kIOReturnBadArgument;
    if (ml_at_interrupt_context() || !fState) return kIOReturnNotReady;
    auto &s = *fState; IOService *releaseOwner = nullptr; bool unpin = false;
    IOLockLock(s.lock);
    IOReturn status = kIOReturnNotReady;
    if (s.client != client || (s.stopping && request->operation != USBPHY::Operation::Disable)) goto out;
    if (request->operation == USBPHY::Operation::Enable) {
        if (s.lease || fPinned) { status = kIOReturnBusy; goto out; }
        if (!s.superSpeed && request->orientation != 1) { status = kIOReturnBadArgument; goto out; }
        if (s.serial == UINT64_MAX) { status = kIOReturnNoResources; goto out; }
        s.lease = ++s.serial; s.leaseOwner = client; client->retain();
        s.role=USB::Role(request->role);
        status = s.enable(USBPHY::Orientation(request->orientation));
        if (status != kIOReturnSuccess) {
            const auto cleanup = s.release();
            if (cleanup == kIOReturnSuccess) { releaseOwner = s.leaseOwner; s.leaseOwner = nullptr; s.lease = 0; }
            else { pin(); status = cleanup; }
        }
        request->lease = s.lease;
        setProperty("USBPhyReady", status == kIOReturnSuccess);
    } else {
        if (s.leaseOwner != client || request->lease != s.lease || request->role!=unsigned(s.role)) { status = kIOReturnBadArgument; goto out; }
        if (request->operation == USBPHY::Operation::Check) {
            status = (s.superSpeed ? s.usb3->ready() : s.usb2->ready()) ? kIOReturnSuccess : kIOReturnNotReady;
        } else {
            status = s.release();
            setProperty("USBPhyReady", false);
            if (status == kIOReturnSuccess) {
                releaseOwner = s.leaseOwner; s.leaseOwner = nullptr; s.lease = 0;
                if (fPinned) { fPinned = false; unpin = true; setProperty("USBPhyFault", false); }
            }
            else pin();
        }
    }
out:
    IOLockUnlock(s.lock);
    if (releaseOwner) releaseOwner->release();
    if (unpin) release();
    return status;
}
bool QcomUSBPHY::requestTerminate(IOService *, IOOptionBits) {
    if (!fState || !fState->lock) return !fPinned;
    auto &s = *fState; IOLockLock(s.lock);
    const bool allowed = !fPinned && !s.client && !s.lease;
    IOLockUnlock(s.lock); return allowed;
}
void QcomUSBPHY::stop(IOService *provider) {
    if (fState && fState->lock) {
        auto &s = *fState; IOLockLock(s.lock);
        s.stopping = true;
        if (s.client || s.lease) { pin(); IOLockUnlock(s.lock); return; }
        IOLockUnlock(s.lock);
        if (s.opened) { s.provider->close(this); s.opened = false; }
    }
    if (fStarted) { IOService::stop(provider); fStarted = false; }
}
void QcomUSBPHY::free() {
    if (fState) {
        stop(fState->provider);
        if (fPinned) return;
        auto *s = fState; fState = nullptr;
        delete s->usb3; delete s->usb2; delete s->io; delete s->calibration;delete s->clampIO;
        for (auto *map : s->maps) if (map) map->release();
        OSSafeReleaseNULL(s->function); OSSafeReleaseNULL(s->provider); OSSafeReleaseNULL(s->bus);
        if (s->lock) IOLockFree(s->lock);
        delete s;
    }
    IOService::free();
}
