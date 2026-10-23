/* SPDX-License-Identifier: BSD-2-Clause */
#include "QcomPanel.h"
#include <AppleARMPlatform/ARMDisplayProperties.h>
#include <ARMPlatformConfig.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSSymbol.h>

#define super IOService
OSDefineMetaClassAndStructors(QcomPanel, super);

IOService *QcomPanel::probe(IOService *p, SInt32 *score) {
    auto status = OSDynamicCast(OSData, p->getProperty("status"));
    if (p->getProperty("status") && !status) return nullptr;
    if (status && !ARMPlatformConfig::enabled(status->getBytesNoCopy(), status->getLength())) return nullptr;
    return super::probe(p, score);
}
bool QcomPanel::initializeSequence(const uint8_t *bytes, size_t size) {
    if (!MIPIDSI::panelSequence(bytes, size)) return false;
    for (size_t pos = 0; pos < size;) {
        uint16_t length = bytes[pos + 2] | uint16_t(bytes[pos + 3]) << 8;
        unsigned delay = bytes[pos + 4] | unsigned(bytes[pos + 5]) << 8;
        uint8_t type = bytes[pos]; const uint8_t *data = bytes + pos + 8;
        IOReturn status = type == 0x29 || type == 0x39 ?
            device->sendLongCommand(type, data, length) :
            device->sendShortCommand(type, data[0], length > 1 ? data[1] : 0);
        if (status != kIOReturnSuccess) return false;
        if (delay) IOSleep(delay);
        pos += 8 + length;
    }
    return true;
}
bool QcomPanel::start(IOService *p) {
    uint32_t version, flags, identifier;
    if (!displayProperty32(p, "panel-handoff-version", version) || version != 1 ||
        !displayProperty32(p, "panel-handoff-flags", flags) || (flags != 3 && flags != 5) ||
        !displayProperty32(p, "AAPL,phandle", identifier) || !identifier) return false;
    device = OSDynamicCast(AppleARMMIPIDSIDevice, p);
    if (!device || !super::start(p)) return false;
    opened = device->open(this);
    workLoop = IOWorkLoop::workLoop();
    gate = IOCommandGate::commandGate(this, brightnessAction);
    if (!opened || !workLoop || !gate || workLoop->addEventSource(gate) != kIOReturnSuccess) goto fail;
    added = true;
    if (device->enableInterface(true) != kIOReturnSuccess) goto fail;
    if (flags == 5) {
        auto sequence = OSDynamicCast(OSData, p->getProperty("panel-init-sequence"));
        if (!sequence || !initializeSequence(static_cast<const uint8_t *>(sequence->getBytesNoCopy()), sequence->getLength())) goto fail;
    }
    if (p->getProperty("panel-brightness-bits") &&
        (!displayProperty32(p, "panel-brightness-bits", brightnessBits) ||
        (brightnessBits != 8 && brightnessBits != 16))) goto fail;
    ready = true;
    setProperty("DisplayPanelIdentifier", identifier, 32);
    setProperty("PanelReady", true);
    registerService();
    return true;
fail:
    releaseResources(); super::stop(p); return false;
}
IOReturn QcomPanel::brightness(uint32_t level) {
    if (!ready) return kIOReturnNotReady;
    if (!brightnessBits) return kIOReturnUnsupported;
    if (level >= (1u << brightnessBits)) return kIOReturnBadArgument;
    if (brightnessBits == 8) return device->sendShortCommand(0x15, 0x51, level);
    const uint8_t packet[] = {0x51, uint8_t(level >> 8), uint8_t(level)};
    return device->sendLongCommand(0x39, packet, sizeof(packet));
}
IOReturn QcomPanel::brightnessAction(OSObject *owner, void *value, void *, void *, void *) {
    return static_cast<QcomPanel *>(owner)->brightness(uint32_t(reinterpret_cast<uintptr_t>(value)));
}
IOReturn QcomPanel::callPlatformFunction(const OSSymbol *function, bool wait, void *p1, void *p2, void *p3, void *p4) {
    if (function && function->isEqualTo("MIPIPanelPrepare")) return ready ? kIOReturnSuccess : kIOReturnNotReady;
    if (function && function->isEqualTo("MIPIPanelSetBrightness"))
        return gate ? gate->runAction(brightnessAction, p1) : kIOReturnNotReady;
    return super::callPlatformFunction(function, wait, p1, p2, p3, p4);
}
IOWorkLoop *QcomPanel::getWorkLoop() const { return workLoop; }
bool QcomPanel::requestTerminate(IOService *, IOOptionBits) { return false; }
void QcomPanel::releaseResources() {
    ready = false;
    if (added) { workLoop->removeEventSource(gate); added = false; }
    OSSafeReleaseNULL(gate); OSSafeReleaseNULL(workLoop);
    if (opened) { device->close(this); opened = false; }
    device = nullptr;
}
void QcomPanel::stop(IOService *p) {
    if (gate) gate->runAction([](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
        static_cast<QcomPanel *>(owner)->ready = false; return kIOReturnSuccess;
    });
    releaseResources(); super::stop(p);
}
void QcomPanel::free() { releaseResources(); super::free(); }
