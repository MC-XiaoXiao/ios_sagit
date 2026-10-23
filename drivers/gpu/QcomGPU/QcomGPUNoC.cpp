// SPDX-License-Identifier: GPL-2.0-only
#include "QcomGPUNoC.h"
#include "AdrenoInterconnect.h"
#include "QcomGPU.h"
#include <ClockControllerProtocol.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/libkern.h>
namespace {
QcomPower::Status result(IOReturn s) {
    using namespace QcomPower;
    if (s == kIOReturnSuccess)
        return OK;
    if (s == kIOReturnBadArgument)
        return Invalid;
    if (s == kIOReturnBusy)
        return Busy;
    if (s == kIOReturnNotPermitted)
        return Denied;
    if (s == kIOReturnUnsupported)
        return Unsupported;
    if (s == kIOReturnTimeout)
        return TimedOut;
    if (s == kIOReturnNotReady || s == kIOReturnOffline || s == kIOReturnNotAttached)
        return NotReady;
    return Stale;
}
} // namespace
struct QcomGPUNoC::State {
    QcomGPU *owner = nullptr;
    IOService *provider = nullptr, *bus = nullptr;
    IOMemoryMap *gpu = nullptr, *bimc = nullptr;
    const OSSymbol *function = nullptr;
    IOLock *lock = nullptr;
    Adreno::NoCContract contract{};
    Adreno::GPUInterconnect<State> path{*this};
    bool fault = false;
    uint32_t read(uint32_t offset) {
        if (!owner->powerReady())
            return UINT32_MAX;
        auto value = *reinterpret_cast<volatile uint32_t *>(bimc->getVirtualAddress() + offset);
        __asm__ volatile("uxtw %x0,%w0\n\tdmb oshld" : "+r"(value) :: "memory");
        return value;
    }
    void write(uint32_t offset, uint32_t value) {
        __asm__ volatile("dmb oshst" ::: "memory");
        *reinterpret_cast<volatile uint32_t *>(bimc->getVirtualAddress() + offset) = value;
        __asm__ volatile("dsb oshst" ::: "memory");
    }
    QcomPower::Status request(ClockController::Operation op, uint32_t id, uint64_t &value, unsigned set = 0) {
        using namespace ClockController;
        // RPM GetRate/GetBandwidth validate their own live transport and
        // acknowledged votes; they do not access GPU MMIO. Keep the fresh
        // power check in read() before BIMC MMIO and in every mutating or
        // setup operation. Carry no cached power result across RPM calls.
        if (op != GetRate && op != GetBandwidth && !owner->powerReady())
            return QcomPower::NotReady;
        auto r = targeted(RPM, op, id, value, GPUConsumer, set);
        const auto status = bus->callPlatformFunction(function, false, &r.request, nullptr, nullptr, nullptr);
        if (!valid(r.request) || r.request.version != 2 || r.request.size != sizeof(r) ||
            r.controller != RPM || r.consumer != GPUConsumer || r.flags != set || r.reserved ||
            r.request.id != id || r.request.operation != op || r.request.name ||
            ((op == SetRate || op == SetBandwidth) && r.request.value != value))
            return QcomPower::Stale;
        value = r.request.value;
        return result(status);
    }
    QcomPower::Status limit(uint32_t id, bool clock, uint64_t &v) {
        return request(clock ? ClockController::GetMaximumRate : ClockController::GetMaximumBandwidth, id, v);
    }
    QcomPower::Status get(uint32_t id, bool clock, uint64_t &v) {
        return request(clock ? ClockController::GetRate : ClockController::GetBandwidth, id, v);
    }
    QcomPower::Status vote(uint32_t id, unsigned set, bool clock, uint64_t v) {
        return request(clock ? ClockController::SetRate : ClockController::SetBandwidth, id, v, set);
    }
    uint32_t gpuRead(uint32_t offset) {
        auto value = *reinterpret_cast<volatile uint32_t *>(gpu->getVirtualAddress() + offset);
        __asm__ volatile("uxtw %x0,%w0\n\tdmb oshld" : "+r"(value) :: "memory");
        return value;
    }
    QcomPower::Status isolated() {
        if (!owner->powerReady())
            return QcomPower::NotReady;
        return (gpuRead(Adreno::MEControl) & Adreno::Halt) == Adreno::Halt &&
                       (gpuRead(Adreno::VBIFHalted) & 15) == 15
                   ? QcomPower::OK
                   : QcomPower::Busy;
    }
};
QcomGPUNoC *QcomGPUNoC::create(QcomGPU *owner, IOService *provider, IOMemoryMap *gpu) {
    auto *data = OSDynamicCast(OSData, provider->getProperty("qcom,gpu-noc-handoff"));
    auto *memory = provider->getDeviceMemoryWithIndex(2);
    if (!data || data->getLength() != sizeof(Adreno::NoCContract) || !memory ||
        provider->getDeviceMemoryCount() != 3 || memory->getPhysicalAddress() != 0x01000000 ||
        memory->getLength() != 0x80000)
        return nullptr;
    auto *noc = new QcomGPUNoC;
    if (!noc)
        return nullptr;
    noc->fState = new State;
    if (!noc->fState) {
        delete noc;
        return nullptr;
    }
    auto &s = *noc->fState;
    s.owner = owner;
    s.provider = provider;
    provider->retain();
    s.bus = provider->getProvider();
    if (s.bus)
        s.bus->retain();
    s.gpu = gpu;
    gpu->retain();
    s.lock = IOLockAlloc();
    s.function = OSSymbol::withCStringNoCopy(ClockController::Function);
    memcpy(&s.contract, data->getBytesNoCopy(), sizeof(s.contract));
    if (s.contract.valid())
        s.bimc = provider->mapDeviceMemoryWithIndex(2, kIOMapInhibitCache);
    if (!s.bus || !s.lock || !s.function || !s.bimc || !s.bimc->getVirtualAddress() ||
        s.bimc->getLength() != 0x80000) {
        delete noc;
        return nullptr;
    }
    return noc;
}
QcomGPUNoC::~QcomGPUNoC() {
    if (!fState)
        return;
    auto &s = *fState;
    OSSafeReleaseNULL(s.bimc);
    OSSafeReleaseNULL(s.gpu);
    OSSafeReleaseNULL(s.provider);
    OSSafeReleaseNULL(s.bus);
    OSSafeReleaseNULL(s.function);
    if (s.lock)
        IOLockFree(s.lock);
    delete fState;
}
IOReturn QcomGPUNoC::acquire() {
    auto &s = *fState;
    IOLockLock(s.lock);
    const auto status = s.path.acquire(s.contract);
    IOLockUnlock(s.lock);
    if (status == QcomPower::OK)
        return kIOReturnSuccess;
    if (status == QcomPower::Invalid)
        return kIOReturnBadArgument;
    if (status == QcomPower::Unsupported)
        return kIOReturnUnsupported;
    return kIOReturnIOError;
}
bool QcomGPUNoC::healthy() {
    auto &s = *fState;
    IOLockLock(s.lock);
    const bool ok = !s.fault && s.path.check() == QcomPower::OK;
    if (!ok)
        s.fault = true;
    IOLockUnlock(s.lock);
    return ok;
}
bool QcomGPUNoC::shutdown() {
    auto &s = *fState;
    IOLockLock(s.lock);
    const bool ok = !s.fault && s.path.release() == QcomPower::OK && s.path.empty();
    IOLockUnlock(s.lock);
    return ok;
}
