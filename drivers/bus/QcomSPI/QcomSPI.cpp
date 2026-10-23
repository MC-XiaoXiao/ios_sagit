// SPDX-License-Identifier: GPL-2.0-only
#include "QcomSPI.h"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>
#define super GenericSPIController
OSDefineMetaClassAndStructors(QcomSPI, super);

uint32_t QcomSPI::Registers::read32(uint32_t reg) {
    auto value = *reinterpret_cast<volatile uint32_t *>(base + reg);
    OSMemoryBarrier(); return value;
}
void QcomSPI::Registers::write32(uint32_t reg, uint32_t value) {
    OSMemoryBarrier();
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    OSMemoryBarrier();
}
void QcomSPI::Registers::delayUS(unsigned us) { IODelay(us); }
uint64_t QcomSPI::Registers::nowUS() {
    uint64_t time, ns; clock_get_uptime(&time);
    absolutetime_to_nanoseconds(time, &ns); return ns / 1000;
}
bool QcomSPI::start(IOService *provider) {
    // v1, flags, actual retained core/wire Hz, reserved=0, MMIO base lo/hi,
    // MMIO span lo/hi. Flags attest AP access, exclusive QUP, quiesced
    // firmware/SLPI/DMA, retained clocks, retained pins/supplies, respectively.
    // This record must come from a real platform resource owner, not a default.
    uint32_t h[8];
    auto *data = OSDynamicCast(OSData, provider->getProperty("qcom,spi-handoff"));
    if (!data || data->getLength() != sizeof(h)) return false;
    memcpy(h, data->getBytesNoCopy(), sizeof(h));
    const uint64_t base = uint64_t(h[4]) | (uint64_t(h[5]) << 32);
    const uint64_t span = uint64_t(h[6]) | (uint64_t(h[7]) << 32);
    if (h[0] != 1 || h[1] != 31 || h[3] || !base || (base & 0xfff) ||
        span < 0x1000 || span > 0x10000 || base > UINT64_MAX - span ||
        h[2] < 100000 || h[2] > 1000000 || !super::start(provider)) return false;
    if (!provider->open(this)) return false;
    fOwner = provider;
    fMap = provider->mapDeviceMemoryWithIndex(0);
    if (!fMap || fMap->getPhysicalAddress() != base || fMap->getLength() != span) goto fail;
    fRegisters.base = reinterpret_cast<volatile uint8_t *>(fMap->getVirtualAddress());
    if (!fEngine.configure(h[2]) || !publishDevices(provider)) goto fail;
    setProperty("SPIRetainedResources", true);
    setProperty("SPIFrequency", h[2], 32);
    return true;
fail:
    OSSafeReleaseNULL(fMap); fRegisters.base = nullptr;
    fOwner->close(this); fOwner = nullptr;
    return false;
}
SPI::Result QcomSPI::execute(SPI::Transaction &t) { return fEngine.transfer(t); }
bool QcomSPI::accepts(const SPI::Configuration &c) const { return fEngine.accepts(c); }
void QcomSPI::stop(IOService *provider) {
    super::stop(provider);
    if (fOwner) { fOwner->close(this); fOwner = nullptr; }
}
void QcomSPI::free() {
    if (fOwner) { fOwner->close(this); fOwner = nullptr; }
    OSSafeReleaseNULL(fMap);
    super::free();
}
