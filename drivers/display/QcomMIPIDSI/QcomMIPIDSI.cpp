/* SPDX-License-Identifier: BSD-2-Clause */
#include "QcomMIPIDSI.h"
#include <AppleARMPlatform/ARMDisplayProperties.h>
#include <IOKit/IOLib.h>

#define super AppleARMMIPIDSIController
OSDefineMetaClassAndStructors(QcomMIPIDSI, super);

uint32_t QcomMIPIDSI::Registers::read(uint32_t offset) {
    uint32_t value = *reinterpret_cast<volatile uint32_t *>(base + offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}
void QcomMIPIDSI::Registers::write(uint32_t offset, uint32_t value) {
    __asm__ volatile("dmb oshst" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
    __asm__ volatile("dsb oshst" ::: "memory");
}
void QcomMIPIDSI::Registers::delay(unsigned us) { IODelay(us); }

bool QcomMIPIDSI::start(IOService *provider) {
    MIPIDSI::PlatformContract c{};
    if (!displayProperty32(provider, "qcom,dsi-handoff-version", c.version) ||
        !displayProperty32(provider, "qcom,dsi-handoff-flags", c.flags) ||
        !displayProperty32(provider, "dsi-lanes", c.lanes) ||
        !displayProperty32(provider, "qcom,dsi-byte-clock-hz", c.byteClockHz) ||
        !displayProperty32(provider, "qcom,dsi-escape-clock-hz", c.escapeClockHz) || !c.valid()) {
        IOLog("QcomMIPIDSI: clock/power/PHY handoff is missing or invalid\n");
        return false;
    }
    if (!super::start(provider)) return false;
    opened = provider->open(this);
    if (!opened || provider->getDeviceMemoryCount() != 1) goto fail;
    map = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
    if (!map || map->getLength() < 0x200) goto fail;
    regs.base = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
    if (!MIPIDSI::controller6G(regs.read(MIPIDSI::LegacyVersion), regs.read(MIPIDSI::ShiftedVersion),
            regs.read(MIPIDSI::HardwareVersion)) ||
        (regs.read(MIPIDSI::Status) & MIPIDSI::DMAActive)) goto fail;
    mapper = IOMapper::copyMapperForDevice(provider);
    if (!mapper && !(c.flags & MIPIDSI::PlatformContract::DMABypass)) goto fail;
    commandMemory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone |
        kIOMapInhibitCache, MIPIDSI::CommandBufferSize, 0xfffff000ULL);
    if (!commandMemory || commandMemory->prepare(kIODirectionOut) != kIOReturnSuccess) goto fail;
    prepared = true;
    dma = IODMACommand::withSpecification(IODMACommand::OutputHost64, 32,
        MIPIDSI::CommandBufferSize, mapper ? IODMACommand::kMapped : IODMACommand::kUnmapped,
        MIPIDSI::CommandBufferSize, 4096, mapper);
    if (!dma || dma->setMemoryDescriptor(commandMemory) != kIOReturnSuccess) goto fail;
    {
        IODMACommand::Segment64 segment{}; UInt64 offset = 0; UInt32 count = 1;
        if (dma->gen64IOVMSegments(&offset, &segment, &count) != kIOReturnSuccess ||
            count != 1 || offset != MIPIDSI::CommandBufferSize ||
            segment.fLength != MIPIDSI::CommandBufferSize ||
            segment.fIOVMAddr > UINT32_MAX - (MIPIDSI::CommandBufferSize - 1)) goto fail;
        commandAddress = segment.fIOVMAddr;
    }
    workLoop = IOWorkLoop::workLoop();
    gate = IOCommandGate::commandGate(this, action);
    if (!workLoop || !gate || workLoop->addEventSource(gate) != kIOReturnSuccess) goto fail;
    gateAdded = true;
    running = true;
    setProperty("MIPIDSI Lanes", c.lanes, 32);
    setProperty("MIPIDSI Byte Clock", c.byteClockHz, 32);
    publishBelow(provider); // Native AppleARMMIPIDSIDevice nubs and matching.
    registerService();
    return true;
fail:
    releaseResources();
    super::stop(provider);
    return false;
}

IOWorkLoop *QcomMIPIDSI::getWorkLoop() const { return workLoop; }
bool QcomMIPIDSI::requestTerminate(IOService *, IOOptionBits) { return false; }

IOReturn QcomMIPIDSI::setEnabled(unsigned kind, bool enable) {
    if (!gate) return kIOReturnNotReady;
    Operation op{kind, 0, nullptr, 0, enable};
    return gate->runAction(action, &op);
}
IOReturn QcomMIPIDSI::enableInterface(bool enable) { return setEnabled(0, enable); }
IOReturn QcomMIPIDSI::enableHighSpeedMode(bool enable) { return setEnabled(1, enable); }
IOReturn QcomMIPIDSI::enableVideo(bool enable) { return setEnabled(2, enable); }
IOReturn QcomMIPIDSI::sendShortCommand(UInt8 type, UInt8 first, UInt8 second) {
    if (!gate) return kIOReturnNotReady;
    uint8_t bytes[2] = {first, second};
    uint64_t length = type == 0x03 || type == 0x04 ? 0 :
        type == 0x05 || type == 0x06 || type == 0x13 || type == 0x14 ? 1 : 2;
    Operation op{3, type, bytes, length, false};
    return gate->runAction(action, &op);
}
IOReturn QcomMIPIDSI::sendLongCommand(UInt8 type, const UInt8 *data, UInt64 length) {
    if (!gate) return kIOReturnNotReady;
    if (type != 0x29 && type != 0x39) return kIOReturnBadArgument;
    Operation op{3, type, data, length, false};
    return gate->runAction(action, &op);
}
IOReturn QcomMIPIDSI::receiveShortCommand(UInt8 type, UInt8 *data) {
    if (!data) return kIOReturnBadArgument;
    UInt64 count = 2;
    IOReturn result = receiveLongCommand(type, data, &count);
    if (result == kIOReturnSuccess && count == 1) data[1] = 0;
    return result;
}
IOReturn QcomMIPIDSI::receiveLongCommand(UInt8 type, UInt8 *data, UInt64 *length) {
    if (!gate) return kIOReturnNotReady;
    if (!data || !length || !*length || *length > 10) return kIOReturnBadArgument;
    unsigned requestBytes = type == 0x04 ? 0 : type == 0x06 || type == 0x14 ? 1 : type == 0x24 ? 2 : 3;
    if (requestBytes > 2 || requestBytes > *length) return kIOReturnBadArgument;
    // Apple uses the same two-byte input/output buffer for short read packets.
    uint8_t request[2] = {data[0], requestBytes > 1 ? data[1] : uint8_t(0)};
    Operation op{4, type, request, *length, false, data};
    IOReturn result = gate->runAction(action, &op);
    *length = result == kIOReturnSuccess ? op.length : 0;
    return result;
}
IOReturn QcomMIPIDSI::action(OSObject *owner, void *arg, void *, void *, void *) {
    return static_cast<QcomMIPIDSI *>(owner)->perform(*static_cast<Operation *>(arg));
}
IOReturn QcomMIPIDSI::perform(Operation &op) {
    if (!running || dmaPinned) return kIOReturnNotReady;
    if (op.kind < 3) {
        if (op.kind == 1) { highSpeed = op.enable; return kIOReturnSuccess; }
        uint32_t mask = op.kind == 0 ? MIPIDSI::Enable : MIPIDSI::VideoEnable;
        uint32_t control = regs.read(MIPIDSI::Control);
        if (!!(control & mask) == op.enable) return kIOReturnSuccess;
        if (regs.read(MIPIDSI::Status) & 0x1f) return kIOReturnBusy;
        regs.write(MIPIDSI::Control, op.enable ? control | mask : control & ~mask);
        return kIOReturnSuccess;
    }
    if (op.kind == 4) {
        uint8_t maximum[2] = {uint8_t(op.length), 0};
        IOReturn result = transfer(0x37, maximum, 2, false);
        if (result != kIOReturnSuccess) return result;
        regs.write(MIPIDSI::ReadbackControl, 1);
        regs.write(MIPIDSI::ReadbackControl, 0);
        unsigned requestBytes = op.type == 0x04 ? 0 : op.type == 0x24 ? 2 : 1;
        result = transfer(op.type, op.data, requestBytes, true);
        if (result != kIOReturnSuccess) return result;
        uint8_t raw[16];
        for (unsigned i = 0; i < 4; ++i) {
            uint32_t word = regs.read(MIPIDSI::Readback + (3 - i) * 4);
            for (unsigned j = 0; j < 4; ++j) raw[i * 4 + j] = uint8_t(word >> (24 - j * 8));
        }
        unsigned actual;
        if (MIPIDSI::response(raw, op.length, op.reply, actual) != MIPIDSI::Result::Success) return kIOReturnIOError;
        op.length = actual;
        return kIOReturnSuccess;
    }
    return transfer(op.type, op.data, op.length, false);
}
IOReturn QcomMIPIDSI::transfer(UInt8 type, const UInt8 *data, UInt64 length, bool read) {
    if (length > MIPIDSI::MaxPayload) return kIOReturnBadArgument;
    unsigned size = MIPIDSI::packet(static_cast<uint8_t *>(commandMemory->getBytesNoCopy()),
        MIPIDSI::CommandBufferSize, type, data, length, read);
    if (!size) return kIOReturnBadArgument;
    MIPIDSI::Host<Registers> host(regs);
    auto result = host.transfer(commandAddress, size, read, !highSpeed);
    if (result == MIPIDSI::Result::Success) return kIOReturnSuccess;
    if (result == MIPIDSI::Result::Busy) return kIOReturnBusy;
    if (result == MIPIDSI::Result::Timeout) {
        if (regs.read(MIPIDSI::Status) & MIPIDSI::DMAActive) {
            dmaPinned = true;
            retain(); // Resident service pins memory that hardware may still read.
            IOLog("QcomMIPIDSI: DMA timeout; command mapping pinned until reboot\n");
        }
        return kIOReturnTimeout;
    }
    return kIOReturnIOError;
}
void QcomMIPIDSI::releaseResources() {
    running = false;
    if (dmaPinned) return;
    if (gateAdded) { workLoop->removeEventSource(gate); gateAdded = false; }
    OSSafeReleaseNULL(gate);
    OSSafeReleaseNULL(workLoop);
    if (dma) { dma->clearMemoryDescriptor(); OSSafeReleaseNULL(dma); }
    if (prepared) { commandMemory->complete(kIODirectionOut); prepared = false; }
    OSSafeReleaseNULL(commandMemory);
    OSSafeReleaseNULL(mapper);
    OSSafeReleaseNULL(map);
    if (opened) { getProvider()->close(this); opened = false; }
}
void QcomMIPIDSI::stop(IOService *provider) {
    // No new transfers can enter after the work-loop fence.
    if (gate) gate->runAction([](OSObject *o, void *, void *, void *, void *) -> IOReturn {
        static_cast<QcomMIPIDSI *>(o)->running = false; return kIOReturnSuccess;
    });
    releaseResources();
    super::stop(provider);
}
void QcomMIPIDSI::free() { releaseResources(); super::free(); }
