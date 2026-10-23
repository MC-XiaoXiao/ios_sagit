// SPDX-License-Identifier: GPL-2.0-only
#include "QcomI2C.h"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>
#include <ClockControllerProtocol.h>
#define super GenericI2CController
OSDefineMetaClassAndStructors(QcomI2C, super);
uint32_t QcomI2C::Registers::read32(uint32_t r) { auto v = *reinterpret_cast<volatile uint32_t *>(base+r); OSMemoryBarrier(); return v; }
void QcomI2C::Registers::write32(uint32_t r, uint32_t v) { OSMemoryBarrier(); *reinterpret_cast<volatile uint32_t *>(base+r) = v; OSMemoryBarrier(); }
void QcomI2C::Registers::delayUS(unsigned us) { IODelay(us); }
uint64_t QcomI2C::Registers::nowUS() { uint64_t t, ns; clock_get_uptime(&t); absolutetime_to_nanoseconds(t, &ns); return ns / 1000; }
bool QcomI2C::clock(unsigned operation,uint32_t id,uint64_t &value) {
    auto r=ClockController::targeted(ClockController::GCC,ClockController::Operation(operation),
                                   id,value,ClockController::I2CConsumer);
    if (!fClock || fClock->callPlatformFunction(ClockController::Function,false,&r.request,nullptr,nullptr,nullptr)!=kIOReturnSuccess)
        return false;
    value=r.request.value;return true;
}
void QcomI2C::releaseClocks() {
    const uint32_t clocks[]={37,46};
    for (unsigned i=2;i--;) if (fClockMask&(1U<<i)) {
        uint64_t value=0;
        if (clock(ClockController::ReleaseClock,clocks[i],value)) fClockMask&=~(1U<<i);
        else IOLog("QcomI2C: clock %u release failed; retaining safe vote\n",clocks[i]);
    }
}
bool QcomI2C::start(IOService *provider) {
    // Handoff v1: version, ownership/retention flags, core Hz, bus Hz.
    // Firmware must stop its I2C/DMA clients and retain pins, AHB/core clocks,
    // and touch/display supplies. This driver never resets the shared panel.
    auto *data = OSDynamicCast(OSData, provider->getProperty("qcom,i2c-handoff"));
    uint32_t h[4];
    if (!data || data->getLength()!=sizeof(h)) return false;
    memcpy(h, data->getBytesNoCopy(), sizeof(h));
    if (h[0]!=1 || h[1]!=7 || !super::start(provider)) return false;
    auto *match=IOService::serviceMatching("QcomGCC");
    if (!match) return false;
    fClock=IOService::waitForMatchingService(match,5000000000ULL);
    match->release();
    uint64_t rate=0;
    if (!fClock || !clock(ClockController::GetRate,46,rate) || rate!=h[2]) return false;
    // Hold shared BLSP1 AHB and QUP5 core through the existing GCC owner.
    // The root rate is observed only; no shared PLL/root reprogramming.
    const uint32_t clocks[]={37,46};
    for (unsigned i=0;i<2;++i) {
        uint64_t value=0;
        if (!clock(ClockController::RetainClock,clocks[i],value)) { releaseClocks();return false; }
        fClockMask|=1U<<i;
    }
    fMap = provider->mapDeviceMemoryWithIndex(0);
    if (!fMap || fMap->getPhysicalAddress()!=0xc179000 || fMap->getLength()<0x600) return false;
    fRegisters.base = reinterpret_cast<volatile uint8_t *>(fMap->getVirtualAddress());
    if (!fEngine.configure(h[2],h[3])) return false;
    setProperty("I2CBusFrequency",h[3],32);
    setProperty("I2CRetainedResources",true);
    return publishDevices(provider);
}
I2C::Result QcomI2C::execute(I2C::Transaction &t) { return fEngine.transfer(t); }
void QcomI2C::stop(IOService *provider) { super::stop(provider);releaseClocks(); }
void QcomI2C::free() { releaseClocks();OSSafeReleaseNULL(fMap);OSSafeReleaseNULL(fClock);super::free(); }
