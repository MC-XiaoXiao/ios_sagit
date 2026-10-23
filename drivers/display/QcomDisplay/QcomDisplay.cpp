/* SPDX-License-Identifier: BSD-2-Clause */
#include "QcomDisplay.h"
#include "QcomDisplayMapper.h"
#include "DisplaySMMU.h"
#include "DisplayCommandTransfer.h"
#include <AppleARMPlatform/ARMDisplayProperties.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSBoolean.h>
#include <IOKit/IOUserClient.h>
#include <kern/task.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <mach/mach_time.h>

#define super IOMobileFramebufferLegacy
// Only callers already guarded by takeDiagnostic use this route. Normal
// operation retains IOLog; explicit capture boots can inspect the bounded
// native queue diagnostics while SERIALMODE_NO_IOLOG remains enabled.
#define DISPLAY_NATIVE_LOG(...) do { if (nativeTraceEnabled) kprintf(__VA_ARGS__); else IOLog(__VA_ARGS__); } while (false)
OSDefineMetaClassAndStructors(QcomDisplay, super);
static bool number(OSDictionary *, const char *, UInt64, unsigned);

static bool takeDiagnostic(UInt32 &count, UInt32 limit = 4) {
    UInt32 previous = __atomic_load_n(&count, __ATOMIC_RELAXED);
    while (previous < limit) {
        if (__atomic_compare_exchange_n(&count, &previous, previous + 1, false,
                __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return true;
    }
    return false;
}

uint32_t QcomDisplay::Registers::read(uint32_t offset) const {
    uint32_t value = *reinterpret_cast<volatile uint32_t *>(base + offset);
    __asm__ volatile("dmb oshld" ::: "memory"); return value;
}
void QcomDisplay::Registers::write(uint32_t offset, uint32_t value) {
    __asm__ volatile("dmb oshst" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
    __asm__ volatile("dsb oshst" ::: "memory");
}

bool QcomDisplay::configure(IOService *p) {
    if (!displayProperty32(p, "qcom,display-handoff-version", contract.version) ||
        !displayProperty32(p, "qcom,display-handoff-flags", contract.flags) ||
        !displayProperty32(p, "qcom,display-interface", contract.interface) ||
        !displayProperty32(p, "qcom,display-control", contract.control) ||
        !displayProperty32(p, "qcom,display-source", contract.source) ||
        !displayProperty32(p, "qcom,display-vsync-bit", contract.interruptBit) ||
        !displayProperty32(p, "display-width", mode.width) ||
        !displayProperty32(p, "display-height", mode.height) ||
        !displayProperty32(p, "display-row-bytes", mode.rowBytes) ||
        !displayProperty32(p, "display-refresh-hz", mode.refreshHz) ||
        !displayProperty32(p, "dot-pitch", pixelsPerInch) || pixelsPerInch < 20 || pixelsPerInch > 1000 ||
        !displayProperty64(p, "framebuffer-address", mode.framebufferAddress) ||
        !displayProperty64(p, "framebuffer-size", mode.framebufferSize) || !mode.valid() ||
        !displayProperty64(p, "qcom,scanout-device-address", scanoutDeviceAddress) ||
        scanoutDeviceAddress > UINT32_MAX || (scanoutDeviceAddress & 4095) ||
        (p->getDeviceMemoryCount() != 1 && p->getDeviceMemoryCount() != 3)) return false;
    if (contract.commandMode() && !displayProperty32(p, "qcom,display-pingpong", contract.pingPong)) return false;
    if (!contract.valid()) return false;
    mdpMap = p->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
    if (!mdpMap || mdpMap->getLength() < 0x8f000) return false;
    regs.base = reinterpret_cast<volatile uint8_t *>(mdpMap->getVirtualAddress());
    // Admit the already configured firmware raster only. The scanout address
    // may be an IOVA: compare it with the explicit device address, and map only
    // the reserved physical carveout supplied by the loader for CPU access.
    uint32_t source = contract.source == 0 ? 0x4000 : 0x24000;
    uint32_t size = mode.width | mode.height << 16;
    if (regs.read(source) != size || regs.read(source + 0x0c) != size ||
        regs.read(source + 0x08) || regs.read(source + 0x10) ||
        !Display::linearBGRA(regs.read(source + 0x30), regs.read(source + 0x34), regs.read(source + 0x38)) ||
        regs.read(source + 0x14) != scanoutDeviceAddress ||
        (regs.read(source + 0x24) & 0xffff) != mode.rowBytes ||
        (!contract.commandMode() && !regs.read(0x6a000 + 0x800 * contract.interface)) ||
        (regs.read(0x1018) & (contract.source == 0 ? 1 : 1u << 11)) ||
        !!(regs.read(0x1014) & (1u << 17)) != contract.commandMode()) return false;
    if (contract.commandMode()) {
        uint32_t pp = 0x70000 + 0x800 * contract.pingPong;
        commandAutoRefresh = regs.read(pp + 0x30) & (1u << 31);
        if ((!regs.read(pp) && !commandAutoRefresh) || !(regs.read(pp + 4) & (1u << 19))) return false;
    }
    doubleBuffered = contract.flags & Display::PlatformContract::DoubleBufferMapped;
    if (doubleBuffered && (mode.framebufferSize < 2 * Display::bufferSpan(mode) ||
            scanoutDeviceAddress > UINT32_MAX - 2 * Display::bufferSpan(mode) + 1)) return false;
    scanoutMemory = IODeviceMemory::withRange(mode.framebufferAddress, mode.framebufferSize);
    if (!scanoutMemory) return false;
    scanoutMap = scanoutMemory->map(kIOMapWriteCombineCache);
    if (!scanoutMap || scanoutMap->getLength() < mode.framebufferSize) return false;
    int direct = 0;
    PE_parse_boot_argn("display-direct-scanout", &direct, sizeof(direct));
    directRequested = direct == 1;
    // An optional fast path must not wait indefinitely on an unresolved
    // phandle. A display mapper publishes its actual object before matching.
    // No global/GPU mapper or physical-address fallback is accepted here.
    if (directRequested && OSDynamicCast(IOMapper, p->getProperty("iommu-parent")))
        displayMapper = IOMapper::copyMapperForDevice(p);
    directReady = displayMapper != nullptr;
    setProperty("DisplayDirectScanoutRequested", directRequested);
    setProperty("DisplayDirectScanoutMapperAvailable", directReady);
    setProperty("DisplayDirectScanoutActive", false);
    uint32_t captureStatus = 0;
    displayProperty32(p, Display::MMU::StatusProperty, captureStatus);
    setProperty("DisplayMapperCaptureStatus", captureStatus, 32);
    if (p->getProperty("display-panel")) {
        uint32_t identifier;
        if (!displayProperty32(p, "display-panel", identifier) || !identifier) return false;
        OSDictionary *matching = serviceMatching("QcomPanel");
        OSDictionary *properties = OSDictionary::withCapacity(1);
        if (!matching || !properties) { OSSafeReleaseNULL(matching); OSSafeReleaseNULL(properties); return false; }
        bool valid = number(properties, "DisplayPanelIdentifier", identifier, 32) &&
            matching->setObject("IOPropertyMatch", properties);
        if (valid) {
            panel = waitForMatchingService(matching, 5000000000ULL);
        }
        properties->release(); matching->release();
        if (!panel) return false;
        panelOpened = panel->open(this);
        if (!panelOpened || panel->callPlatformFunction("MIPIPanelPrepare", true,
                nullptr, nullptr, nullptr, nullptr) != kIOReturnSuccess) return false;
    }
    return configureBacklight(p);
}

bool QcomDisplay::configureBacklight(IOService *p) {
    if (!p->getProperty("display-backlight")) return true;
    if (!displayProperty32(p, "display-backlight", backlightIdentifier) || !backlightIdentifier) return false;
    auto *matching = serviceMatching("IOService");
    auto *properties = OSDictionary::withCapacity(2);
    if (!matching || !properties) { OSSafeReleaseNULL(matching); OSSafeReleaseNULL(properties); return false; }
    bool valid = number(properties, "DisplayBacklightIdentifier", backlightIdentifier, 32) &&
        number(properties, "DisplayBacklightProtocolVersion", 1, 32) &&
        matching->setObject("IOPropertyMatch", properties);
    if (valid) backlight = waitForMatchingService(matching, 5000000000ULL);
    properties->release(); matching->release();
    if (!backlight) {
        IOLog("QcomDisplay: backlight service unavailable identifier=%u; framebuffer not started\n", backlightIdentifier);
        return false;
    }
    backlightClaimed = backlightRequest(DisplayBacklight::Claim) == kIOReturnSuccess;
    if (!backlightClaimed)
        IOLog("QcomDisplay: backlight claim failed identifier=%u; framebuffer not started\n", backlightIdentifier);
    return backlightClaimed;
}
IOReturn QcomDisplay::backlightRequest(DisplayBacklight::Operation operation, uint32_t value) {
    if (!backlight) return kIOReturnUnsupported;
    DisplayBacklight::Request request;
    request.operation = operation; request.identifier = backlightIdentifier; request.value = value;
    auto result = backlight->callPlatformFunction(DisplayBacklight::Function, true, &request, this, nullptr, nullptr);
    if (operation != DisplayBacklight::Release) {
        setProperty("BacklightStateValid", result == kIOReturnSuccess && request.valid);
        setProperty("BacklightLastError", uint32_t(result), 32);
        if (result == kIOReturnSuccess) {
            setProperty("DisplayBrightness", request.brightness, 32);
            setProperty("DisplayAppliedBrightness", request.appliedBrightness, 32);
            setProperty("DisplayBacklightPower", bool(request.power));
        }
    }
    return result;
}

bool QcomDisplay::start(IOService *provider) {
    // The native init() owns the framework work loop and command gate. Never
    // replace these with a second framebuffer queue or a private user client.
    swapValidationLogs = swapAcceptedLogs = surfaceValidationLogs = swapBeginLogs = powerStateLogs = 0;
    scanoutCaptureGeneration = 0;
    swapSampleHead=swapSampleCount=0;displayedSwapCount=failedSwapCount=0;
    int capture = 0;
    PE_parse_boot_argn("display-scanout-capture", &capture, sizeof(capture));
    scanoutCaptureEnabled = capture == 1;
    int trace = 0;
    PE_parse_boot_argn("display-submit-trace", &trace, sizeof(trace));
    nativeTraceEnabled = scanoutCaptureEnabled && trace == 1;
    opened = provider->open(this);
    if (!opened || !configure(provider)) {
        IOLog("QcomDisplay: display resource initialization failed\n");
        releaseResources(); return false;
    }
    resourcesReady = true;
    nativeStarted = true;
    if (!super::start(provider)) {
        stopFramework(provider);
        releaseResources(); return false;
    }
    if (!defaultSurface) {
        stopFramework(provider);
        releaseResources(); return false;
    }
    registerService();
    IOLog("QcomDisplay: iOS 18 IOMobileFramebufferLegacy, %ux%u @ %u Hz\n",
        mode.width, mode.height, mode.refreshHz);
    return true;
}

bool QcomDisplay::start_hardware() {
    IOLog("QcomDisplay: start_hardware ready=%u surface-root=%p\n", resourcesReady, nativeState<IOSurfaceRoot *>(SurfaceRoot));
    if (!resourcesReady) return false;
    nativeState<UInt32>(NativeWidth) = mode.width;
    nativeState<UInt32>(NativeHeight) = mode.height;
    nativeState<UInt32>(NativePixelFormat) = Display::BGRA;
    // The native initializer owns both cached swap transaction templates.
    // Run it before enabling our framebuffer: its later allocation branch
    // requires Apple's PurpleGfxMem region, while our surface uses ordinary
    // IOSurface memory. Publish readiness only after our setup succeeds.
    nativeState<UInt8>(DisplayReady) = 0;
    super::create_default_fb_surface();
    const size_t templateOffsets[] = {DefaultTransaction, EmptyTransaction};
    for (size_t offset : templateOffsets) {
        UInt32 changed = nativeState<UInt32>(offset + 0x14c);
        UInt32 ready = nativeState<UInt32>(offset + 0x150);
        if (changed != ~UInt32(0) || ready != ~UInt32(0)) {
            IOLog("QcomDisplay: native swap template initialization failed offset=%lx changed=%x ready=%x\n",
                static_cast<unsigned long>(offset), changed, ready);
            return false;
        }
    }
    setProperty("IOMFBDefaultSwapTemplatesInitialized", true);
    if (scanoutCaptureEnabled)
        kprintf("QcomDisplay: native swap templates initialized count=2 changed=ffffffff ready=ffffffff\n");
    nativeState<UInt8>(MainDisplay) = 1;
    create_default_fb_surface();
    if (!defaultSurface) { IOLog("QcomDisplay: default IOSurface creation failed\n"); return false; }
    IOWorkLoop *loop = getWorkLoop();
    if (!loop) { IOLog("QcomDisplay: native work loop unavailable\n"); return false; }
    frameworkLoop = loop;
    frameworkLoop->retain();
    gate = IOCommandGate::commandGate(this);
    if(scanoutCaptureEnabled)timeline=IONewZero(Display::TimelineRing,1);
    interrupt = timeline ? IOFilterInterruptEventSource::filterInterruptEventSource(this,interruptAction,interruptFilter,getProvider(),0) :
        IOInterruptEventSource::interruptEventSource(this, interruptAction, getProvider(), 0);
    timeout = IOTimerEventSource::timerEventSource(this, timeoutAction);
    if (!loop || !gate || !interrupt || !timeout) { IOLog("QcomDisplay: event-source allocation failed\n"); return false; }
    if (loop->addEventSource(gate) != kIOReturnSuccess) return false;
    gateAdded = true;
    if (loop->addEventSource(interrupt) != kIOReturnSuccess) return false;
    interruptAdded = true;
    if (loop->addEventSource(timeout) != kIOReturnSuccess) return false;
    timerAdded = true;
    if (contract.commandMode()) {
        Display::CommandTransfer<Registers> transfer(regs,contract.pingPong,mode.height);
        if (!transfer.adopt([](unsigned us){IODelay(us);})) {
            IOLog("QcomDisplay: firmware command transfer did not drain; native scanout disabled\n");
            return false;
        }
        commandAutoRefresh = false;
        commandSingleTransfer = true;
        setProperty("DisplayCommandSingleTransfer",true);
        if (scanoutCaptureEnabled)
            kprintf("QcomDisplay: command single-transfer enabled pp=%u auto=0 tear=1 drained=1\n",contract.pingPong);
    }
    if (directRequested && !displayMapper) {
        ownedMapper = QcomDisplayMapper::create(this, getProvider(), mode, scanoutDeviceAddress);
        if (ownedMapper) displayMapper = IOMapper::copyMapperForDevice(getProvider());
        directReady = displayMapper != nullptr;
        setProperty("DisplayDirectScanoutMapperAvailable", directReady);
        if (!directReady) IOLog("QcomDisplay: MMSS handoff unavailable; using reserved staging\n");
    }
    // Native start() publishes the service before returning. Its matching
    // clients must see identity/capabilities on their first notification.
    if (!publishDisplayProperties()) { IOLog("QcomDisplay: display property publication failed\n"); return false; }
    savedInterruptEnable = regs.read(Display::InterruptEnable);
    uint32_t bit = contract.ownedInterrupts();
    regs.write(Display::InterruptClear, bit);
    regs.write(Display::InterruptEnable, savedInterruptEnable | bit);
    irqEnabled = true;
    displayVisible = true;
    interrupt->enable();
    // The final native iOS 18 display subclasses register PM themselves;
    // IOMobileFramebufferLegacy does not do this for its hardware subclass.
    // Both states retain the handed-off raster/rails/clocks. State 0 applies
    // WLED blanking; it cannot stop DMA or cancel a latched native transaction.
    nativeState<UInt8>(NativePMInitialized) = 1;
    PMinit(); pmStarted = true;
    static IOPMPowerState states[2] = {
        {1, 0, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
    };
    getProvider()->joinPMtree(this);
    IOReturn powerStatus = registerPowerDriver(this, states, 2);
    if (powerStatus != kIOReturnSuccess) {
        IOLog("QcomDisplay: power registration failed result=%x\n", powerStatus);
        return false;
    }
    nativeState<UInt8>(DisplayReady) = 1;
    changePowerStateTo(1);
    return true;
}

static bool number(OSDictionary *d, const char *key, UInt64 value, unsigned bits = 64) {
    OSNumber *n = OSNumber::withNumber(value, bits);
    bool ok = n && d->setObject(key, n);
    OSSafeReleaseNULL(n); return ok;
}
IOReturn QcomDisplay::setPowerState(unsigned long state, IOService *device) {
    if (state > 1) return device ? kIOPMNoSuchState : kIOReturnBadArgument;
    IOReturn result = super::setPowerState(state, device);
    // IOPM interprets positive values as acknowledgement delays, not IOReturn
    // errors. Complete that callback on failure and preserve the actual error
    // and unchanged native current fields. Direct display callers get errors.
    if (device && (uint32_t(result) & 0xf0000000u) == 0xe0000000u) {
        setProperty("DisplayPowerTransitionError", uint32_t(result), 32);
        IOLog("QcomDisplay: IOPM transition failed (%x); hardware current state retained\n", result);
        return kIOPMAckImplied;
    }
    return result;
}
void QcomDisplay::setDisplayVisible(bool enable) {
    // This PM profile only blanks WLED. TE and transfer completion remain
    // live in both states; single-transfer mode separately ends DMA reads.
    // A dark panel is not a DMA-stop proof, and does not cancel an in-flight
    // source latch, its native completion, or the producer's surface lease.
    displayVisible = enable;
    setProperty("DisplayVisible", enable);
}
IOReturn QcomDisplay::do_power_state_change_gated() {
    if (!resourcesReady || !scanoutMap || !interrupt || !timeout) return kIOReturnNotReady;
    if (nativeState<UInt8>(0x18a9) == 1) return kIOReturnSuccess; // Native ignore-power policy.
    UInt32 controller = nativeState<UInt32>(RequestedControllerPower);
    UInt32 display = nativeState<UInt32>(RequestedDisplayPower);
    if (controller > 1 || display > 1) return kIOReturnUnsupported;
    recordTimeline(Display::TimelineKind::PowerRequested,0,nullptr,(UInt64(controller)<<32)|display);
    bool on = controller && display;
    if (!controller || !display) {
        if (!backlight) {
            if (!nativeState<UInt32>(CurrentControllerPower) && !nativeState<UInt32>(CurrentDisplayPower)) return kIOReturnSuccess;
            return kIOReturnUnsupported;
        }
    }
    if (on && (directQuarantined || !retainedRasterValid() || (!backlight && !frames))) return kIOReturnNotReady;
    if (on && !displayVisible && !swapPending && doubleBuffered && !frontSurface) {
        uint32_t source = contract.source == 0 ? 0x4000 : 0x24000;
        uint32_t flush = contract.source == 0 ? 1 : 1u << 11;
        if (regs.read(0x1018) & flush) return kIOReturnBusy;
        uint64_t offset = regs.read(source + 0x14) - scanoutDeviceAddress;
        if (offset && offset != Display::bufferSpan(mode)) return kIOReturnNotReady;
        frontOffset = offset; // An aborted pre-sleep latch may have completed.
    }
    if (backlight) {
        auto result = backlightRequest(DisplayBacklight::SetPower, on);
        if (result != kIOReturnSuccess) {
            IOLog("QcomDisplay: backlight power transition failed controller=%u display=%u result=%x\n", controller, display, result);
            return result;
        }
        setDisplayVisible(on);
    }
    // Commit native current states and notification only after hardware ACK
    // and readback. The controller state describes software usability while
    // its firmware resources stay retained. A disabled controller is dark.
    UInt32 effectiveDisplay = on ? 1 : 0;
    if (nativeState<UInt32>(CurrentControllerPower) != controller ||
        nativeState<UInt32>(CurrentDisplayPower) != effectiveDisplay) {
        nativeState<UInt32>(CurrentControllerPower) = controller;
        nativeState<UInt32>(CurrentDisplayPower) = effectiveDisplay;
        recordTimeline(Display::TimelineKind::PowerApplied,0,nullptr,(UInt64(controller)<<32)|effectiveDisplay);
        nativeNotifyPowerState(controller, effectiveDisplay);
        IOLog("QcomDisplay: retained display controller=%u display=%u, backlight=%u\n", controller, effectiveDisplay, bool(backlight));
    }
    if (on && !swapPending && nativeState<void *>(PendingRequest)) swap_begin_gated();
    if (nativeTraceEnabled && takeDiagnostic(powerStateLogs, 24))
        DISPLAY_NATIVE_LOG("QcomDisplay: power requested=%u,%u current=%u,%u visible=%u irq=%u pending=%u active=%u queued=%u retained-raster=1\n",
            controller, display, nativeState<UInt32>(CurrentControllerPower), nativeState<UInt32>(CurrentDisplayPower),
            displayVisible, irqEnabled, swapPending, nativeState<void *>(ActiveRequest) != nullptr,
            nativeState<void *>(PendingRequest) != nullptr);
    setProperty("DisplayPowerTransitionError", 0ULL, 32);
    return kIOReturnSuccess;
}
bool QcomDisplay::retainedRasterValid() const {
    const auto &r = regs;
    uint32_t source = contract.source == 0 ? 0x4000 : 0x24000;
    uint32_t size = mode.width | mode.height << 16;
    uint64_t address = r.read(source + 0x14);
    uint32_t rowBytes = r.read(source + 0x24) & 0xffff;
    uint64_t span = uint64_t(rowBytes) * mode.height;
    uint32_t routing = r.read(0x1014);
    if (((routing >> 4) & 7) != contract.interface + 1 ||
        bool(routing & (1u << 17)) != contract.commandMode()) return false;
    bool mapped = (frontSurface && frontSurface->matches(address, rowBytes)) ||
        (stagedExposed && stagedSurface && stagedSurface->matches(address, rowBytes));
    if (!mapped && (rowBytes != mode.rowBytes || address < scanoutDeviceAddress ||
            address - scanoutDeviceAddress > mode.framebufferSize ||
            span > mode.framebufferSize - (address - scanoutDeviceAddress))) return false;
    if (r.read(source) != size || r.read(source + 0x0c) != size ||
            r.read(source + 0x08) || r.read(source + 0x10) ||
            !Display::linearBGRA(r.read(source + 0x30), r.read(source + 0x34), r.read(source + 0x38))) return false;
    if (contract.commandMode()) {
        uint32_t pp = 0x70000 + 0x800 * contract.pingPong;
        if (bool(r.read(pp + 0x30) & (1u << 31)) != commandAutoRefresh) return false;
        if ((!r.read(pp) && !(r.read(pp + 0x30) & (1u << 31))) || !(r.read(pp + 4) & (1u << 19))) return false;
    } else if (!r.read(0x6a000 + 0x800 * contract.interface)) return false;
    return true;
}
IOReturn QcomDisplay::brightnessAction(OSObject *owner, void *value, void *, void *, void *) {
    auto *self = static_cast<QcomDisplay *>(owner);
    if (!self->resourcesReady || !self->backlightClaimed) return kIOReturnNotReady;
    return self->backlightRequest(DisplayBacklight::SetBrightness, uint32_t(reinterpret_cast<uintptr_t>(value)));
}
IOReturn QcomDisplay::setProperties(OSObject *object) {
    auto properties = OSDynamicCast(OSDictionary, object);
    if(properties && properties->getObject("DisplayTimeline")) {
        if(!timeline)return kIOReturnUnsupported;
        if(properties->getCount()!=1 || properties->getObject("DisplayTimeline")!=kOSBooleanTrue)return kIOReturnBadArgument;
        if(IOUserClient::clientHasPrivilege(current_task(),kIOClientPrivilegeAdministrator)!=kIOReturnSuccess)return kIOReturnNotPrivileged;
        return gate && gateAdded?gate->runAction(timelineAction):kIOReturnNotReady;
    }
    if(properties && properties->getObject("DisplayLeaseHistory")) {
        if(!scanoutCaptureEnabled)return kIOReturnUnsupported;
        if(properties->getCount()!=1 || properties->getObject("DisplayLeaseHistory")!=kOSBooleanTrue)return kIOReturnBadArgument;
        if(IOUserClient::clientHasPrivilege(current_task(),kIOClientPrivilegeAdministrator)!=kIOReturnSuccess)return kIOReturnNotPrivileged;
        return gate && gateAdded?gate->runAction(leaseHistoryAction):kIOReturnNotReady;
    }
    if(properties && properties->getObject("DisplayFrameStatistics")) {
        if(!scanoutCaptureEnabled)return kIOReturnUnsupported;
        if(properties->getCount()!=1 || properties->getObject("DisplayFrameStatistics")!=kOSBooleanTrue)return kIOReturnBadArgument;
        if(IOUserClient::clientHasPrivilege(current_task(),kIOClientPrivilegeAdministrator)!=kIOReturnSuccess)return kIOReturnNotPrivileged;
        return gate && gateAdded?gate->runAction(swapStatisticsAction):kIOReturnNotReady;
    }
    if (properties && properties->getObject("DisplayBrightness")) {
        auto *value = OSDynamicCast(OSNumber, properties->getObject("DisplayBrightness"));
        if (properties->getCount() != 1 || !value || value->unsigned64BitValue() > 4095) return kIOReturnBadArgument;
        if (!backlight) return kIOReturnUnsupported;
        if (IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
            return kIOReturnNotPrivileged;
        return gate && gateAdded ? gate->runAction(brightnessAction,
            reinterpret_cast<void *>(uintptr_t(value->unsigned32BitValue()))) : kIOReturnNotReady;
    }
    if (!properties || !properties->getObject("DisplayScanoutCapture")) return super::setProperties(object);
    if (!scanoutCaptureEnabled) return kIOReturnUnsupported;
    if (properties->getCount() != 1 || properties->getObject("DisplayScanoutCapture") != kOSBooleanTrue)
        return kIOReturnBadArgument;
    if (IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    if (!gate || !gateAdded) return kIOReturnNotReady;
    return gate->runAction(captureScanoutAction);
}
IOReturn QcomDisplay::captureScanoutAction(OSObject *owner, void *, void *, void *, void *) {
    return static_cast<QcomDisplay *>(owner)->captureScanoutGated();
}
IOReturn QcomDisplay::swapStatisticsAction(OSObject *owner,void *,void *,void *,void *) {
    auto *self=static_cast<QcomDisplay *>(owner);
    Display::SwapStatisticsHeader h;h.count=self->swapSampleCount;h.displayed=self->displayedSwapCount;h.failed=self->failedSwapCount;
    h.nowTicks=mach_continuous_time();absolutetime_to_nanoseconds(h.timebaseSpanTicks,&h.timebaseSpanNanoseconds);
    auto *data=OSData::withCapacity(sizeof(h)+h.count*sizeof(Display::SwapSample));
    if(!data)return kIOReturnNoMemory;
    bool ok=data->appendBytes(&h,sizeof(h));
    for(uint32_t i=0;ok && i<h.count;++i) {
        const auto &sample=self->swapSamples[(self->swapSampleHead+i)%Display::SwapSampleCapacity];
        ok=data->appendBytes(&sample,sizeof(sample));
    }
    if(ok)ok=self->setProperty("DisplayFrameStatistics",data);
    data->release();
    if(ok) {
        auto snapshot=self->preparation;snapshot.nowTicks=h.nowTicks;snapshot.displayed=h.displayed;snapshot.hardwareVBL=self->frames;
        auto *prepared=OSData::withBytes(&snapshot,sizeof(snapshot));
        if(prepared){self->setProperty("DisplayPrepareStatistics",prepared);prepared->release();}
        Display::MappingCacheStatistics cache;cache.nowTicks=h.nowTicks;cache.displayed=h.displayed;
        cache.hits=self->mappingCacheHits;cache.misses=self->mappingCacheMisses;cache.evicted=self->mappingCacheEvictions;cache.cached=self->idleMappings.count();
        self->setProperty("DisplayMappingCacheStatistics",&cache,sizeof(cache));
    }
    return ok?kIOReturnSuccess:kIOReturnNoMemory;
}
void QcomDisplay::recordTimeline(Display::TimelineKind kind,uint32_t status,void *request,uint64_t auxiliary) {
    if(!timeline)return;
    const auto ticks=mach_absolute_time();
    uint32_t token=0;if(request)memcpy(&token,static_cast<UInt8 *>(request)+0x314,4);
    const auto pp=0x70000+0x800*contract.pingPong;
    const auto source=contract.source==0?0x4000:0x24000;
    timeline->append(Display::TimelineSample::make(ticks,reinterpret_cast<uintptr_t>(request),kind,token,status,
        regs.read(0x1018),regs.read(0x101c),regs.read(pp+0x14),regs.read(pp+0x2c),regs.read(source+0x14),auxiliary));
}
bool QcomDisplay::interruptFilter(OSObject *owner,IOFilterInterruptEventSource *) {
    auto *self=static_cast<QcomDisplay *>(owner);
    // Diagnostic primary context only: no clear, native calls, gate, pointers
    // into requests, logging, or policy changes. Preserve normal scheduling.
    const auto status=self->regs.read(Display::InterruptStatus)&self->contract.ownedInterrupts();
    if(status)self->recordTimeline(Display::TimelineKind::IRQ,status);
    return true;
}
IOReturn QcomDisplay::timelineAction(OSObject *owner,void *,void *,void *,void *) {
    auto *self=static_cast<QcomDisplay *>(owner);if(!self->timeline || !self->resourcesReady)return kIOReturnNotReady;
    Display::TimelineHeader h;h.total=self->timeline->total();h.dropped=self->timeline->losses();
    h.firstSerial=h.total>Display::TimelineCapacity?h.total-Display::TimelineCapacity:0;
    h.nowTicks=mach_absolute_time();absolutetime_to_nanoseconds(h.timebaseTicks,&h.timebaseNS);
    auto *data=OSData::withCapacity(sizeof(h)+(h.total-h.firstSerial)*sizeof(Display::TimelineSample));
    if(!data)return kIOReturnNoMemory;
    bool ok=data->appendBytes(&h,sizeof(h));
    for(uint64_t n=h.firstSerial;ok && n<h.total;++n) {
        Display::TimelineSample sample;
        if(!self->timeline->read(n,sample)){++h.unavailable;continue;}
        ok=data->appendBytes(&sample,sizeof(sample));++h.count;
    }
    const auto pp=0x70000+0x800*self->contract.pingPong;
    h.tear=self->regs.read(pp);h.sync=self->regs.read(pp+4);h.height=self->regs.read(pp+8);
    h.writeInit=self->regs.read(pp+0xc);h.readInit=self->regs.read(pp+0x10);h.threshold=self->regs.read(pp+0x18);
    h.start=self->regs.read(pp+0x1c);h.readIRQ=self->regs.read(pp+0x20);h.writeIRQ=self->regs.read(pp+0x24);h.autoRefresh=self->regs.read(pp+0x30);
    if(ok) {memcpy(const_cast<void *>(data->getBytesNoCopy()),&h,sizeof(h));ok=self->setProperty("DisplayTimeline",data);}
    data->release();return ok?kIOReturnSuccess:kIOReturnNoMemory;
}
void QcomDisplay::recordLease(Display::LeasePhase phase,void *request) {
    if(!scanoutCaptureEnabled || leaseFrozenAt)return;
    // Called on the native display work loop. Its request/displayed references
    // protect the surfaces; copy fence identities without acquiring their locks.
    // No per-frame allocation, logging, or GPU/display state mutation.
    Display::LeaseSample s{};
    s.phase=uint32_t(phase);s.request=reinterpret_cast<uintptr_t>(request);
    s.active=reinterpret_cast<uintptr_t>(nativeState<void *>(ActiveRequest));
    s.queued=reinterpret_cast<uintptr_t>(nativeState<void *>(PendingRequest));
    s.front=frontSurface?frontSurface->surface()->nativeID():0;
    s.staged=stagedSurface?stagedSurface->surface()->nativeID():0;
    s.lastCompleted=nativeState<UInt32>(LastCompletedSwap);s.lastDisplayed=nativeState<UInt32>(LastDisplayedSwap);
    s.flags=uint32_t(displayVisible)|(frontSurface?2u:0u)|(swapPending?4u:0u)|(pipelineFaulted?8u:0u)|(directQuarantined?16u:0u);
    auto bytes=static_cast<const UInt8 *>(request);
    if(bytes) {
        memcpy(&s.token,bytes+0x314,4);memcpy(&s.intent,bytes+RequestIntent,4);
        memcpy(&s.changed,bytes+ChangedLayers,4);memcpy(&s.ready,bytes+ReadyLayers,4);
        memcpy(&s.state,bytes+0x34c,4);s.cancelled=bytes[0x54a];
    }
    for(unsigned layer=0;layer<4;++layer) {
        IOSurface *surface=nullptr;
        if(bytes) {
            memcpy(&surface,bytes+SurfaceArray+8*layer,sizeof(surface));s.surfaces[layer]=surface?surface->nativeID():0;
            // Effective pointers are inherited, not owned by this request.
            // A cancelled request may retain stale identities until pruning;
            // never dereference them. Prepare precedes native inheritance.
            if(phase!=Display::LeasePhase::Prepare) {
                memcpy(&s.effective[layer],bytes+EffectiveSurfaces+8*layer,8);
            }
            memcpy(&s.fences[layer],bytes+2600+8*layer,8);
        }
        surface=nativeState<IOSurface *>(5096+8*layer);s.displayed[layer]=surface?surface->nativeID():0;
        s.displayedFences[layer]=reinterpret_cast<uintptr_t>(nativeState<void *>(5288+8*layer));
        s.displayedTokens[layer]=UInt32(nativeState<UInt64>(5456+8*layer));
    }
    // Preserve the first sustained access wait before GPU timeout cleanup can
    // allow cached frames to replay and overwrite its ownership history. This
    // freezes diagnostics only; native readiness, deadlines and queues proceed.
    if(phase==Display::LeasePhase::Pending) {
        if(request && !s.active && !s.cancelled && s.changed!=s.ready) {
            const auto now=mach_absolute_time();
            if(leaseWaitRequest!=s.request || leaseWaitToken!=s.token || !leaseWaitStarted) {
                leaseWaitRequest=s.request;leaseWaitToken=s.token;leaseWaitStarted=now;
            } else {
                uint64_t ns=0;absolutetime_to_nanoseconds(now-leaseWaitStarted,&ns);
                if(ns>=1000000000ULL) {s.phase=uint32_t(Display::LeasePhase::Stalled);leaseFrozenAt=now;}
            }
        } else leaseWaitStarted=0;
    } else if(phase==Display::LeasePhase::Active) leaseWaitStarted=0;
    // An unchanged pending head can wait for seconds. Preserve the transition
    // history instead of replacing it with one identical sample per VBL.
    if(leaseSampleCount) {
        auto previous=leaseSamples[(leaseSampleHead+leaseSampleCount-1)%Display::LeaseHistoryCapacity];
        previous.ticks=0;if(!memcmp(&previous,&s,sizeof(s)))return;
    }
    s.ticks=mach_absolute_time();
    leaseSamples[(leaseSampleHead+leaseSampleCount)%Display::LeaseHistoryCapacity]=s;
    if(leaseSampleCount<Display::LeaseHistoryCapacity)++leaseSampleCount;
    else leaseSampleHead=(leaseSampleHead+1)%Display::LeaseHistoryCapacity;
    ++leaseSampleTotal;
}
IOReturn QcomDisplay::leaseHistoryAction(OSObject *owner,void *,void *,void *,void *) {
    auto *self=static_cast<QcomDisplay *>(owner);
    self->recordLease(Display::LeasePhase::Query,self->nativeState<void *>(PendingRequest));
    Display::LeaseHistoryHeader h;h.count=self->leaseSampleCount;h.total=self->leaseSampleTotal;
    h.owner=reinterpret_cast<uintptr_t>(self);
    h.frozenAtTicks=self->leaseFrozenAt;
    h.nowTicks=mach_absolute_time();absolutetime_to_nanoseconds(h.timebaseSpanTicks,&h.timebaseSpanNanoseconds);
    auto *data=OSData::withCapacity(sizeof(h)+h.count*sizeof(Display::LeaseSample));
    if(!data)return kIOReturnNoMemory;
    bool ok=data->appendBytes(&h,sizeof(h));
    for(uint32_t i=0;ok && i<h.count;++i)
        ok=data->appendBytes(&self->leaseSamples[(self->leaseSampleHead+i)%Display::LeaseHistoryCapacity],sizeof(Display::LeaseSample));
    if(ok)ok=self->setProperty("DisplayLeaseHistory",data);
    data->release();return ok?kIOReturnSuccess:kIOReturnNoMemory;
}
IOReturn QcomDisplay::captureScanoutGated() {
    // Read the retained mapping selected by the actual source register. A
    // direct front must be stable; its IOVA is never treated as a physical PA.
    // This diagnostic never writes pixels, routing or queue state.
    if (!scanoutCaptureEnabled || !resourcesReady || !scanoutMap || !irqEnabled) return kIOReturnNotReady;
    uint32_t source = contract.source == 0 ? 0x4000 : 0x24000;
    uint32_t address = regs.read(source + 0x14);
    uint32_t sourceSize = regs.read(source), outputSize = regs.read(source + 0x0c);
    uint32_t stride = regs.read(source + 0x24) & 0xffff;
    uint32_t format = regs.read(source + 0x30), unpack = regs.read(source + 0x34), operation = regs.read(source + 0x38);
    uint64_t bytes = uint64_t(stride) * mode.height;
    bool mapped = frontSurface && frontSurface->matches(address, stride);
    if ((frontSurface || stagedSurface) && (swapPending || stagedExposed)) return kIOReturnBusy;
    // The panel now owns the completed raster. Its former IOSurface can have
    // been reused by a producer, so reading that buffer is not a screenshot.
    if (mapped && commandSingleTransfer) return kIOReturnNotReady;
    if (sourceSize != (mode.width | mode.height << 16) || outputSize != sourceSize ||
            !Display::linearBGRA(format, unpack, operation) || !bytes || bytes > 16 * 1024 * 1024) return kIOReturnUnsupported;
    uint64_t offset = 0;
    if (!mapped) {
        if (stride != mode.rowBytes || address < scanoutDeviceAddress) return kIOReturnUnsupported;
        offset = address - scanoutDeviceAddress;
        if (offset > mode.framebufferSize || bytes > mode.framebufferSize - offset) return kIOReturnBadArgument;
    }
    auto snapshot = OSDictionary::withCapacity(32);
    if (!snapshot) return kIOReturnNoMemory;
    uint64_t timestamp = mach_absolute_time();
    uint32_t flushBefore = regs.read(0x1018);
    uint32_t mapperCapture = 0;
    displayProperty32(getProvider(), Display::MMU::StatusProperty, mapperCapture);
    __asm__ volatile("dsb osh" ::: "memory");
    auto pixels = mapped ? frontSurface->copyPixels() :
        OSData::withBytes(reinterpret_cast<const uint8_t *>(scanoutMap->getVirtualAddress()) + offset, unsigned(bytes));
    uint32_t addressAfter = regs.read(source + 0x14), flushAfter = regs.read(0x1018);
    bool ok = pixels && snapshot->setObject("Pixels", pixels) &&
        number(snapshot, "Version", 1, 32) && number(snapshot, "Generation", ++scanoutCaptureGeneration, 32) &&
        number(snapshot, "Width", mode.width, 32) && number(snapshot, "Height", mode.height, 32) &&
        number(snapshot, "RowBytes", stride, 32) && number(snapshot, "PixelFormat", Display::BGRA, 32) &&
        number(snapshot, "SourceAddress", address, 32) && number(snapshot, "SourceAddressAfter", addressAfter, 32) &&
        number(snapshot, "PhysicalAddress", mapped ? 0 : mode.framebufferAddress + offset) &&
        number(snapshot, "PhysicalAddressValid", !mapped, 32) &&
        number(snapshot, "DirectScanout", mapped, 32) &&
        number(snapshot, "DirectScanoutFrames", directFrames) &&
        number(snapshot, "DirectScanoutMapperAvailable", directReady, 32) &&
        number(snapshot, "DirectScanoutQuarantined", directQuarantined, 32) &&
        number(snapshot, "MapperCaptureStatus", mapperCapture, 32) &&
        number(snapshot, "SurfaceID", mapped ? frontSurface->surface()->nativeID() : 0, 32) &&
        number(snapshot, "Timestamp", timestamp) &&
        number(snapshot, "HardwareVBLCount", frames) && number(snapshot, "SourcePipe", contract.source, 32) &&
        number(snapshot, "SourceFormat", format, 32) && number(snapshot, "SourceUnpack", unpack, 32) &&
        number(snapshot, "SourceOperation", operation, 32) && number(snapshot, "ControlFlushBefore", flushBefore, 32) &&
        number(snapshot, "ControlFlushAfter", flushAfter, 32) &&
        number(snapshot, "InterruptStatus", regs.read(Display::InterruptStatus), 32) &&
        number(snapshot, "SwapPending", swapPending, 32) &&
        number(snapshot, "SourceSize", sourceSize, 32) && number(snapshot, "OutputSize", outputSize, 32) &&
        number(snapshot, "NativeState17E4", nativeState<UInt32>(0x17e4), 32) &&
        number(snapshot, "NativeState17E8", nativeState<UInt32>(0x17e8), 32) &&
        number(snapshot, "NativeState17EC", nativeState<UInt32>(0x17ec), 32) &&
        number(snapshot, "NativeState17F0", nativeState<UInt32>(0x17f0), 32) &&
        number(snapshot, "NativePMInitialized", nativeState<UInt8>(NativePMInitialized), 32) &&
        number(snapshot, "NativeLastCompletedSwap", nativeState<UInt32>(LastCompletedSwap), 32) &&
        number(snapshot, "NativeLastDisplayedSwap", nativeState<UInt32>(LastDisplayedSwap), 32) &&
        number(snapshot, "NativeActiveRequestPresent", nativeState<void *>(ActiveRequest) != nullptr, 32) &&
        number(snapshot, "NativeQueuedRequestPresent", nativeState<void *>(PendingRequest) != nullptr, 32) &&
        number(snapshot, "NativeVBLSubscriberPresent", nativeState<void *>(NativeVBLSubscribers) != nullptr, 32) &&
        number(snapshot, "NativeVBLCount", nativeState<UInt64>(NativeVBLCount)) &&
        number(snapshot, "NativeVBLTime", nativeState<UInt64>(NativeVBLTime)) &&
        number(snapshot, "NativeVBLDelta", nativeState<UInt64>(NativeVBLDelta)) &&
        number(snapshot, "DisplayIRQEnabled", irqEnabled, 32) &&
        number(snapshot, "DisplayVisible", displayVisible, 32) &&
        number(snapshot, "LastHardwareVBLTime", lastFrameTime) &&
        setProperty("DisplayScanoutSnapshot", snapshot);
    OSSafeReleaseNULL(pixels);
    snapshot->release();
    return ok ? kIOReturnSuccess : kIOReturnNoMemory;
}
void QcomDisplay::create_default_fb_surface() {
    if (defaultSurface || !resourcesReady) return;
    IOSurfaceRoot *root = nativeState<IOSurfaceRoot *>(SurfaceRoot);
    if (!root) return;
    OSDictionary *properties = OSDictionary::withCapacity(8);
    if (!properties) return;
    UInt32 stride = (mode.width * 4 + 63) & ~63u;
    bool ok = number(properties, "IOSurfaceWidth", mode.width) &&
        number(properties, "IOSurfaceHeight", mode.height) &&
        number(properties, "IOSurfaceBytesPerElement", 4) &&
        number(properties, "IOSurfaceBytesPerRow", stride) &&
        number(properties, "IOSurfaceAllocSize", UInt64(stride) * mode.height) &&
        number(properties, "IOSurfacePixelFormat", Display::BGRA, 32);
    if (ok) defaultSurface = root->createSurface(nullptr, properties);
    properties->release();
    if (!defaultSurface) { IOLog("QcomDisplay: native createSurface returned null\n"); return; }
    if (!validateSurface(defaultSurface)) {
        IOMemoryDescriptor *memory = defaultSurface->nativeMemoryDescriptor();
        IOLog("QcomDisplay: default surface invalid width=%llu height=%llu row=%llu format=%x alloc=%llu memory=%p length=%llu tiled=%u tile-format=%u\n",
            defaultSurface->nativeWidth(), defaultSurface->nativeHeight(), defaultSurface->nativeRowBytes(),
            defaultSurface->nativePixelFormat(), defaultSurface->nativeAllocationSize(), memory,
            memory ? memory->getLength() : 0, defaultSurface->nativeTiled(), defaultSurface->nativeTileFormat());
        OSSafeReleaseNULL(defaultSurface); return;
    }
    defaultSurface->retain(); // The native default-surface field owns a reference.
    nativeState<IOSurface *>(DefaultSurface) = defaultSurface;
    // The fixed canvas is already allocated here, so the parent's lazy
    // default_fb_surface_gated allocation branch will not populate its cached
    // transaction. Use the audited native submit layout: surface IDs at +0x9c,
    // packed source rectangles at +0xac and destination rectangles at +0x10c.
    // Native swap_submit resolves this ID to DefaultSurface and owns its refs.
    UInt32 identifier = defaultSurface->nativeID();
    int32_t rectangle[] = {0, 0, int32_t(mode.width), int32_t(mode.height)};
    auto transaction = reinterpret_cast<UInt8 *>(this) + DefaultTransaction;
    memcpy(transaction + 0x9c, &identifier, sizeof(identifier));
    memcpy(transaction + 0xac, rectangle, sizeof(rectangle));
    memcpy(transaction + 0x10c, rectangle, sizeof(rectangle));
}
UInt32 QcomDisplay::default_fb_surface_gated(UInt32 width, UInt32 height) {
    if (!defaultSurface || (width && width != mode.width) || (height && height != mode.height)) return 0;
    return defaultSurface->nativeID();
}
IOReturn QcomDisplay::deallocate_default_fb_surface_gated() {
    // The single fixed canvas remains allocated for the lifetime of this
    // resident display. Never let the native resize path remove its backing.
    return kIOReturnSuccess;
}
bool QcomDisplay::validateSurface(IOSurface *surface) const {
    if (!surface || surface->nativeProtection() || surface->nativeBaseOffset() ||
            surface->nativePlaneCount() > 1 || surface->nativeBytesPerElement() != 4 ||
            surface->nativeElementWidth() != 1 || surface->nativeElementHeight() != 1) return false;
    IOMemoryDescriptor *memory = surface->nativeMemoryDescriptor();
    if (!memory) return false;
    Display::Surface description{surface->nativeWidth(), surface->nativeHeight(),
        surface->nativeRowBytes(), surface->nativeAllocationSize(), memory->getLength(), surface->nativePixelFormat(),
        surface->nativeTiled(), surface->nativeTileFormat()};
    return description.valid(mode);
}
bool QcomDisplay::copySurface(IOSurface *surface, uint64_t offset) {
    if (!validateSurface(surface) || !scanoutMap) return false;
    if (offset > mode.framebufferSize || uint64_t(mode.rowBytes) * mode.height > mode.framebufferSize - offset) return false;
    IOMemoryDescriptor *memory = surface->nativeMemoryDescriptor();
    const UInt32 logLimit = nativeTraceEnabled ? 16 : 4;
    const UInt64 copyBegin = nativeTraceEnabled ? mach_absolute_time() : 0;
    // The CPU reads this surface. Match the native descriptor's read access;
    // kIODirectionIn would request writes and can reject read-only preparation.
    IOReturn prepared = memory->prepare(kIODirectionOut);
    if (prepared != kIOReturnSuccess) {
        if (takeDiagnostic(copyLogs, logLimit)) DISPLAY_NATIVE_LOG("QcomDisplay: source preparation failed result=%x\n", prepared);
        return false;
    }
    const UInt64 preparedAt = copyBegin ? mach_absolute_time() : 0;
    // Preparing pins the pages; it does not invalidate CPU cache lines left
    // by a noncoherent GPU writer. Synchronize the original prepared backing
    // before reading, preserving the original descriptor's cache policy.
    uint64_t stride = surface->nativeRowBytes();
    uint64_t sourceBytes = (uint64_t(mode.height) - 1) * stride + uint64_t(mode.width) * 4;
    IOReturn synchronized = memory->performOperation(kIOMemoryIncoherentIOFlush, 0, sourceBytes);
    if (synchronized != kIOReturnSuccess) {
        memory->complete(kIODirectionOut);
        if (takeDiagnostic(copyLogs, logLimit)) DISPLAY_NATIVE_LOG("QcomDisplay: source synchronization failed result=%x\n", synchronized);
        return false;
    }
    const UInt64 synchronizedAt = copyBegin ? mach_absolute_time() : 0;
    // Mapping the prepared original descriptor preserves its cache attributes
    // and avoids a physical-segment walk/copy for every raster row. Do not
    // impose a different cache mode on the IOSurface's existing mappings.
    auto sourceMap = memory->createMappingInTask(kernel_task, 0,
        kIOMapAnywhere | kIOMapReadOnly, 0, sourceBytes);
    const auto source = sourceMap && sourceMap->getLength() >= sourceBytes ?
        reinterpret_cast<const uint8_t *>(sourceMap->getVirtualAddress()) : nullptr;
    const UInt64 mappedAt = copyBegin ? mach_absolute_time() : 0;
    auto dest = reinterpret_cast<uint8_t *>(scanoutMap->getVirtualAddress()) + offset;
    bool copied = true;
    for (unsigned y = 0; y < mode.height; ++y) {
        if (source) {
            memcpy(dest + UInt64(y) * mode.rowBytes, source + UInt64(y) * stride, mode.width * 4);
        } else {
            // Some original descriptors cannot provide a kernel mapping.
            // Keep the checked descriptor read as their functional fallback.
            IOByteCount count = memory->readBytes(UInt64(y) * stride, dest + UInt64(y) * mode.rowBytes, mode.width * 4);
            if (count != mode.width * 4) {
                if (takeDiagnostic(copyLogs, logLimit)) DISPLAY_NATIVE_LOG("QcomDisplay: source read failed row=%u bytes=%llu expected=%u\n", y, UInt64(count), mode.width * 4);
                copied = false; break;
            }
        }
    }
    __asm__ volatile("dsb oshst" ::: "memory");
    const UInt64 copiedAt = copyBegin ? mach_absolute_time() : 0;
    OSSafeReleaseNULL(sourceMap);
    memory->complete(kIODirectionOut);
    if (takeDiagnostic(copyLogs, logLimit)) {
        DISPLAY_NATIVE_LOG("QcomDisplay: source copy completed=%u offset=%llu surface=%u row=%llu copy-ticks=%llu read=%s prepare-ticks=%llu sync-ticks=%llu map-ticks=%llu read-ticks=%llu\n",
            copied, offset, surface->nativeID(), stride, copyBegin ? mach_absolute_time() - copyBegin : 0,
            source ? "map" : "descriptor", preparedAt - copyBegin, synchronizedAt - preparedAt,
            mappedAt - synchronizedAt, copiedAt - mappedAt);
        // Explicit capture boots only. Sample the copied rows so a black
        // source can be distinguished from a subsequent native clear/swap.
        // Do not map the source again or read beyond the visible raster.
        if (nativeTraceEnabled && copied) {
            const unsigned sampleRows[] = {0, mode.height / 4, mode.height / 2, 3 * mode.height / 4, mode.height - 1};
            for (unsigned y : sampleRows) {
                UInt32 pixels[3];
                const auto row = dest + UInt64(y) * mode.rowBytes;
                memcpy(&pixels[0], row, sizeof(UInt32));
                memcpy(&pixels[1], row + UInt64(mode.width / 2) * 4, sizeof(UInt32));
                memcpy(&pixels[2], row + UInt64(mode.width - 1) * 4, sizeof(UInt32));
                kprintf("QcomDisplay: source pixels surface=%u y=%u bgra=%x,%x,%x\n",
                    surface->nativeID(), y, pixels[0], pixels[1], pixels[2]);
            }
        }
    }
    return copied;
}
QcomDisplaySurface *QcomDisplay::prepareScanoutSurface(IOSurface *surface,bool ahead) {
    if (!directReady || directQuarantined || !surface) return nullptr;
    const auto begin=mach_absolute_time();
    if(!ahead)++preparation.lateCalls;
    auto finished=[&](QcomDisplaySurface *result) {
        const auto elapsed=mach_absolute_time()-begin;
        if(ahead)preparation.prepareTicks+=elapsed;else preparation.lateTicks+=elapsed;
        return result;
    };
    QcomDisplaySurface *mapping = nullptr;
    // A prefetched replacement must not alias the old front's ownership.
    // Idle entries already ended their command read, but keep their original
    // descriptor/DMA preparation wired. The new request still owns its ready
    // native read fence and performs fresh coherence before hardware exposure.
    if (!ahead && frontSurface && frontSurface->surface() == surface) {
        mapping = frontSurface; mapping->retain();
        if (mapping->synchronize()) return finished(mapping);
    } else if(commandSingleTransfer && (mapping=idleMappings.take(surface))) {
        ++mappingCacheHits;
        if(mapping->synchronize())return finished(mapping);
    } else {
        ++mappingCacheMisses;
        mapping = new QcomDisplaySurface;
        if (mapping && mapping->initWithSurface(surface, displayMapper, mode)) return finished(mapping);
    }
    auto error = mapping ? mapping->error() : kIOReturnNoMemory;
    // A failed DMA preparation can retain an uncertain mapping. Disable
    // further attempts so a fallback stream cannot accumulate pinned graphs.
    directReady = false;
    setProperty("DisplayDirectScanoutMapperAvailable", false);
    setProperty("DisplayDirectScanoutLastError", UInt32(error), 32);
    IOLog("QcomDisplay: direct scanout preparation failed (%x); using reserved staging\n", error);
    OSSafeReleaseNULL(mapping); return finished(nullptr);
}
bool QcomDisplay::preparationKey(void *request,Display::PreparedSwapKey &key,IOFence *&read) const {
    if(!request)return false;
    const auto *bytes=static_cast<const UInt8 *>(request);
    UInt32 changed=0,ready=0,intent=0,token=0;
    memcpy(&changed,bytes+ChangedLayers,4);memcpy(&ready,bytes+ReadyLayers,4);
    memcpy(&intent,bytes+RequestIntent,4);memcpy(&token,bytes+0x314,4);
    // Restrict ahead preparation to the ordinary complete layer0 update.
    // Control/inherited/black transactions keep the original preparation.
    if(intent || changed!=1 || ready!=1 || bytes[0x54a])return false;
    IOSurface *surface=nullptr;memcpy(&surface,bytes+EffectiveSurfaces,sizeof(surface));
    for(unsigned layer=1;layer<4;++layer) {
        IOSurface *other=nullptr;memcpy(&other,bytes+EffectiveSurfaces+layer*sizeof(other),sizeof(other));
        if(other)return false;
    }
    if(!validateSurface(surface))return false;
    int32_t source[4],destination[4];
    memcpy(source,bytes+SourceRects,sizeof(source));memcpy(destination,bytes+DestinationRects,sizeof(destination));
    if(source[0] || source[1] || destination[0] || destination[1] ||
        source[2]!=int32_t(mode.width) || source[3]!=int32_t(mode.height) ||
        destination[2]!=int32_t(mode.width) || destination[3]!=int32_t(mode.height))return false;
    memcpy(&read,bytes+2600,sizeof(read));
    // ReadyLayers alone is not a persistent read lease. In particular, a
    // special immediate-completed access is unsuitable for this reuse path.
    if(!read || !read->nativeReady() || read->nativeCompleted() ||
        read->nativeAccessObservation(false).state!=GPU::Native::SurfaceAccessState::Ready)return false;
    key={reinterpret_cast<uintptr_t>(request),reinterpret_cast<uintptr_t>(surface),reinterpret_cast<uintptr_t>(read),token};
    return true;
}
bool QcomDisplay::retirePreparedMapping(QcomDisplaySurface *mapping) {
    if(mapping->retire()){mapping->release();return true;}
    // This map was never exposed. Failed cleanup still stops further maps;
    // retain its graph just as the existing active/front failure path does.
    directQuarantined=true;pipelineFaulted=true;directReady=false;
    setProperty("DisplayDirectScanoutQuarantined",true);
    setProperty("DisplayDirectScanoutMapperAvailable",false);
    setProperty("DisplayDirectScanoutLastError",UInt32(mapping->error()),32);
    if(!quarantinedSurface)quarantinedSurface=mapping;else mapping->release();
    return false;
}
bool QcomDisplay::discardPreparedSwap() {
    if(!preparedSwap.empty())++preparation.discarded;
    preparedAt=0;
    return preparedSwap.discard([&](QcomDisplaySurface *mapping){return retirePreparedMapping(mapping);});
}
void QcomDisplay::preparePendingSwap() {
    if(preparingSwap || !commandSingleTransfer || !directReady || directQuarantined || pipelineFaulted ||
        !swapPending || !nativeState<void *>(ActiveRequest))return;
    void *request=nativeState<void *>(PendingRequest);
    Display::PreparedSwapKey key{};IOFence *read=nullptr;
    if(request==nativeState<void *>(ActiveRequest) || !preparationKey(request,key,read) ||
        (stagedSurface && reinterpret_cast<uintptr_t>(stagedSurface->surface())==key.surface)) {
        discardPreparedSwap();return;
    }
    if(preparedSwap.matches(key,true,false))return;
    if(!discardPreparedSwap())return;
    preparingSwap=true;
    struct Reset {bool &busy;~Reset(){busy=false;}} reset{preparingSwap};
    ++preparation.attempts;
    auto *mapping=prepareScanoutSurface(reinterpret_cast<IOSurface *>(key.surface),true);
    if(!mapping)return;
    Display::PreparedSwapKey after{};IOFence *afterRead=nullptr;
    if(!directReady || directQuarantined || pipelineFaulted || !swapPending ||
        nativeState<void *>(PendingRequest)!=request || !preparationKey(request,after,afterRead) || !(after==key) ||
        !preparedSwap.adopt(key,mapping,read)) {
        ++preparation.discarded;retirePreparedMapping(mapping);return;
    }
    preparedAt=mach_absolute_time();
}
void QcomDisplay::finishScanout(bool displayed) {
    if (displayed) {
        auto *old = frontSurface;
        frontSurface = stagedSurface; stagedSurface = nullptr;
        if (frontSurface && commandSingleTransfer) frontSurface->transferCompleted();
        bool changedTransport = bool(old) != bool(frontSurface);
        // The completed transfer/latch has replaced the old fetch target.
        // Native swap completion may immediately admit another request.
        if(old && old!=frontSurface && commandSingleTransfer && !old->hardwareReading()) {
            // Physical reading ended at the previous PP_DONE. This retains
            // address translation, not a read access or permission to fetch.
            old=idleMappings.put(old);
            if(old)++mappingCacheEvictions;
        }
        if (old && old != frontSurface && !old->retire()) {
            // Do not accumulate one failed unmap per frame. Preserve this
            // single failed graph and the new front, and stop admissions.
            directQuarantined = true; pipelineFaulted = true; directReady = false;
            setProperty("DisplayDirectScanoutQuarantined", true);
            setProperty("DisplayDirectScanoutMapperAvailable", false);
            setProperty("DisplayDirectScanoutLastError", UInt32(old->error()), 32);
            quarantinedSurface = old; old = nullptr;
        }
        OSSafeReleaseNULL(old);
        if (!frontSurface) frontOffset = stagedOffset;
        else ++directFrames;
        stagedExposed = false;
        if (changedTransport) setProperty("DisplayDirectScanoutActive", frontSurface != nullptr);
        if (frontSurface && (directFrames == 1 || !(directFrames % 60)))
            setProperty("DisplayDirectScanoutFrames", directFrames, 64);
    } else if (stagedExposed && (frontSurface || stagedSurface)) {
        // Either old or new surface may still be fetched. Keep both until
        // reset; power-off/timeout is not proof of DMA retirement.
        directQuarantined = true; pipelineFaulted = true; directReady = false;
        setProperty("DisplayDirectScanoutQuarantined", true);
        setProperty("DisplayDirectScanoutMapperAvailable", false);
    } else {
        OSSafeReleaseNULL(stagedSurface);
        stagedExposed = false;
    }
}
bool QcomDisplay::clearScanout(uint64_t offset) {
    if (!scanoutMap || offset > mode.framebufferSize ||
            uint64_t(mode.rowBytes) * mode.height > mode.framebufferSize - offset) return false;
    auto destination = reinterpret_cast<uint8_t *>(scanoutMap->getVirtualAddress()) + offset;
    for (unsigned y = 0; y < mode.height; ++y) {
        auto row = reinterpret_cast<uint32_t *>(destination + y * mode.rowBytes);
        for (unsigned x = 0; x < mode.width; ++x) row[x] = 0xff000000u;
    }
    __asm__ volatile("dsb oshst" ::: "memory");
    return true;
}

void QcomDisplay::prepare_swap(void *request) {
    super::prepare_swap(request);
    recordLease(Display::LeasePhase::Prepare,request);
    recordTimeline(Display::TimelineKind::Prepare,0,request);
    if (!scanoutCaptureEnabled || !request) return;
    auto bytes = static_cast<const UInt8 *>(request);
    UInt32 sequence, kind, enabled, ready;
    int32_t source[4], destination[4];
    memcpy(&sequence, bytes + 0x314, sizeof(sequence));
    memcpy(&kind, bytes + RequestIntent, sizeof(kind));
    memcpy(&enabled, bytes + ChangedLayers, sizeof(enabled));
    memcpy(&ready, bytes + ReadyLayers, sizeof(ready));
    memcpy(source, bytes + SourceRects, sizeof(source));
    memcpy(destination, bytes + DestinationRects, sizeof(destination));
    bool physical = source[2] == int32_t(mode.width) && source[3] == int32_t(mode.height);
    if (!takeDiagnostic(physical ? preparePhysicalLogs : prepareOtherLogs)) return;
    DISPLAY_NATIVE_LOG("QcomDisplay: prepare_swap id=%u kind=%u enabled=%x ready=%x keep=%u policy=%u cancelled=%u src=%d,%d,%d,%d dst=%d,%d,%d,%d\n",
        sequence, kind, enabled, ready, bytes[0x551], nativeState<UInt8>(0x18a7), bytes[0x54a],
        source[0], source[1], source[2], source[3], destination[0], destination[1], destination[2], destination[3]);
}
bool QcomDisplay::verify_swap(void *request) {
    return validateSwap(request, false) == kIOReturnSuccess;
}
bool QcomDisplay::verify_swap_gated(void *request) {
    return validateSwap(request, true) == kIOReturnSuccess;
}
IOReturn QcomDisplay::validateSwap(void *request, bool gated) {
    if (directQuarantined) return kIOReturnNotReady;
    if (!resourcesReady || pipelineFaulted) return traceSwapVerification(request, kIOReturnNotReady, gated);
    if (!request) return traceSwapVerification(request, kIOReturnBadArgument, gated);
    auto b = static_cast<const uint8_t *>(request);
    UInt32 intent; memcpy(&intent, b + RequestIntent, sizeof(intent));
    UInt32 background; memcpy(&background, b + RequestBackground, sizeof(background));
    UInt32 mask; memcpy(&mask, b + ChangedLayers, sizeof(mask));
    UInt32 surfaces = 0;
    for (unsigned layer = 0; layer < 4; ++layer) {
        IOSurface *surface;
        memcpy(&surface, b + (gated ? EffectiveSurfaces : SurfaceArray) + layer * sizeof(surface), sizeof(surface));
        if (surface) surfaces |= 1u << layer;
    }
    auto content = Display::nativeSwapContent(intent, mask, surfaces, background);
    if (content == Display::SwapContent::Unsupported)
        return traceSwapVerification(request, kIOReturnUnsupported, gated);
    if (content == Display::SwapContent::Black)
        return traceSwapVerification(request, kIOReturnSuccess, gated);
    if (intent == 6) {
        IOSurface *surface;
        memcpy(&surface, b + (gated ? EffectiveSurfaces : SurfaceArray), sizeof(surface));
        if (!defaultSurface || surface != defaultSurface)
            return traceSwapVerification(request, kIOReturnUnsupported, gated);
    }
    // Native request rectangles are packed (unaligned at +0x11 and +0x51).
    int32_t source[4], dest[4];
    memcpy(source, b + SourceRects, sizeof(source));
    memcpy(dest, b + DestinationRects, sizeof(dest));
    if (source[0] || source[1] || dest[0] || dest[1] ||
        source[2] != int32_t(mode.width) || source[3] != int32_t(mode.height) ||
        dest[2] != int32_t(mode.width) || dest[3] != int32_t(mode.height)) return traceSwapVerification(request, kIOReturnUnsupported, gated);
    return traceSwapVerification(request, verify_swap_surfaces(request, gated), gated);
}
IOReturn QcomDisplay::traceSwapVerification(void *request, IOReturn result, bool gated) {
    // Bound diagnostics independently of frame rate; keep native rejection
    // results and the submitted rectangles unchanged.
    if (!takeDiagnostic(result == kIOReturnSuccess ? swapAcceptedLogs : swapValidationLogs)) return result;
    UInt32 mask = 0, kind = 0;
    int32_t source[4] = {}, dest[4] = {};
    if (request) {
        auto bytes = static_cast<const uint8_t *>(request);
        memcpy(&mask, bytes + ChangedLayers, sizeof(mask));
        memcpy(&kind, bytes + RequestIntent, sizeof(kind));
        memcpy(source, bytes + SourceRects, sizeof(source));
        memcpy(dest, bytes + DestinationRects, sizeof(dest));
    }
    DISPLAY_NATIVE_LOG("QcomDisplay: verify_swap stage=%u result=%x request=%p ready=%u fault=%u kind=%u changed=%x src=%d,%d,%d,%d dst=%d,%d,%d,%d\n",
        gated ? 352 : 304, result, request, resourcesReady, pipelineFaulted, kind, mask,
        source[0], source[1], source[2], source[3], dest[0], dest[1], dest[2], dest[3]);
    if (scanoutCaptureEnabled && request) {
        auto bytes = static_cast<const UInt8 *>(request);
        for (unsigned layer = 0; layer < 4; ++layer) {
            IOSurface *surface;
            memcpy(&surface, bytes + (gated ? EffectiveSurfaces : SurfaceArray) + layer * sizeof(surface), sizeof(surface));
            if (!surface && !(mask & (1u << layer))) continue;
            memcpy(source, bytes + SourceRects + layer * sizeof(source), sizeof(source));
            memcpy(dest, bytes + DestinationRects + layer * sizeof(dest), sizeof(dest));
            IOLog("QcomDisplay: native layer=%u changed=%u surface-id=%u width=%llu height=%llu row=%llu format=%x protected=%llu tiled=%u src=%d,%d,%d,%d dst=%d,%d,%d,%d\n",
                layer, bool(mask & (1u << layer)), surface ? surface->nativeID() : 0,
                surface ? surface->nativeWidth() : 0, surface ? surface->nativeHeight() : 0,
                surface ? surface->nativeRowBytes() : 0, surface ? surface->nativePixelFormat() : 0,
                surface ? surface->nativeProtection() : 0, surface ? surface->nativeTiled() : false,
                source[0], source[1], source[2], source[3], dest[0], dest[1], dest[2], dest[3]);
        }
    }
    return result;
}
IOReturn QcomDisplay::verify_swap_surfaces(void *request, bool gated) {
    if (!request) return kIOReturnBadArgument;
    auto surface = *reinterpret_cast<IOSurface **>(static_cast<uint8_t *>(request) + (gated ? EffectiveSurfaces : SurfaceArray));
    bool valid = validateSurface(surface);
    if (takeDiagnostic(surfaceValidationLogs)) {
        DISPLAY_NATIVE_LOG("QcomDisplay: verify_swap_surfaces valid=%u surface=%p width=%llu height=%llu row=%llu format=%x tiled=%u tile-format=%u\n",
            valid, surface,
            surface ? surface->nativeWidth() : 0, surface ? surface->nativeHeight() : 0,
            surface ? surface->nativeRowBytes() : 0, surface ? surface->nativePixelFormat() : 0,
            surface ? surface->nativeTiled() : false, surface ? surface->nativeTileFormat() : 0);
    }
    return valid ? kIOReturnSuccess : kIOReturnUnsupported;
}
void QcomDisplay::swap_begin_gated() {
    if (takeDiagnostic(swapBeginLogs))
        DISPLAY_NATIVE_LOG("QcomDisplay: swap_begin ready=%u pending=%u active=%p queued=%p native17e4=%u native17e8=%u native17ec=%u\n",
            resourcesReady, swapPending, nativeState<void *>(ActiveRequest), nativeState<void *>(PendingRequest),
            nativeState<UInt32>(0x17e4), nativeState<UInt32>(0x17e8), nativeState<UInt32>(0x17ec));
    // Visibility follows WLED policy. Accepted transactions still require a
    // real latch/completion while dark, including Apple's control swaps.
    if (!resourcesReady || !irqEnabled || directQuarantined) return;
    if (swapPending || nativeState<void *>(ActiveRequest)) {preparePendingSwap();return;}
    if (!nativeState<void *>(PendingRequest)) {discardPreparedSwap();return;}
    recordLease(Display::LeasePhase::Pending,nativeState<void *>(PendingRequest));
    auto previousHead=nativeState<void *>(PendingRequest);
    if(nativePrunePendingSwaps()!=kIOReturnSuccess) {
        if(previousHead!=nativeState<void *>(PendingRequest)) {
            recordLease(Display::LeasePhase::Pruned,nativeState<void *>(PendingRequest));discardPreparedSwap();
        }
        return;
    }
    nativeDropRedundantPendingSwap();
    nativeDropCompletedPendingSwaps();
    nativeDropSupersededPendingSwaps();
    if(previousHead!=nativeState<void *>(PendingRequest))recordLease(Display::LeasePhase::Pruned,nativeState<void *>(PendingRequest));
    void *request = nativeState<void *>(PendingRequest);
    if (!request) {discardPreparedSwap();return;}
    auto bytes = static_cast<uint8_t *>(request);
    UInt32 enabled, ready;
    memcpy(&enabled, bytes + ChangedLayers, sizeof(enabled));
    memcpy(&ready, bytes + ReadyLayers, sizeof(ready));
    if (enabled != ready) {discardPreparedSwap();return;} // Native access is pending.
    auto &links = *reinterpret_cast<Display::NativeSwapLinks *>(bytes + RequestLinks);
    auto next = links.next ? reinterpret_cast<Display::NativeSwapLinks *>(static_cast<uint8_t *>(links.next) + RequestLinks) : nullptr;
    if (!Display::detachNativeSwapHead(nativeState<void *>(PendingRequest), nativeState<void **>(PendingQueueLast), request, links, next)) {
        pipelineFaulted = true;
        IOLog("QcomDisplay: native swap queue links inconsistent, submission stopped\n");
        return;
    }
    nativeState<void *>(ActiveRequest) = request; // Transfer the detached queue reference.
    nativeRecordSwapState(request, 2);
    recordLease(Display::LeasePhase::Active,request);
    recordTimeline(Display::TimelineKind::Active,0,request);
    IOSurface *surface;
    memcpy(&surface, bytes + EffectiveSurfaces, sizeof(surface));
    UInt32 intent;
    memcpy(&intent, bytes + RequestIntent, sizeof(intent));
    stagedOffset = doubleBuffered ? frontOffset ? 0 : Display::bufferSpan(mode) : 0;
    stagedAddress = scanoutDeviceAddress + stagedOffset; stagedRowBytes = mode.rowBytes; stagedExposed = false;
    if (!pipelineFaulted && intent != 7) {
        Display::PreparedSwapKey key{};IOFence *read=nullptr;
        if(preparationKey(request,key,read))
            stagedSurface=preparedSwap.take(key,read->nativeReady(),read->nativeCompleted());
        if(stagedSurface) {
            ++preparation.hits;preparation.hitLeadTicks+=mach_absolute_time()-preparedAt;preparedAt=0;
        } else if(discardPreparedSwap() && !pipelineFaulted)stagedSurface=prepareScanoutSurface(surface);
    } else discardPreparedSwap();
    if (stagedSurface) {
        stagedAddress = stagedSurface->address(); stagedRowBytes = stagedSurface->rowBytes(); swapDisplayed = true;
    } else swapDisplayed = !pipelineFaulted && (intent == 7 ? clearScanout(stagedOffset) : copySurface(surface, stagedOffset));
    if (swapDisplayed && (doubleBuffered || frontSurface || stagedSurface)) {
        Display::Scanout<Registers> engine(regs, contract.source);
        swapDisplayed = engine.stage(stagedAddress, stagedRowBytes);
        stagedExposed = swapDisplayed;
        if (stagedExposed && stagedSurface) stagedSurface->exposed();
    }
    if (swapDisplayed && contract.commandMode()) {
        Display::Scanout<Registers> engine(regs, contract.source);
        regs.write(Display::InterruptClear, 1u << (8 + contract.pingPong));
        if (commandAutoRefresh) {
            regs.write(0x1018, (contract.source == 0 ? 1 : 1u << 11) | (1u << 17));
        } else {
            swapDisplayed = engine.startCommand();
            if(swapDisplayed) {
                recordTimeline(Display::TimelineKind::Started,0,request,stagedSurface?stagedSurface->surface()->nativeID():0);
                recordLease(Display::LeasePhase::Started,request);
            }
        }
    }
    swapPending = true;
    // The original ready mask/fences precede both paths. Completion and old
    // surface retirement are withheld until the actual transfer/latch.
    timeout->setTimeoutMS(100);
}
void QcomDisplay::interruptAction(OSObject *owner, IOInterruptEventSource *, int) {
    auto self = static_cast<QcomDisplay *>(owner);
    uint32_t status = self->regs.read(Display::InterruptStatus) & self->contract.ownedInterrupts();
    if (!self->irqEnabled || !status) return;
    self->recordTimeline(Display::TimelineKind::Worker,status);
    self->regs.write(Display::InterruptClear, status);
    Display::dispatchInterrupt(status,1u<<self->contract.interruptBit,
        self->contract.commandMode()?1u<<(8+self->contract.pingPong):0,
        [&]() {
            // Native note_vbl_ISR samples its continuous clock and computes
            // delta from the preceding observation. Completion may retire a
            // cold mapping and reenter swap_begin_gated; none of that work
            // belongs in this VBL's timestamp or refresh interval.
            self->note_vbl_ISR(1000000000u/self->mode.refreshHz);
            Display::VBLObservation timing{self->nativeState<UInt64>(NativeVBLCount),
                self->nativeState<UInt64>(NativeVBLTime),self->nativeState<UInt64>(NativeVBLDelta)};
            self->recordTimeline(Display::TimelineKind::VBLObserved,status,nullptr,timing.time);
            return timing;
        },[&]() {
            if (!self->swapPending) return;
            Display::Scanout<Registers> engine(self->regs, self->contract.source);
            bool latched = engine.latched(self->stagedAddress, self->stagedRowBytes);
            // Auto-refresh can complete a transfer that started before our flush.
            // Keep the native request in flight until a later real completion sees
            // our source latched; the existing hardware timeout still applies.
            Display::CommandTransfer<Registers> transfer(self->regs,self->contract.pingPong,self->mode.height);
            const bool drained=!self->commandSingleTransfer || transfer.idle();
            if (!self->swapDisplayed || (latched && drained)) {
                const bool displayed = self->swapDisplayed && latched;
                const bool trace=!displayed || (self->scanoutCaptureEnabled && takeDiagnostic(self->transferCompletionLogs));
                // Native completion may immediately replace the staged state.
                const auto expected=self->stagedAddress;
                const bool copied=self->swapDisplayed;
                const auto address=trace?self->regs.read((self->contract.source==0?0x4000:0x24000)+0x14):0;
                const auto flush=trace?self->regs.read(0x1018):0;
                self->swapPending = false;
                self->timeout->cancelTimeout();
                self->completeSwap(displayed);
                if (trace)
                IOLog("QcomDisplay: command transfer completed displayed=%u copied=%u address=%x expected=%llx flush=%x\n",
                    displayed,copied,address,expected,flush);
            }
        },[&]() {
            // Native completion wakes producers but can leave a ready request
            // queued. Arm it before the next TE, after the old DMA/read fence
            // retired. Reentrant native submission may already own the engine.
            if (!self->swapPending && !self->nativeState<void *>(ActiveRequest) &&
                self->nativeState<void *>(PendingRequest)) self->swap_begin_gated();
        },[&](const Display::VBLObservation &timing) {self->onVBL(timing);});
}
void QcomDisplay::onVBL(const Display::VBLObservation &timing) {
    recordTimeline(Display::TimelineKind::VBL);
    // Publish the original helper's exact observation from before retirement.
    // Notification still follows transfer completion, so its reentrant swap
    // cannot consume the captured old PP_DONE. No synthetic VBL or clock reset.
    lastFrameTime = timing.time;
    nativeNotifyVBL(timing.count,timing.time,timing.delta);
    ++frames;
    if (frames == 1) IOLog("QcomDisplay: first hardware %s interrupt\n", contract.commandMode() ? "TE/read-pointer" : "VBL");
    if (swapPending && !contract.commandMode()) {
        if (swapDisplayed && stagedExposed) {
            Display::Scanout<Registers> engine(regs, contract.source);
            if (!engine.latched(stagedAddress, stagedRowBytes)) return;
        }
        bool displayed = swapDisplayed;
        swapPending = false;
        timeout->cancelTimeout();
        completeSwap(displayed);
    }
    if (!swapPending && nativeState<void *>(PendingRequest)) swap_begin_gated();
    else if(swapPending)preparePendingSwap();
}
void QcomDisplay::timeoutAction(OSObject *owner, IOTimerEventSource *) {
    auto self = static_cast<QcomDisplay *>(owner);
    if (!self->swapPending) return;
    if (self->doubleBuffered && self->swapDisplayed) self->pipelineFaulted = true;
    self->swapPending = false;
    IOLog("QcomDisplay: hardware VBL timeout, rejecting swap\n");
    self->completeSwap(false);
}
void QcomDisplay::completeSwap(bool displayed) {
    recordTimeline(Display::TimelineKind::CompleteBefore,0,nativeState<void *>(ActiveRequest),displayed);
    recordLease(Display::LeasePhase::CompleteBefore,nativeState<void *>(ActiveRequest));
    // The request still owns its read fence here. Retire only that access
    // after actual PP_DONE and a drained, non-repeating DMA engine. Native
    // completion then transfers the logical surface/map with a null fence;
    // it remains available for Apple's inheritance and cleanup semantics.
    if (displayed && commandSingleTransfer && !retireCommandRead(nativeState<void *>(ActiveRequest))) {
        pipelineFaulted=true;
        displayed=false;
    }
    recordTimeline(Display::TimelineKind::ReadRetired,0,nativeState<void *>(ActiveRequest),displayed);
    finishScanout(displayed);
    recordTimeline(Display::TimelineKind::SurfaceRetired,0,nativeState<void *>(ActiveRequest),displayed);
    auto request = static_cast<const UInt8 *>(nativeState<void *>(ActiveRequest));
    bool trace = scanoutCaptureEnabled && takeDiagnostic(completionLogs);
    UInt32 sequence = 0, kind = 0, state = 0;
    if (request) {
        memcpy(&sequence, request + 0x314, sizeof(sequence));
        memcpy(&kind, request + RequestIntent, sizeof(kind));
        memcpy(&state, request + 0x34c, sizeof(state));
    }
    if(scanoutCaptureEnabled) {
        if(displayed) {
            ++displayedSwapCount;
            const uint32_t at=(swapSampleHead+swapSampleCount)%Display::SwapSampleCapacity;
            swapSamples[at]={mach_continuous_time(),sequence,uint32_t(displayVisible)|(frontSurface?2u:0u)};
            if(swapSampleCount<Display::SwapSampleCapacity)++swapSampleCount;
            else swapSampleHead=(swapSampleHead+1)%Display::SwapSampleCapacity;
        } else ++failedSwapCount;
    }
    if (trace) DISPLAY_NATIVE_LOG("QcomDisplay: native completion before id=%u kind=%u state=%u active=%u cancelled=%u displayed=%u completed=%u\n",
        sequence, kind, state, request != nullptr, request ? request[0x54a] : 0, displayed,
        nativeState<UInt32>(LastCompletedSwap));
    // The original function owns the active reference, state transition and
    // CommandWake. Do not touch the request after it returns.
    super::swap_complete_gated(displayed);
    recordLease(Display::LeasePhase::CompleteAfter);
    recordTimeline(Display::TimelineKind::CompleteAfter,0,nullptr,displayed);
    if (trace) DISPLAY_NATIVE_LOG("QcomDisplay: native completion after id=%u completed=%u displayed=%u active=%u\n",
        sequence, nativeState<UInt32>(LastCompletedSwap), nativeState<UInt32>(LastDisplayedSwap),
        nativeState<void *>(ActiveRequest) != nullptr);
}
bool QcomDisplay::retireCommandRead(void *request) {
    Display::CommandTransfer<Registers> transfer(regs,contract.pingPong,mode.height);
    if (!request || !transfer.idle()) return false;
    // Accepted surface transactions always replace primary layer0; all other
    // layers are null. Do not clear displayedData, signal a producer fence,
    // or infer readiness from a completed swap token.
    auto **fences=reinterpret_cast<IOFence **>(static_cast<UInt8 *>(request)+2600);
    for (unsigned layer=0;layer<4;++layer) if (auto *fence=fences[layer]) {
        // Native signal rewrites an already completed status. Preserve an
        // existing error instead of converting a cancelled read to success.
        const auto access=fence->nativeAccessObservation(false);
        if (access.state==GPU::Native::SurfaceAccessState::Failed) return false;
        const auto result=fence->nativeCompleted()?kIOReturnSuccess:QcomIOFenceSignal(fence,kIOReturnSuccess);
        if (result!=kIOReturnSuccess) {
            IOLog("QcomDisplay: completed transfer read retirement failed layer=%u result=%x\n",layer,result);
            return false;
        }
        fences[layer]=nullptr;
        fence->release();
        ++commandReadRetired;
    }
    if (commandReadRetired==1 || !(commandReadRetired%60))
        setProperty("DisplayCommandReadRetired",commandReadRetired,64);
    return true;
}
bool QcomDisplay::is_main_display() { return resourcesReady; }
IOReturn QcomDisplay::set_display_device(UInt32) {
    // The pinned iOS 18 AppleH7DisplayPipe also accepts this routing operation
    // unconditionally for its single internal output. This is not an index
    // into our (single) physical source pipe.
    return kIOReturnSuccess;
}
bool QcomDisplay::get_timing_attributes_gated(UInt64 *attributes, bool) const {
    if (!attributes || !mode.refreshHz) return false;
    uint64_t period = 1000000000ULL / mode.refreshHz, ticks;
    nanoseconds_to_absolutetime(period, &ticks);
    attributes[0] = attributes[2] = (1ULL << 32) / mode.refreshHz;
    attributes[1] = attributes[3] = ticks;
    attributes[4] = attributes[5] = 0;
    return true;
}
IOReturn QcomDisplay::set_notify_tag_mask_gated(UInt32, void *, UInt32) { return kIOReturnUnsupported; }
IOReturn QcomDisplay::supported_frame_info(UInt32, UInt32 *count, char (*name)[64], UInt32 *flags) {
    if (!count || !name || !flags) return kIOReturnBadArgument;
    *count = 0; (*name)[0] = 0; *flags = 0; return kIOReturnUnsupported;
}
IOReturn QcomDisplay::factory_portal(void *, task_t) { return kIOReturnUnsupported; }
IOReturn QcomDisplay::executeFactoryConfigurations_gated() { return kIOReturnUnsupported; }
IOReturn QcomDisplay::set_frame_info_notification_state_gated(void *, bool enable) {
    return enable ? kIOReturnUnsupported : kIOReturnSuccess;
}
void QcomDisplay::do_swap_info_notify_gated(void *, void *) {}
void QcomDisplay::notify_swap_done_gated(UInt32) {}
void QcomDisplay::notify_crc_ready_gated(UInt32) {}
bool QcomDisplay::timing_enabled() const {
    // configure() verifies the live raster, DSI command/video mode and retained
    // resources. WLED blanking does not stop this hardware timing domain.
    return resourcesReady && irqEnabled;
}

bool QcomDisplay::publishDisplayProperties() {
    setProperty("IOFBDependentIndex", 0ULL, 32);
    setProperty("IOFBDependentID", 0ULL, 32);
    setProperty("DisplayWidth", mode.width, 32);
    setProperty("DisplayHeight", mode.height, 32);
    setProperty("Width", mode.width, 32);
    setProperty("Height", mode.height, 32);
    setProperty("RowBytes", defaultSurface->nativeRowBytes(), 32);
    setProperty("BitsPerPixel", 32ULL, 32);
    setProperty("PixelFormat", "BGRA");
    setProperty("dot-pitch", pixelsPerInch, 32);
    setProperty("MDP Double Buffered", doubleBuffered);
    setProperty("Location", "Internal");
    setProperty("IOMFBSupportsXFlip", false);
    setProperty("IOMFBSupportsYFlip", false);
    // Identity comes from the panel/loader, never a shared hardcoded UUID.
    auto uuid = OSDynamicCast(OSData, getProvider()->getProperty("display-uuid"));
    if (!uuid || uuid->getLength() != 37 || static_cast<const char *>(uuid->getBytesNoCopy())[36]) return false;
    const char *identity = static_cast<const char *>(uuid->getBytesNoCopy());
    for (unsigned i = 0; i < 36; ++i) {
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        bool hex = (identity[i] >= '0' && identity[i] <= '9') ||
            (identity[i] >= 'a' && identity[i] <= 'f') || (identity[i] >= 'A' && identity[i] <= 'F');
        if (dash ? identity[i] != '-' : !hex) return false;
    }
    // iOS 18 represents the built-in digitizer's display with a nil UUID.
    // Publishing IOMFBUUID here makes CAWindowServer's main display uniqueId
    // non-nil, so BackBoard no longer recognizes that digitizer as the main
    // display and converts its normalized contacts with zero screen bounds.
    // Keep the loader's panel identity for diagnostics without assigning an
    // external-display identity to the built-in framebuffer.
    if (!setProperty("QcomPanelUUID", identity)) return false;
    OSDictionary *refresh = OSDictionary::withCapacity(6);
    if (!refresh) return false;
    UInt64 attributes[6];
    bool ok = get_timing_attributes_gated(attributes, false);
    const char *names[] = {"displayMinRefreshInterval", "displayMinRefreshIntervalMachTime",
        "displayMaxRefreshInterval", "displayMaxRefreshIntervalMachTime", "displayRefreshStep", "displayRefreshStepMachTime"};
    for (unsigned i = 0; i < 6 && ok; ++i) ok = number(refresh, names[i], attributes[i]);
    ok = ok && setProperty("IOMFBDisplayRefresh", refresh);
    refresh->release();
    // Names and dictionary grouping are present in this iOS 18 image. Values
    // describe our supported linear full-canvas path, not the Apple H7 limits.
    OSDictionary *limits = OSDictionary::withCapacity(7);
    if (!limits) return false;
    ok = ok && number(limits, "MaxSrcRectWidth", mode.width, 32) &&
        number(limits, "MaxSrcRectTotal", UInt64(mode.width) * mode.height, 32) &&
        number(limits, "MaxSrcBufferWidth", mode.width, 32) &&
        number(limits, "MaxSrcBufferHeight", mode.height, 32) &&
        number(limits, "MaxVideoSrcDownscalingWidth", mode.width, 32) &&
        // QuartzCore reads both compression keys from IOMFBMaxSrcPixels.
        // The DPU path accepts linear BGRA and cannot decode Apple's lossless
        // IOSurface layouts; an absent capability would retain its default.
        number(limits, "IOMFBCompressionSupport", 0, 32) &&
        number(limits, "IOMFBMaxCompressedSizeInBytes", 0, 32) &&
        setProperty("IOMFBMaxSrcPixels", limits);
    limits->release();
    OSDictionary *alignment = OSDictionary::withCapacity(5);
    if (!alignment) return false;
    ok = ok && number(alignment, "DefaultStride", defaultSurface->nativeRowBytes(), 32) &&
        number(alignment, "TileX_Alignment", 1, 32) && number(alignment, "TileY_Alignment", 1, 32) &&
        number(alignment, "LinearX_Alignment", 64, 32) && number(alignment, "LinearY_Alignment", 1, 32) &&
        number(alignment, "PlaneBaseAlignmentLinear", 64, 32) &&
        setProperty("DisplayPipePlaneBaseAlignment", alignment);
    alignment->release();
    OSDictionary *stride = OSDictionary::withCapacity(2);
    if (!stride) return false;
    ok = ok && number(stride, "StrideLinearHorizontal", 64, 32) &&
        number(stride, "StrideLinearVertical", 1, 32) && setProperty("DisplayPipeStrideRequirements", stride);
    stride->release();
    OSDictionary *scaling = OSDictionary::withCapacity(4);
    if (!scaling) return false;
    const char *scaleNames[] = {"RGBLayer_MinScaleFraction", "RGBLayer_MaxScale", "YUVLayer_MinScaleFraction", "YUVLayer_MaxScale"};
    for (auto key : scaleNames) ok = ok && number(scaling, key, 1, 32);
    ok = ok && setProperty("IOMFBScalingLimits", scaling);
    scaling->release();
    return ok;
}

void QcomDisplay::stop_hardware_gated() {
    if (resourcesReady && backlightClaimed) backlightRequest(DisplayBacklight::SetPower, 0);
    displayVisible = false;
    resourcesReady = false;
    if (interrupt) interrupt->disable();
    if (irqEnabled) {
        uint32_t bit = contract.ownedInterrupts();
        regs.write(Display::InterruptEnable,
            (regs.read(Display::InterruptEnable) & ~bit) | (savedInterruptEnable & bit));
        regs.write(Display::InterruptClear, bit);
        irqEnabled = false;
    }
    if (timeout) timeout->cancelTimeout();
    if (swapPending) { swapPending = false; completeSwap(false); }
}
bool QcomDisplay::requestTerminate(IOService *, IOOptionBits) { return false; }
void QcomDisplay::displayMapperFault() {
    // The mapper callback and native presentation run under the same work
    // loop. Do not release an exposed surface or invent completion here.
    directReady = false; directQuarantined = true; pipelineFaulted = true;
    setProperty("DisplayMapperFault", true);
    setProperty("DisplayDirectScanoutMapperAvailable", false);
    setProperty("DisplayDirectScanoutQuarantined", true);
}
void QcomDisplay::releaseResources() {
    stopPowerManagement();
    resourcesReady = false;
    IOWorkLoop *loop = frameworkLoop;
    if (loop && timerAdded) { loop->removeEventSource(timeout); timerAdded = false; }
    if (loop && interruptAdded) { loop->removeEventSource(interrupt); interruptAdded = false; }
    if (loop && gateAdded) { loop->removeEventSource(gate); gateAdded = false; }
    OSSafeReleaseNULL(timeout); OSSafeReleaseNULL(interrupt); OSSafeReleaseNULL(gate);
    OSSafeReleaseNULL(frameworkLoop);
    if(timeline){IODelete(timeline,Display::TimelineRing,1);timeline=nullptr;}
    if ((frontSurface && frontSurface->hardwareReading()) || (stagedSurface && stagedSurface->hardwareReading()))
        IOLog("QcomDisplay: raster remains active at teardown; direct mappings quarantined\n");
    discardPreparedSwap();
    while(auto *idle=idleMappings.pop())retirePreparedMapping(idle);
    OSSafeReleaseNULL(frontSurface); OSSafeReleaseNULL(stagedSurface); OSSafeReleaseNULL(quarantinedSurface);
    OSSafeReleaseNULL(displayMapper);
    if (ownedMapper) ownedMapper->shutdown();
    OSSafeReleaseNULL(ownedMapper);
    OSSafeReleaseNULL(defaultSurface);
    OSSafeReleaseNULL(scanoutMap); OSSafeReleaseNULL(scanoutMemory); OSSafeReleaseNULL(mdpMap);
    if (panelOpened) { panel->close(this); panelOpened = false; }
    OSSafeReleaseNULL(panel);
    if (backlightClaimed) { backlightRequest(DisplayBacklight::Release); backlightClaimed = false; }
    OSSafeReleaseNULL(backlight);
    if (opened) { getProvider()->close(this); opened = false; }
}
void QcomDisplay::stopPowerManagement() {
    if (!pmStarted) return;
    // Pair the locally owned registration before native teardown. The native
    // interface flag must not cause a second PMstop in the superclass.
    nativeState<UInt8>(NativePMInitialized) = 0;
    PMstop(); pmStarted = false;
}
void QcomDisplay::stopFramework(IOService *provider) {
    stopPowerManagement();
    if (nativeStarted) {
        if (gate) gate->runAction([](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto self = static_cast<QcomDisplay *>(owner);
            self->stop_hardware_gated();
            // The native stop path has additional ownership assumptions for
            // default descriptors that it allocates itself. Detach our field
            // before entering that path; the surface owns its own descriptor.
            auto surface = self->nativeState<IOSurface *>(DefaultSurface);
            self->nativeState<IOSurface *>(DefaultSurface) = nullptr;
            OSSafeReleaseNULL(surface);
            return kIOReturnSuccess;
        });
        else {
            stop_hardware_gated();
            auto surface = nativeState<IOSurface *>(DefaultSurface);
            nativeState<IOSurface *>(DefaultSurface) = nullptr;
            OSSafeReleaseNULL(surface);
        }
        if (frameworkLoop && timerAdded) { frameworkLoop->removeEventSource(timeout); timerAdded = false; }
        if (frameworkLoop && interruptAdded) { frameworkLoop->removeEventSource(interrupt); interruptAdded = false; }
        if (frameworkLoop && gateAdded) { frameworkLoop->removeEventSource(gate); gateAdded = false; }
        super::stop(provider); nativeStarted = false;
    }
}
void QcomDisplay::stop(IOService *provider) {
    stopFramework(provider);
    releaseResources();
}
void QcomDisplay::free() { releaseResources(); super::free(); }
