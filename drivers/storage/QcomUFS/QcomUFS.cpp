/* SPDX-License-Identifier: BSD-2-Clause */
#include "QcomUFS.h"
#include "QcomUFSBlockDevice.h"
#include "QcomUFSPhy.h"
#include <IOKit/IOLib.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSBoolean.h>
#include <kern/clock.h>
#include <mach/mach_time.h>

#define super IOService
OSDefineMetaClassAndStructors(QcomUFS, IOService);

uint32_t QcomUFS::Registers::read(uint32_t offset) {
    uint32_t value = *reinterpret_cast<volatile uint32_t *>(base + offset);
    __asm__ volatile("dmb oshld" ::: "memory"); return value;
}
void QcomUFS::Registers::write(uint32_t offset, uint32_t value) {
    __asm__ volatile("dmb oshst" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
    __asm__ volatile("dsb oshst" ::: "memory");
}
void QcomUFS::Registers::delay(unsigned usec) {
    // Register handshakes run under a command gate as well as at startup.
    // Use a bounded hardware-counter spin; IODelay can block its thread while
    // CPU idle latency is unknown, which is not valid for these gated polls.
    uint64_t start, now, hz;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(start));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
    const uint64_t ticks=(hz*usec+999999)/1000000;
    do {__asm__ volatile("yield; mrs %0, cntvct_el0" : "=r"(now));} while(now-start<ticks);
}

static bool property32(IOService *service, const char *name, uint32_t &value) {
    auto data = OSDynamicCast(OSData, service->getProperty(name));
    if (!data || data->getLength() != 4) return false;
    const auto *b = static_cast<const uint8_t *>(data->getBytesNoCopy());
    value = uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
    return true;
}

bool QcomUFS::configurePlatform(IOService *provider) {
    // Clocks, regulators, device RESET_N and NoC votes belong to the platform.
    // This explicit, versioned firmware contract must be emitted by a loader
    // only after it has prepared and pinned those resources. Merely setting
    // compatible in a DT does not assert that DMA or the clocks are ready.
    UFS::PlatformContract contract{};
    if (!property32(provider, "qcom,ufs-handoff-version", contract.version) ||
        !property32(provider, "qcom,ufs-handoff-flags", contract.flags) ||
        !property32(provider, "qcom,ufs-core-clock-hz", contract.coreHz) ||
        !property32(provider, "qcom,ufs-unipro-clock-hz", contract.uniproHz) ||
        !property32(provider, "dma-address-bits", contract.addressBits) ||
        !property32(provider, "lanes-per-direction", contract.lanes) || !contract.valid()) {
        IOLog("QcomUFS: missing/invalid platform clock, power, reset, NoC or DMA contract\n");
        return false;
    }
    laneLimit = contract.lanes; addressBits = contract.addressBits;
    warmHandoff = contract.version == 2;
    if(warmHandoff && (!property32(provider,"qcom,ufs-hs-gear",firmwareGear) || firmwareGear<1 || firmwareGear>3))return false;
    if(warmHandoff && (!property32(provider,"qcom,ufs-hs-power-mode",firmwarePowerMode) || (firmwarePowerMode!=0x11 && firmwarePowerMode!=0x44)))return false;
    uint32_t polling=0;
    if (provider->getProperty("qcom,ufs-polled-completion")) {
        if (!property32(provider,"qcom,ufs-polled-completion",polling) || polling>1 || !warmHandoff) return false;
    }
    polledCompletion=polling==1;
    // If the platform supplies an IOMapper, use its device domain. Physical
    // addressing is permitted only with the separate firmware bypass pledge.
    mapper = IOMapper::copyMapperForDevice(provider);
    if (!mapper && !(contract.flags & 16)) {
        IOLog("QcomUFS: no device IOMapper and no firmware DMA bypass handoff\n"); return false;
    }
    if (auto coherent = provider->getProperty("dma-coherent")) {
        auto data = OSDynamicCast(OSData, coherent);
        if (coherent != kOSBooleanTrue && (!data || data->getLength())) return false;
        dmaCoherent = true;
    }
    if (provider->getDeviceMemoryCount() != 2) return false;
    hostMap = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
    phyMap = provider->mapDeviceMemoryWithIndex(1, kIOMapInhibitCache);
    if (!hostMap || !phyMap || hostMap->getLength() < 0x100 || phyMap->getLength() < 0x1000)
        return false;
    regs.base = reinterpret_cast<volatile uint8_t *>(hostMap->getVirtualAddress());
    phy.base = reinterpret_cast<volatile uint8_t *>(phyMap->getVirtualAddress());
    uint32_t major = regs.read(UFS::HWVER) >> 28;
    // The SDM835 v2 controller misreports VER; use the audited HCI 2.0 ABI.
    // Do not apply these PHY/UniPro settings to a later Qualcomm controller.
    if ((!warmHandoff && major != 2) || (warmHandoff && regs.read(UFS::HWVER) != 0x30010000)) {
        IOLog("QcomUFS: unsupported controller revision %u for handoff %u\n", major, contract.version); return false;
    }
    queueSlots = (regs.read(UFS::CAP) & 31) + 1;
    if (queueSlots < 2) return false;
    if (!(regs.read(UFS::CAP) & (1u << 24))) addressBits = 32;
    setProperty("UFSHCI Version", 0x200, 32);
    setProperty("UFS Queue Depth", queueSlots, 32);
    setProperty("DMA Address Bits", addressBits, 32);
    setProperty("DMA Uses IOMapper", mapper != nullptr);
    setProperty("Firmware Warm Handoff", warmHandoff);
    setProperty("Polled Completion", polledCompletion);
    IOLog("QcomUFS: platform contract v%u, controller=%08x, DMA=%u-bit, warm=%u\n",
        contract.version, regs.read(UFS::HWVER), addressBits, warmHandoff);
    return true;
}

bool QcomUFS::initializeLink() {
    UFS::Host<Registers> host(regs);
    hardwareOwned = true;
    IOLog("QcomUFS: stopping inherited HCE=%08x HCS=%08x\n",regs.read(UFS::HCE),regs.read(UFS::HCS));
    if (!host.stop()) return false;
    IOLog("QcomUFS: inherited DMA quiesced\n");
    dmaQuiesced = true;
    if (warmHandoff) {
        // The bridge validated this PHY/rail/clock/NoC state by independent DMA
        // after EFI exit. Retain the trained link instead of cold-resetting it.
        if (!host.enable()) return false;
        IOLog("QcomUFS: HCE enabled HCS=%08x\n",regs.read(UFS::HCS));
        dmaQuiesced = false;
        regs.write(UFS::AHIT, 0); regs.write(UFS::IACR, 0);
        regs.write(UFS::SYS1CLK,200);
        uint32_t core;
        if(!host.get(0xd002,core) || !host.set(0xd002,(core&~0x1ffu)|150) || !host.set(0x155e,0))return false;
        IOLog("QcomUFS: retained-link timer core-control=%08x -> %08x SYS1CLK=%u\n",core,(core&~0x1ffu)|150,regs.read(UFS::SYS1CLK));
        return (regs.read(UFS::HCS) & 1) || (host.uic(0x16, 0, 0) && (regs.read(UFS::HCS) & 1));
    }
    // Firmware has relinquished both descriptor lists. HCE is now off, so
    // firmware descriptors cannot be fetched while the PHY is reprogrammed.
    regs.write(UFS::CFG1, regs.read(UFS::CFG1) | 3); // QUniPro + PHY reset
    IODelay(1000);
    QcomUFSPhy::start(phy);
    regs.write(UFS::CFG1, regs.read(UFS::CFG1) & ~2u);
    IODelay(1000);
    phy.write(QcomUFSPhy::Start, phy.read(QcomUFSPhy::Start) | 1);
    if (!UFS::Host<Registers>(phy).wait(QcomUFSPhy::Ready, 1, 1, 10000)) return false;
    dmaQuiesced = false;
    if (!host.enable()) return false;
    regs.write(UFS::AHIT, 0); // Explicit Hibern8 only, no unsupported auto-H8.
    regs.write(UFS::IACR, 0); // Completion per command; no broken aggregation.
    regs.write(UFS::SYS1CLK, 200);
    uint32_t txState = 0;
    for (unsigned retry = 0; retry < 100; ++retry) {
        if (!host.get(0x41, txState)) return false; // MPHY_TX_FSM_STATE, lane 0
        if (txState == 1) break; // Hibern8, ready for link startup
        IODelay(100);
    }
    if (txState != 1) return false;
    uint32_t core;
    if (!host.get(0xd002, core) || !host.set(0xd002, (core & ~0x1ffu) | 150) ||
        !host.set(0x155e, 0)) return false;
    bool linked = false;
    for (unsigned retry = 0; retry < 3 && !linked; ++retry)
        linked = host.uic(0x16, 0, 0) && (regs.read(UFS::HCS) & 1);
    return linked;
}

bool QcomUFS::negotiateLink() {
    UFS::Host<Registers> host(regs);
    uint32_t rxGear, txGear, rxLanes, txLanes;
    if (!host.get(UFS::MaxRXHS, rxGear) || !host.get(UFS::MaxRXHS, txGear, true) ||
        !host.get(UFS::ConnectedRX, rxLanes) || !host.get(UFS::ConnectedTX, txLanes) ||
        !rxGear || !txGear || !rxLanes || !txLanes) return false;
    unsigned gear = rxGear < txGear ? rxGear : txGear;
    unsigned powerMode=warmHandoff ? firmwarePowerMode : 0x44;
    if (gear > (warmHandoff ? firmwareGear : 3)) gear = warmHandoff ? firmwareGear : 3;
    if (rxLanes > laneLimit) rxLanes = laneLimit;
    if (txLanes > laneLimit) txLanes = laneLimit;
    // Use FASTAUTO on v2: peer DME access is unreliable in plain FAST mode.
    // Rate-B PHY calibration is selected before the power-mode change.
    if (!host.configureTimeouts() || !host.set(UFS::RXGear, gear) || !host.set(UFS::TXGear, gear) ||
        !host.set(UFS::RXLanes, rxLanes) || !host.set(UFS::TXLanes, txLanes) ||
        !host.set(UFS::RXTermination, 1) || !host.set(UFS::TXTermination, 1) ||
        !host.set(UFS::HSSeries, 2) || !host.set(UFS::PowerMode, powerMode, UFS::PowerDone)) return false;
    uint32_t value;
    if (!host.get(UFS::RXGear, value) || value != gear ||
        !host.get(UFS::TXGear, value) || value != gear ||
        !host.get(UFS::RXLanes, value) || value != rxLanes ||
        !host.get(UFS::TXLanes, value) || value != txLanes ||
        !host.get(UFS::HSSeries, value) || value != 2 ||
        !host.get(UFS::PowerMode, value) || value != powerMode) return false;
    negotiatedGear = gear; negotiatedRXLanes = rxLanes; negotiatedTXLanes = txLanes;
    setProperty("UFS HS Gear", gear, 32); setProperty("UFS HS Series", "B");
    setProperty("UFS RX Lanes", rxLanes, 32); setProperty("UFS TX Lanes", txLanes, 32);
    IOLog("QcomUFS: HS-G%u Rate-B RX=%u TX=%u queue=%u mode=%02x\n", gear, rxLanes, txLanes, queueSlots,powerMode);
    return true;
}

bool QcomUFS::allocateRegion(DMARegion &region, unsigned bytes, unsigned alignment) {
    // Uncached descriptor memory avoids invalidating cache lines shared by
    // independent 32-byte UTRDs while the controller is writing a neighbour.
    region.memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone |
        kIOMapInhibitCache, bytes, alignment);
    if (!region.memory || region.memory->prepare() != kIOReturnSuccess) return false;
    region.prepared = true;
    region.dma = IODMACommand::withSpecification(IODMACommand::OutputHost64,
        addressBits, bytes, mapper ? IODMACommand::kMapped : IODMACommand::kUnmapped,
        bytes, alignment, mapper);
    if (!region.dma || region.dma->setMemoryDescriptor(region.memory) != kIOReturnSuccess) return false;
    if (region.dma->getIOMemoryDescriptor() != region.memory) return false;
    IODMACommand::Segment64 segment; UInt64 offset = 0; UInt32 count = 1;
    if (region.dma->gen64IOVMSegments(&offset, &segment, &count) != kIOReturnSuccess ||
        count != 1 || offset != bytes || segment.fLength != bytes ||
        (segment.fIOVMAddr & (alignment - 1)) || !UFS::addressFits(segment.fIOVMAddr, bytes, addressBits))
        return false;
    region.address = segment.fIOVMAddr; region.bytes = region.memory->getBytesNoCopy();
    UFS::zero(region.bytes, bytes); return true;
}
void QcomUFS::freeRegion(DMARegion &region) {
    if (region.dma) { region.dma->clearMemoryDescriptor(); region.dma->release(); region.dma = nullptr; }
    if (region.memory) { if (region.prepared) region.memory->complete(); region.memory->release(); region.memory = nullptr; region.prepared = false; }
    region.bytes = nullptr;
}

bool QcomUFS::initializeLists() {
    if (!allocateRegion(transferList, 1024, 1024) ||
        !allocateRegion(commandList, sizeof(UFS::CommandDescriptor) * queueSlots, 128) ||
        !allocateRegion(taskList, 1024, 1024)) return false;
    regs.write(UFS::UTRLBA, uint32_t(transferList.address));
    regs.write(UFS::UTRLBAU, uint32_t(transferList.address >> 32));
    regs.write(UFS::UTMRLBA, uint32_t(taskList.address));
    regs.write(UFS::UTMRLBAU, uint32_t(taskList.address >> 32));
    regs.write(UFS::IS, UINT32_MAX);
    if (!UFS::Host<Registers>(regs).wait(UFS::HCS, 0xe, 0xe, 100000)) return false;
    regs.write(UFS::UTMRLRSR, 1); regs.write(UFS::UTRLRSR, 1);
    regs.write(UFS::IE, polledCompletion ? 0 : UFS::Interrupts);
    IOLog("QcomUFS: DMA lists UTRL=%llx UCD=%llx UTMR=%llx IE=%08x\n",
        transferList.address,commandList.address,taskList.address,regs.read(UFS::IE));
    return true;
}

bool QcomUFS::start(IOService *provider) {
    IOLog("QcomUFS: start requested\n");
    recoveredUICNotifications=0;
    linkChecking=false;
    if (!super::start(provider)) return false;
    if (!provider->open(this)) return false;
    providerOpened = true;
    if (provider->getResources() != kIOReturnSuccess) { releaseResources(); return false; }
    if (!configurePlatform(provider)) { releaseResources(); return false; }
    workLoop = IOWorkLoop::workLoop();
    if (workLoop) gate = IOCommandGate::commandGate(this);
    if (gate && workLoop->addEventSource(gate) == kIOReturnSuccess) gateAdded = true;
    if (gateAdded && !polledCompletion) interrupt = IOInterruptEventSource::interruptEventSource(this, interruptAction, provider, 0);
    if (interrupt && workLoop->addEventSource(interrupt) == kIOReturnSuccess) interruptAdded = true;
    if (gateAdded && (interruptAdded || polledCompletion)) timer = IOTimerEventSource::timerEventSource(this, timerAction);
    if (timer && workLoop->addEventSource(timer) == kIOReturnSuccess) timerAdded = true;
    finisher = thread_call_allocate_with_options(finish, this, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    powerCall = thread_call_allocate_with_options(changePower, this, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    preparer = thread_call_allocate_with_options(prepareGated, this, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    backgroundCall = thread_call_allocate_with_options(background, this, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    linkCheckCall = thread_call_allocate_with_options(checkLink, this, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    nanoseconds_to_absolutetime(30000000000ULL, &timeoutTicks);
    if (!timerAdded || !finisher || !powerCall || !preparer || !backgroundCall || !linkCheckCall) {
        IOLog("QcomUFS: event setup failed gate=%u interrupt=%u timer=%u\n",gateAdded,interruptAdded,timerAdded);
        releaseResources(); return false;
    }
    IOLog("QcomUFS: event sources ready; initializing retained link\n");
    if (!initializeLink()) {
        IOLog("QcomUFS: link initialization failed HCE=%08x HCS=%08x IS=%08x\n",regs.read(UFS::HCE),regs.read(UFS::HCS),regs.read(UFS::IS));
        releaseResources(); return false;
    }
    IOLog("QcomUFS: link ready; allocating command lists\n");
    if (!initializeLists()) {
        IOLog("QcomUFS: command list setup failed\n");
        releaseResources(); return false;
    }
    running = true;
    IOLog("QcomUFS: command lists ready; discovering LUNs\n");
    if (interruptAdded) interrupt->enable();
    timer->setTimeoutMS(polledCompletion ? 1 : 10);
    if (polledCompletion) IOLog("QcomUFS: firmware SPI forwarding unavailable; using 1ms polled DMA completion\n");
    if (!discover()) { IOLog("QcomUFS: LUN discovery failed\n"); releaseResources(); return false; }
    // Publish only after every enabled LU and its capacity has been checked.
    for (unsigned i = 0; i < UFS::MaxLUNs; ++i) if (luns[i].enabled) {
        luns[i].device = new QcomUFSBlockDevice;
        if (!luns[i].device || !luns[i].device->initLUN(this, i) || !luns[i].device->attach(this)) {
            releaseResources(); return false;
        }
    }
    published = true;
    PMinit(); pmStarted = true;
    // State 0 retains all platform clocks/rails and uses UIC Hibern8. It does
    // not authorize platform power collapse or loss of the IOMapper domain.
    static IOPMPowerState states[2] = {
        {1, 0, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
    };
    provider->joinPMtree(this); registerPowerDriver(this, states, 2);
    changePowerStateTo(1);
    for (unsigned i = 0; i < UFS::MaxLUNs; ++i) if (luns[i].device) luns[i].device->registerService();
    registerService(); return true;
}

IOWorkLoop *QcomUFS::getWorkLoop() const { return workLoop; }
bool QcomUFS::requestTerminate(IOService *, IOOptionBits) { return !published; }

IOReturn QcomUFS::prepare(Request *request, IOMemoryDescriptor *buffer) {
    if (!buffer) return request->length ? kIOReturnBadArgument : kIOReturnSuccess;
    if (!request->length || buffer->getLength() != request->length) return kIOReturnBadArgument;
    request->buffer = buffer; buffer->retain();
    IOReturn result = buffer->prepare();
    if (result != kIOReturnSuccess) return result;
    request->prepared = true;
    // Keep physical-mode walking free of IODMACommand's hidden bounce pages:
    // validate the device width on every PRD instead. IOStorageFamily supplies
    // cache-line-aligned data via the constraints on the block-device nub.
    request->dma = IODMACommand::withSpecification(IODMACommand::OutputHost64, mapper ? addressBits : 64,
        UFS::MaxPRDBytes, mapper ? IODMACommand::kMapped : IODMACommand::kUnmapped,
        UFS::MaxTransfer, 1, mapper);
    if (!request->dma) return kIOReturnNoMemory;
    // Mapping/wiring can sleep: this entire method runs in the submitting
    // thread, outside the controller's workloop gate.
    result = request->dma->setMemoryDescriptor(buffer);
    if (result != kIOReturnSuccess) return result;
    request->dmaPrepared = true;
    if (request->dma->getIOMemoryDescriptor() != buffer) return kIOReturnDMAError;
    UInt64 offset = 0, mappedBytes = 0;
    while (offset < request->length) {
        IODMACommand::Segment64 segments[16]; UInt32 count = 16; UInt64 previous = offset;
        result = request->dma->gen64IOVMSegments(&offset, segments, &count);
        if (result != kIOReturnSuccess || !count || offset <= previous || offset > request->length)
            return result == kIOReturnSuccess ? kIOReturnDMAError : result;
        for (unsigned i = 0; i < count; ++i) {
            if (!dmaCoherent && (segments[i].fIOVMAddr & 63)) return kIOReturnNotAligned;
            mappedBytes += segments[i].fLength;
            if (mappedBytes > request->length || (!dmaCoherent && mappedBytes < request->length &&
                (segments[i].fLength & 63))) return kIOReturnNotAligned;
            if (!UFS::appendPRD(request->prds, request->prdCount, segments[i].fIOVMAddr,
                segments[i].fLength, addressBits)) return kIOReturnDMAError;
        }
    }
    if (mappedBytes != request->length) return kIOReturnDMAError;
    // Explicit cache maintenance also covers physical DMA without a system
    // mapper. Clean+invalidate before read, clean before write. No bounce or
    // mapping allocation occurs after this synchronization point.
    if (!dmaCoherent) {
        result = request->dma->getIOMemoryDescriptor()->performOperation(buffer->getDirection() == kIODirectionIn ?
            kIOMemoryIncoherentIOFlush : kIOMemoryIncoherentIOStore, 0, request->length);
        if (result != kIOReturnSuccess) return result;
    }
    return kIOReturnSuccess;
}
void QcomUFS::dispose(Request *request) {
    if (request->dma) { request->dma->clearMemoryDescriptor(); request->dma->release(); }
    if (request->prepared) request->buffer->complete();
    OSSafeReleaseNULL(request->buffer);
    if (request->syncLock) IOLockFree(request->syncLock);
    IODelete(request, Request, 1);
}

IOReturn QcomUFS::enqueueRequest(OSObject *owner, void *arg, void *, void *, void *) {
    auto self = static_cast<QcomUFS *>(owner); auto request = static_cast<Request *>(arg);
    if (!self->running || self->suspended || (self->powerChanging && !request->synchronous && !request->admitted))
        return kIOReturnNotReady;
    if (self->pendingCount >= UFS::MaxPending) return kIOReturnBusy;
    request->deadline = mach_absolute_time() + self->timeoutTicks;
    if(request->linkProbe) {
        request->next=self->pendingHead;self->pendingHead=request;
        if(!self->pendingTail)self->pendingTail=request;
    } else {
        if (self->pendingTail) self->pendingTail->next = request;
        else self->pendingHead = request;
        self->pendingTail = request;
    }
    ++self->pendingCount;
    self->submitPending(); return kIOReturnSuccess;
}
void QcomUFS::submitPending() {
    if (!running || suspended) return;
    unsigned tag; Request *request;
    if(linkChecking && (!pendingHead || !pendingHead->linkProbe))return;
    while (UFS::selectPending(pendingHead, pendingTail, pendingCount, active, queueSlots, luns,
        activeMask, tag, request)) {
        auto &command = static_cast<UFS::CommandDescriptor *>(commandList.bytes)[tag];
        UFS::zero(&command, sizeof(command));
        for (unsigned i = 0; i < 512; ++i) command.request[i] = request->packet[i];
        command.request[3] = tag;
        for (unsigned i = 0; i < request->prdCount; ++i) command.prdt[i] = request->prds[i];
        unsigned direction = request->length ? (request->buffer->getDirection() == kIODirectionOut ? 1 : 2) : 0;
        auto &transfer = static_cast<UFS::Transfer *>(transferList.bytes)[tag];
        UFS::initTransfer(transfer, commandList.address + tag * sizeof(command), request->prdCount, direction);
        active[tag] = request; activeMask |= 1u << tag;
        // The doorbell is W1S. Never read/modify/write it: completions can
        // clear older bits concurrently and a stale value would reissue I/O.
        regs.write(UFS::UTRLDBR, 1u << tag);
        if (request->ordered || linkChecking) break;
    }
}

void QcomUFS::appendDone(Request *request, IOReturn result, uint32_t actual) {
    request->result = result; request->actual = actual; request->next = nullptr;
    if (doneTail) doneTail->next = request; else doneHead = request;
    doneTail = request; thread_call_enter(finisher);
}
void QcomUFS::complete(unsigned tag) {
    Request *request = active[tag];
    auto &transfer = static_cast<UFS::Transfer *>(transferList.bytes)[tag];
    const auto &command = static_cast<UFS::CommandDescriptor *>(commandList.bytes)[tag];
    auto result = UFS::response(transfer, command.response, request->responseType, tag, request->lun, request->length);
    for (unsigned i = 0; i < 512; ++i) request->reply[i] = command.response[i];
    active[tag] = nullptr; activeMask &= ~(1u << tag);
    IOReturn status = kIOReturnIOError;
    switch (result.result) {
        case UFS::Result::Good: status = kIOReturnSuccess; break;
        case UFS::Result::Busy: status = kIOReturnBusy; break;
        case UFS::Result::QueryRejected: status = command.response[6] == 0xfc ? kIOReturnNotFound : kIOReturnUnsupported; break;
        case UFS::Result::CheckCondition:
            status = result.senseKey == 5 && (result.asc == 0x20 || result.asc == 0x24) ? kIOReturnUnsupported :
                result.senseKey == 7 ? kIOReturnNotWritable :
                (result.senseKey == 2 || result.senseKey == 6) ? kIOReturnNotReady : kIOReturnIOError;
            IOLog("QcomUFS: LUN=%u SCSI=%02x sense=%02x/%02x/%02x\n", request->lun,
                result.status, result.senseKey, result.asc, result.ascq); break;
        case UFS::Result::ShortTransfer: status = kIOReturnUnderrun; break;
        default: break;
    }
    appendDone(request, status, result.bytes);
    if (request->responseType == UFS::Response && (command.response[9] & 1))
        thread_call_enter(backgroundCall);
    if (result.result == UFS::Result::Controller || result.result == UFS::Result::Protocol) {
        IOLog("QcomUFS: invalid completion kind=%u OCS=%08x request=%02x/%02x/%02x reply=%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x sense-length=%02x%02x\n",
            unsigned(result.result),transfer.ocs,request->packet[0],request->packet[12],request->packet[16],
            command.response[0],command.response[1],command.response[2],command.response[3],command.response[4],command.response[5],
            command.response[6],command.response[7],command.response[8],command.response[9],command.response[10],command.response[11],
            command.response[32],command.response[33]);
        failController(kIOReturnIOError);
    }
}
void QcomUFS::interruptAction(OSObject *owner, IOInterruptEventSource *, int) {
    auto self = static_cast<QcomUFS *>(owner);
    if (!self->running) return;
    uint32_t status = self->regs.read(UFS::IS) & UFS::Interrupts;
    self->regs.write(UFS::IS, status);
    if (status & (UFS::Fatal | UFS::UICError)) {
        // Snapshot layer errors before resetting. No failed write is replayed.
        uint32_t pa=self->regs.read(UFS::UECPA),dl=self->regs.read(UFS::UECDL),
            n=self->regs.read(UFS::UECN),t=self->regs.read(UFS::UECT),dme=self->regs.read(UFS::UECDME);
        if ((status & UFS::Fatal) || UFS::uicRequiresRecovery(pa,dl,n,t,dme)) {
            IOLog("QcomUFS: IS=%08x PA=%08x DL=%08x N=%08x T=%08x DME=%08x\n",status,pa,dl,n,t,dme);
            self->failController(kIOReturnIOError); return;
        }
        ++self->recoveredUICNotifications;
        if(self->recoveredUICNotifications<=4 || !(self->recoveredUICNotifications%64))
            IOLog("QcomUFS: hardware link retry notification %u PA=%08x DL=%08x\n",self->recoveredUICNotifications,pa,dl);
        if(self->published && !self->linkChecking) {
            self->linkChecking=true;
            thread_call_enter(self->linkCheckCall);
        }
    }
    uint32_t completed = UFS::completedSlots(self->activeMask, self->regs.read(UFS::UTRLDBR));
    for (unsigned i = 0; i < self->queueSlots && self->running; ++i)
        if (completed & (1u << i)) self->complete(i);
    self->submitPending();
}
void QcomUFS::timerAction(OSObject *owner, IOTimerEventSource *timer) {
    auto self = static_cast<QcomUFS *>(owner);
    if (!self->running) return;
    // Reap by doorbell as well as by IRQ: a lost interrupt cannot strand DMA.
    interruptAction(owner, nullptr, 0);
    uint64_t now = mach_absolute_time();
    bool timedOut = self->pendingHead && self->pendingHead->deadline <= now;
    for (unsigned i = 0; i < self->queueSlots; ++i)
        if (self->active[i] && self->active[i]->deadline <= now) timedOut = true;
    if (timedOut && self->running) self->failController(kIOReturnTimeout);
    if (self->running) timer->setTimeoutMS(self->polledCompletion ? 1 : 10);
}
void QcomUFS::failController(IOReturn reason) {
    IOLog("QcomUFS: controller offline reason=%08x active=%08x DBR=%08x IS=%08x\n",reason,activeMask,regs.read(UFS::UTRLDBR),regs.read(UFS::IS));
    running = false;
    dmaQuiesced = UFS::Host<Registers>(regs).stop();
    if (!dmaQuiesced) {
        // Releasing wired caller buffers while DMA is still possible would
        // corrupt arbitrary kernel memory. Stop rather than fake completion.
        panic("QcomUFS: controller failed to quiesce DMA");
    }
    timer->cancelTimeout();
    for (unsigned i = 0; i < queueSlots; ++i) if (active[i]) {
        appendDone(active[i], reason); active[i] = nullptr;
    }
    activeMask = 0;
    while (pendingHead) {
        Request *request = pendingHead; pendingHead = request->next;
        appendDone(request, reason);
    }
    pendingTail = nullptr; pendingCount = 0;
    while (mappingHead) {
        Request *request = mappingHead; mappingHead = request->next;
        appendDone(request, reason);
    }
    mappingTail = nullptr; mappingCount = 0;
    setProperty("UFS Controller Failed", true);
    mediaNotificationPending = true; thread_call_enter(finisher);
}
IOReturn QcomUFS::takeDone(OSObject *owner, void *arg, void *notification, void *, void *) {
    auto self = static_cast<QcomUFS *>(owner);
    *static_cast<Request **>(arg) = self->doneHead;
    self->doneHead = self->doneTail = nullptr;
    if (notification) {
        *static_cast<bool *>(notification) = self->mediaNotificationPending;
        self->mediaNotificationPending = false;
    }
    return kIOReturnSuccess;
}
IOReturn QcomUFS::mappingAction(OSObject *owner, void *arg, void *operation, void *statusArg, void *) {
    auto self = static_cast<QcomUFS *>(owner);
    uintptr_t op = reinterpret_cast<uintptr_t>(operation);
    if (op == 1) { // Admit a request received on our workloop gate.
        auto request = static_cast<Request *>(arg);
        if (!self->running || self->suspended || self->powerChanging) return kIOReturnNotReady;
        if (self->mappingCount + self->mappingInFlight + self->pendingCount >= UFS::MaxPending) return kIOReturnBusy;
        request->buffer->retain(); request->admitted = true;
        if (self->mappingTail) self->mappingTail->next = request; else self->mappingHead = request;
        self->mappingTail = request; ++self->mappingCount;
        thread_call_enter(self->preparer); return kIOReturnSuccess;
    }
    if (op == 2) { // Preparation is complete; retire outside-gate ownership.
        --self->mappingInFlight;
        auto request = static_cast<Request *>(arg);
        IOReturn status = static_cast<IOReturn>(reinterpret_cast<uintptr_t>(statusArg));
        if (status == kIOReturnSuccess) status = enqueueRequest(owner, request, nullptr, nullptr, nullptr);
        if (status != kIOReturnSuccess) self->appendDone(request, status);
        return kIOReturnSuccess;
    }
    if (op == 3) return self->mappingCount || self->mappingInFlight ? kIOReturnBusy : kIOReturnSuccess;
    auto output = static_cast<Request **>(arg);
    *output = self->mappingHead;
    if (*output) {
        self->mappingHead = (*output)->next; if (!self->mappingHead) self->mappingTail = nullptr;
        (*output)->next = nullptr; --self->mappingCount; ++self->mappingInFlight;
    }
    return kIOReturnSuccess;
}
void QcomUFS::prepareGated(thread_call_param_t arg, thread_call_param_t) {
    auto self = static_cast<QcomUFS *>(arg);
    for (;;) {
        Request *request = nullptr; self->gate->runAction(mappingAction, &request);
        if (!request) break;
        IOMemoryDescriptor *buffer = request->buffer; request->buffer = nullptr;
        IOReturn status = self->prepare(request, buffer); buffer->release();
        self->gate->runAction(mappingAction, request, reinterpret_cast<void *>(2),
            reinterpret_cast<void *>(static_cast<uintptr_t>(status)));
    }
}
void QcomUFS::background(thread_call_param_t arg, thread_call_param_t) {
    auto self = static_cast<QcomUFS *>(arg); uint8_t reply[4] = {};
    if (self->query(3, 0x0e, 0, reply) != kIOReturnSuccess) return; // EE_STATUS
    const uint32_t events = UFS::be32(reply);
    self->setProperty("UFS Exception Events", events, 32);
    if (events & 4) { // Urgent background operations, a valid device notification.
        IOReturn status = self->query(6, 4, 0, nullptr); // Enable automatic BKOPS.
        self->setProperty("UFS Background Operations Enabled", status == kIOReturnSuccess);
        if (status != kIOReturnSuccess) IOLog("QcomUFS: cannot enable background operations: %08x\n", status);
    }
}
void QcomUFS::checkLink(thread_call_param_t arg,thread_call_param_t) {
    auto self=static_cast<QcomUFS *>(arg);
    // Samsung DL-NAC quirk: pause new data submissions, allow 50ms for a
    // severe error, then require a successful NOP. Never replay data writes.
    self->regs.delay(50000);
    Request *nop=IONewZero(Request,1);
    IOReturn status=kIOReturnNoMemory;
    if(nop) {
        nop->packet[0]=UFS::NOPOut;nop->responseType=UFS::NOPIn;nop->linkProbe=true;
        status=self->execute(nop);self->dispose(nop);
    }
    if(status==kIOReturnSuccess) {
        uint32_t mode=0;
        if(!UFS::Host<Registers>(self->regs).get(UFS::PowerMode,mode) || mode!=(self->warmHandoff?self->firmwarePowerMode:0x44))status=kIOReturnIOError;
    }
    self->gate->runAction(linkChecked,reinterpret_cast<void *>(static_cast<uintptr_t>(status)));
}
IOReturn QcomUFS::linkChecked(OSObject *owner,void *result,void *,void *,void *) {
    auto self=static_cast<QcomUFS *>(owner);
    IOReturn status=static_cast<IOReturn>(reinterpret_cast<uintptr_t>(result));
    if(!self->running)return kIOReturnOffline;
    if(status!=kIOReturnSuccess) {self->failController(status);return status;}
    self->linkChecking=false;
    IOLog("QcomUFS: link NOP and HS mode verified after NAC; resuming queued I/O\n");
    self->submitPending();return kIOReturnSuccess;
}
void QcomUFS::finish(thread_call_param_t arg, thread_call_param_t) {
    auto self = static_cast<QcomUFS *>(arg);
    // ONCE prevents overlapping callbacks. The gate protects only list
    // detachment; mapper teardown, cache sync and client callbacks run ungated.
    Request *request = nullptr; bool notify = false;
    self->gate->runAction(takeDone, &request, &notify);
    while (request) {
        Request *next = request->next;
        if (request->dmaPrepared && !self->dmaCoherent && request->buffer->getDirection() == kIODirectionIn) {
            IOReturn status = request->dma->getIOMemoryDescriptor()->performOperation(kIOMemoryIncoherentIOFlush, 0, request->length);
            if (status != kIOReturnSuccess) { request->result = status; request->actual = 0; }
        }
        if (request->dma) {
            IOReturn status = request->dma->clearMemoryDescriptor();
            if (status != kIOReturnSuccess) { request->result = status; request->actual = 0; }
            request->dma->release(); request->dma = nullptr;
        }
        if (request->prepared) {
            IOReturn status = request->buffer->complete(); request->prepared = false;
            if (status != kIOReturnSuccess) { request->result = status; request->actual = 0; }
        }
        if (request->synchronous) {
            IOLockLock(request->syncLock); request->completed = true;
            IOLockWakeup(request->syncLock, &request->completed, false);
            IOLockUnlock(request->syncLock);
        }
        else {
            auto completion = request->completion; auto status = request->result; auto bytes = request->actual;
            self->dispose(request);
            completion.action(completion.target, completion.parameter, status, bytes);
        }
        request = next;
    }
    if (notify && self->published) {
        for (unsigned i = 0; i < UFS::MaxLUNs; ++i) if (self->luns[i].device)
            self->luns[i].device->messageClients(kIOMessageMediaStateHasChanged,
                nullptr); // IOMediaStateOffline = 0 in IOBlockStorageDriver.
    }
}

IOReturn QcomUFS::execute(Request *request) {
    request->synchronous = true;
    request->syncLock = IOLockAlloc();
    if (!request->syncLock) return kIOReturnNoMemory;
    IOReturn status = gate->runAction(enqueueRequest, request);
    if (status != kIOReturnSuccess) return status;
    IOLockLock(request->syncLock);
    while (!request->completed) IOLockSleep(request->syncLock, &request->completed, THREAD_UNINT);
    status = request->result; IOLockUnlock(request->syncLock);
    return status;
}
IOReturn QcomUFS::query(uint8_t opcode, uint8_t idn, uint8_t index, uint8_t *reply) {
    Request *request = IONewZero(Request, 1); if (!request) return kIOReturnNoMemory;
    UFS::query(request->packet, 0, opcode, idn, index, opcode == 1 ? 255 : 0);
    request->responseType = UFS::QueryResponse;
    IOReturn result = execute(request);
    if (result == kIOReturnSuccess) {
        const uint8_t *p = request->reply;
        if (p[12] != opcode || p[13] != idn || p[14] != index || p[15]) result = kIOReturnIOError;
        else if (opcode == 1) {
            unsigned length = UFS::be16(p + 18);
            if (length < 2 || length > 255 || length != UFS::be16(p + 10) ||
                p[32] != length || p[33] != idn) result = kIOReturnIOError;
            else for (unsigned i = 0; i < length; ++i) reply[i] = p[32 + i];
        } else if (reply) UFS::put32(reply, UFS::be32(p + 20));
    }
    dispose(request); return result;
}
IOReturn QcomUFS::scsi(uint8_t lun, const uint8_t *cdb, IOMemoryDescriptor *buffer, uint32_t bytes, bool ordered) {
    if (workLoop->inGate()) return kIOReturnCannotLock;
    Request *request = IONewZero(Request, 1); if (!request) return kIOReturnNoMemory;
    request->length = bytes; request->lun = lun; request->responseType = UFS::Response; request->ordered = ordered;
    UFS::scsi(request->packet, lun, 0, cdb, bytes, buffer && buffer->getDirection() == kIODirectionOut, ordered);
    IOReturn status = prepare(request, buffer);
    if(status!=kIOReturnSuccess)IOLog("QcomUFS: LUN=%u CDB=%02x DMA prepare=%08x bytes=%u buffer-length=%llu PRDs=%u\n",lun,cdb[0],status,bytes,buffer?buffer->getLength():0,request->prdCount);
    if (status == kIOReturnSuccess) status = execute(request);
    dispose(request); return status;
}
bool QcomUFS::discover() {
    Request *nop = IONewZero(Request, 1); if (!nop) return false;
    nop->packet[0] = UFS::NOPOut; nop->responseType = UFS::NOPIn;
    IOReturn status = execute(nop);
    IOLog("QcomUFS: NOP status=%08x response=%02x/%02x/%02x\n",status,nop->reply[0],nop->reply[2],nop->reply[3]);
    dispose(nop); if (status != kIOReturnSuccess) return false;
    status=query(6,1,0,nullptr);
    IOLog("QcomUFS: fDeviceInit SET status=%08x\n",status);
    if (status != kIOReturnSuccess) return false;
    bool initialized = false;
    for (unsigned i = 0; i < 100; ++i) {
        uint8_t flag[4] = {};
        if ((status=query(5, 1, 0, flag)) != kIOReturnSuccess) {IOLog("QcomUFS: fDeviceInit GET status=%08x\n",status);return false;}
        if (!UFS::be32(flag)) { initialized = true; break; }
        regs.delay(10000); // Boot discovery must not depend on scheduler timers.
    }
    if (!initialized) return false;
    uint8_t device[255] = {};
    status=query(1,0,0,device);
    IOLog("QcomUFS: device descriptor status=%08x length=%u LUs=%u\n",status,device[0],device[6]);
    if (status != kIOReturnSuccess || device[0] < 0x1c || !device[6]) return false;
    const uint16_t manufacturer = UFS::be16(device + 0x18);
    IOLog("QcomUFS: manufacturer=%04x specification=%04x\n",manufacturer,UFS::be16(device+0x10));
    setProperty("UFS Manufacturer ID", manufacturer, 16);
    setProperty("UFS Specification Version", UFS::be16(device + 0x10), 16);
    UFS::Host<Registers> host(regs);
    if ((manufacturer == 0x1ce || manufacturer == 0x145) && !host.tuneActivation()) {IOLog("QcomUFS: activation timing adjustment failed\n");return false;}
    if (manufacturer == 0x1ad) {
        uint32_t config;
        if (!host.get(0x9000, config) || !host.set(0x9000, config | (1u << 12))) return false;
    }
    if (!negotiateLink()) {IOLog("QcomUFS: power mode negotiation failed\n");return false;}
    OSSharedPtr<IOBufferMemoryDescriptor> capacity(IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionIn | kIOMemoryMapperNone, 64, 64), OSNoRetain);
    if (!capacity) return false;
    capacity->setLength(32); // Retain a private whole cache line for a 32-byte response.
    unsigned enabled = 0;
    for (unsigned lun = 0; lun < UFS::MaxLUNs; ++lun) {
        uint8_t unit[255] = {};
        status = query(1, 2, lun, unit);
        IOLog("QcomUFS: unit %u descriptor status=%08x length=%u enabled=%u\n",lun,status,unit[0],unit[3]);
        if (status == kIOReturnNotFound) continue;
        if (status != kIOReturnSuccess) return false;
        if (unit[0] < 0x23 || unit[2] != lun) return false;
        if (!unit[3]) continue;
        uint8_t cdb[16] = {}; bool ready = false;
        for (unsigned retry = 0; retry < 20; ++retry) {
            status = scsi(lun, cdb, nullptr, 0);
            IOLog("QcomUFS: unit %u TEST UNIT READY status=%08x\n",lun,status);
            if (status == kIOReturnSuccess) { ready = true; break; }
            if (status != kIOReturnNotReady) break;
            regs.delay(20000); // Bounded startup-only reset/not-ready backoff.
        }
        if (!ready) return false;
        cdb[0] = 0x9e; cdb[1] = 0x10; UFS::put32(cdb + 10, 32); // READ CAPACITY(16)
        capacity->setLength(32);
        status=scsi(lun, cdb, capacity.get(), 32);
        IOLog("QcomUFS: unit %u READ CAPACITY(16) status=%08x\n",lun,status);
        bool capacity10=false;
        if(status==kIOReturnUnsupported) {
            UFS::zero(cdb,16);cdb[0]=0x25;capacity->setLength(8);
            status=scsi(lun,cdb,capacity.get(),8);capacity10=true;
            IOLog("QcomUFS: unit %u READ CAPACITY(10) status=%08x\n",lun,status);
        }
        if(status!=kIOReturnSuccess)return false;
        const auto *p = static_cast<const uint8_t *>(capacity->getBytesNoCopy());
        uint64_t last = capacity10 ? UFS::be32(p) : UFS::be64(p);
        uint32_t size = UFS::be32(p + (capacity10 ? 4 : 8));
        IOLog("QcomUFS: unit %u capacity last=%llu size=%u descriptor-blocks=%llu shift=%u\n",lun,last,size,UFS::be64(unit+11),unit[10]);
        if(capacity10 && last==UINT32_MAX)return false;
        if (last == UINT64_MAX || size < 512 || size > 65536 || (size & (size - 1)) ||
            last + 1 > UINT64_MAX / size || unit[10] >= 32 || (1u << unit[10]) != size ||
            UFS::be64(unit + 11) != last + 1) return false;
        luns[lun].blocks = last + 1; luns[lun].blockSize = size;
        luns[lun].writeProtected = unit[5] != 0;
        luns[lun].depth = unit[6] && unit[6] < queueSlots ? unit[6] : queueSlots;
        luns[lun].enabled = true; ++enabled;
        IOLog("QcomUFS: LUN=%u blocks=%llu block-size=%u depth=%u\n",lun,last+1,size,luns[lun].depth);
    }
    return enabled && enabled == device[6];
}

IOReturn QcomUFS::readWrite(uint8_t lun, IOMemoryDescriptor *buffer, uint64_t block,
    uint64_t count, IOStorageAttributes *attributes, IOStorageCompletion *completion) {
    if (lun >= UFS::MaxLUNs || !luns[lun].enabled || !buffer || !completion || !completion->action)
        return kIOReturnBadArgument;
    auto direction = buffer->getDirection();
    if (direction != kIODirectionIn && direction != kIODirectionOut) return kIOReturnBadArgument;
    bool write = direction == kIODirectionOut;
    if (write && luns[lun].writeProtected) return kIOReturnNotWritable;
    // IsEncrypted means the caller has already encrypted these bytes; it is
    // not an ICE request. Static is a placement hint. Both transfer unchanged.
    if (attributes && (attributes->options & kIOStorageOptionReserved)) return kIOReturnUnsupported;
    Request *request = IONewZero(Request, 1); if (!request) return kIOReturnNoMemory;
    uint8_t cdb[16];
    if (!UFS::readWriteCDB(cdb, block, count, luns[lun].blockSize, luns[lun].blocks, write,
        attributes && (attributes->options & kIOStorageOptionForceUnitAccess), request->length)) {
        dispose(request); return kIOReturnBadArgument;
    }
    request->lun = lun; request->responseType = UFS::Response; request->completion = *completion;
    UFS::scsi(request->packet, lun, 0, cdb, request->length, write);
    if (workLoop->inGate()) {
        request->buffer = buffer; // mappingAction takes the asynchronous retain.
        IOReturn status = gate->runAction(mappingAction, request, reinterpret_cast<void *>(1));
        if (status != kIOReturnSuccess) { request->buffer = nullptr; dispose(request); }
        return status;
    }
    IOReturn status = prepare(request, buffer);
    if (status == kIOReturnSuccess) status = gate->runAction(enqueueRequest, request);
    if (status != kIOReturnSuccess) dispose(request);
    // On success exactly one asynchronous completion owns the request. On
    // failure there is no callback; this is IOBlockStorageDevice's contract.
    return status;
}
IOReturn QcomUFS::synchronize(uint8_t lun) {
    if (lun >= UFS::MaxLUNs || !luns[lun].enabled) return kIOReturnBadArgument;
    // UFS supports SYNCHRONIZE CACHE(10); the optional 16-byte variant may
    // return ILLEGAL REQUEST. Zero LBA/count flushes the entire LU, including
    // media whose capacity exceeds the 10-byte command's address field.
    uint8_t cdb[16] = {}; cdb[0] = 0x35;
    // An ordered barrier drains older commands and holds younger commands
    // until the flush has completed. FUA writes retain their native SCSI bit.
    return scsi(lun, cdb, nullptr, 0, true);
}
uint64_t QcomUFS::blocks(uint8_t lun) const { return luns[lun].blocks; }
uint32_t QcomUFS::blockSize(uint8_t lun) const { return luns[lun].blockSize; }
bool QcomUFS::writeProtected(uint8_t lun) const { return luns[lun].writeProtected; }
bool QcomUFS::mediaPresent(uint8_t lun) const {
    bool present = false;
    gate->runAction(mediaAction, &present, reinterpret_cast<void *>(static_cast<uintptr_t>(lun)));
    return present;
}
IOReturn QcomUFS::mediaAction(OSObject *owner, void *arg, void *lunArg, void *, void *) {
    auto self = static_cast<QcomUFS *>(owner); uintptr_t lun = reinterpret_cast<uintptr_t>(lunArg);
    *static_cast<bool *>(arg) = lun < UFS::MaxLUNs && self->luns[lun].enabled && self->running;
    return kIOReturnSuccess;
}

IOReturn QcomUFS::setPowerState(unsigned long state, IOService *) {
    if (!published || state > 1) return IOPMAckImplied;
    gate->runAction(powerAction, reinterpret_cast<void *>(state), reinterpret_cast<void *>(1));
    thread_call_enter(powerCall); return 60000000; // asynchronous ack, microseconds
}
IOReturn QcomUFS::powerAction(OSObject *owner, void *stateArg, void *begin, void *, void *) {
    auto self = static_cast<QcomUFS *>(owner);
    if (begin) { self->powerTarget = reinterpret_cast<uintptr_t>(stateArg); self->powerChanging = true; return kIOReturnSuccess; }
    if (!self->running) { self->powerChanging = false; return kIOReturnOffline; }
    if (self->linkChecking || self->pendingHead || self->activeMask || self->mappingCount || self->mappingInFlight) return kIOReturnBusy;
    UFS::Host<Registers> host(self->regs);
    bool ok = true;
    if (!self->powerTarget && !self->suspended)
        ok = host.uic(0x17, 0, 0, nullptr, UFS::HibernateEnter);
    else if (self->powerTarget && self->suspended)
        ok = host.uic(0x18, 0, 0, nullptr, UFS::HibernateExit);
    if (ok) self->suspended = !self->powerTarget;
    else self->failController(kIOReturnIOError);
    self->powerChanging = false; return ok ? kIOReturnSuccess : kIOReturnIOError;
}
void QcomUFS::changePower(thread_call_param_t arg, thread_call_param_t) {
    auto self = static_cast<QcomUFS *>(arg);
    for (unsigned retry = 0; retry < 400; ++retry) {
        if (self->gate->runAction(mappingAction, nullptr, reinterpret_cast<void *>(3)) != kIOReturnBusy) break;
        if (retry == 399) self->gate->runAction(shutdown, reinterpret_cast<void *>(kIOReturnTimeout));
        else IOSleep(10);
    }
    if (!self->powerTarget && !self->suspended) {
        for (unsigned i = 0; i < UFS::MaxLUNs; ++i) if (self->luns[i].enabled)
            if (self->synchronize(i) != kIOReturnSuccess) {
                self->gate->runAction(shutdown, reinterpret_cast<void *>(kIOReturnIOError)); break;
            }
    }
    for (unsigned retry = 0; retry < 400; ++retry) {
        IOReturn status = self->gate->runAction(powerAction);
        if (status != kIOReturnBusy) break;
        if (retry == 399) self->gate->runAction(shutdown, reinterpret_cast<void *>(kIOReturnTimeout));
        else IOSleep(10);
    }
    self->acknowledgeSetPowerState();
}
IOReturn QcomUFS::shutdown(OSObject *owner, void *reason, void *, void *, void *) {
    auto self = static_cast<QcomUFS *>(owner);
    if (self->hardwareOwned && !self->dmaQuiesced)
        self->failController(reason ? static_cast<IOReturn>(reinterpret_cast<uintptr_t>(reason)) : kIOReturnOffline);
    return kIOReturnSuccess;
}
void QcomUFS::releaseResources() {
    if (pmStarted) { PMstop(); pmStarted = false; }
    if (gateAdded) gate->runAction(shutdown);
    else if (hardwareOwned && !dmaQuiesced && !UFS::Host<Registers>(regs).stop())
        panic("QcomUFS: cannot stop DMA during startup cleanup");
    if (interruptAdded) interrupt->disable();
    if (timer) timer->cancelTimeout();
    if (powerCall) { thread_call_cancel_wait(powerCall); thread_call_free(powerCall); powerCall = nullptr; }
    if (backgroundCall) { thread_call_cancel_wait(backgroundCall); thread_call_free(backgroundCall); backgroundCall = nullptr; }
    if (linkCheckCall) { thread_call_cancel_wait(linkCheckCall); thread_call_free(linkCheckCall); linkCheckCall = nullptr; }
    if (preparer) { thread_call_cancel_wait(preparer); thread_call_free(preparer); preparer = nullptr; }
    if (finisher) { thread_call_cancel_wait(finisher); if (gateAdded) finish(this, nullptr); thread_call_free(finisher); finisher = nullptr; }
    for (unsigned i = 0; i < UFS::MaxLUNs; ++i) if (luns[i].device) {
        if (luns[i].device->getProvider() == this) luns[i].device->detach(this);
        luns[i].device->release(); luns[i].device = nullptr;
    }
    if (timerAdded) { workLoop->removeEventSource(timer); timerAdded = false; }
    if (interruptAdded) { workLoop->removeEventSource(interrupt); interruptAdded = false; }
    if (gateAdded) { workLoop->removeEventSource(gate); gateAdded = false; }
    OSSafeReleaseNULL(timer); OSSafeReleaseNULL(interrupt); OSSafeReleaseNULL(gate); OSSafeReleaseNULL(workLoop);
    freeRegion(commandList); freeRegion(taskList); freeRegion(transferList);
    OSSafeReleaseNULL(mapper); OSSafeReleaseNULL(phyMap); OSSafeReleaseNULL(hostMap);
    regs.base = phy.base = nullptr; hardwareOwned = false;
    if (providerOpened) { getProvider()->close(this); providerOpened = false; }
}
void QcomUFS::stop(IOService *provider) { releaseResources(); super::stop(provider); }
void QcomUFS::free() { releaseResources(); super::free(); }
