// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBProtocol.h"
#include <ClockControllerProtocol.h>
#include <QcomPowerHardware.h>

namespace USB {
constexpr const char *ControllerBootProperty="qcom,usb-resource-handoff";
// A generated board policy is not a retained rail/PLL lease. Its capture
// path requires the pinned stop metadata, actual BCR before/after fresh AP
// baseline ACKs, and the scoped RPM operation before any GCC mutation.
// BCR does not gate the inherited master clock. Cover both its observed
// rate and the intended rate before RPM can lower an AP voltage baseline.
template<class Backend> Result prepareControllerBootResources(Backend &backend,uint32_t coreHz) {
    if(!QcomPower::usbCoreCXLevel(coreHz))return Result::Invalid;
    if(!backend.bootStopped())return Result::NotReady;
    uint64_t value=0;
    auto r=backend.clockRequest(ClockController::GCC,ClockController::GetResetState,18,value);
    if(r!=Result::Success)return r;
    if(value!=1)return value ? Result::HardwareError:Result::NotReady;
    value=0;r=backend.clockRequest(ClockController::GCC,ClockController::GetRate,163,value);
    if(r!=Result::Success)return r;
    if(!value)return Result::NotReady;
    const auto coveredHz=QcomPower::usbCoreCoveredClock(coreHz,value);
    if(!coveredHz)return Result::Unsupported;
    const auto inheritedHz=uint32_t(value);
    const auto required=QcomPower::usbCoreCXLevel(coveredHz);
    value=0;r=backend.clockRequest(ClockController::RPM,ClockController::GetCapabilities,0,value);
    if(r!=Result::Success)return r;
    const uint64_t caps=ClockController::ResourceHandoff|ClockController::Domains;
    if((value&caps)!=caps)return Result::Unsupported;
    value=0;r=backend.clockRequest(ClockController::RPM,ClockController::PrepareUSBResources,coveredHz,value);
    if(r!=Result::Success)return r;
    if(value)return Result::HardwareError;
    value=0;r=backend.clockRequest(ClockController::RPM,ClockController::GetDomainLevel,0x1000,value);
    if(r!=Result::Success)return r;
    if(value<required || value>512)return Result::HardwareError;
    value=0;r=backend.clockRequest(ClockController::GCC,ClockController::GetRate,163,value);
    if(r!=Result::Success)return r;
    if(value!=inheritedHz)return Result::HardwareError;
    value=0;r=backend.clockRequest(ClockController::GCC,ClockController::GetResetState,18,value);
    if(r!=Result::Success)return r;
    return value==1 ? Result::Success:value ? Result::HardwareError:Result::NotReady;
}
// Backend.request is a real GCC platform function. halt must inspect and stop
// the actual current DWC3 role, not reuse a cached role from a former nub.
// No controller reset or firmware-pin adoption occurs before halt succeeds.
template<class Backend> class ControllerResources {
    Backend &backend;
    struct Clock { uint32_t id; bool held; } clocks[5] = {{71,false},{116,false},{32,false},{118,false},{117,false}};
    bool domain = false, uncertain = false, reset = false, resetTried = false, quiesced = false;
    uint32_t measuredCore = 0;
    Result request(ClockController::Operation op, uint32_t id, uint64_t value=0) {
        return backend.request(op,id,value);
    }
    Result changeClock(Clock &clock, bool enable) {
        auto r=request(enable ? ClockController::RetainClock : ClockController::ReleaseClock,clock.id);
        if (r==Result::Success) clock.held=enable;
        else if (r==Result::HardwareError || r==Result::Timeout) uncertain=true;
        return r;
    }
    Result assertReset() {
        resetTried=true; reset=false;
        auto r=request(ClockController::AssertReset,18);
        reset=r==Result::Success; return r;
    }
public:
    explicit ControllerResources(Backend &operations) : backend(operations) {}
    bool empty() const {
        if (domain || uncertain || (resetTried && !reset)) return false;
        for (const auto &clock:clocks) if (clock.held) return false;
        return true;
    }
    bool failed() const { return uncertain; }
    bool inReset() const { return reset; }
    uint32_t coreHz() const { return measuredCore; }
    Result acquire(uint32_t targetHz) {
        if (!empty()) return Result::Busy;
        if (targetHz!=60000000 && targetHz!=120000000 && targetHz!=150000000) return Result::Invalid;
        reset=false; resetTried=false; quiesced=false; measuredCore=0;
        auto r=backend.prepareClocks(targetHz);if(r!=Result::Success)return r;
        uint64_t value=0;
        r=backend.request(ClockController::GetCapabilities,0,value);
        const uint64_t caps=ClockController::Clocks|ClockController::Domains|ClockController::Resets|ClockController::ClockHandoff;
        if (r!=Result::Success) return r;
        if ((value&caps)!=caps) return Result::Unsupported;
        r=request(ClockController::RetainDomain,2);
        if (r!=Result::Success) { if (r==Result::Timeout || r==Result::HardwareError) uncertain=true; return r; }
        domain=true;
        value=0; r=backend.request(ClockController::GetResetState,18,value);
        if (r!=Result::Success) return r;
        if (value>1) return Result::HardwareError;
        if (value) { reset=resetTried=quiesced=true; }
        else {
            for (auto &clock:clocks) {
                r=changeClock(clock,true); if (r!=Result::Success) return r;
            }
            // Inherited DMA must stop while its existing clocks and
            // translation remain intact. BCR must not hide an active engine.
            r=backend.halt(); if (r!=Result::Success) return r;
            quiesced=true;
        }
        for (auto &clock:clocks) {
            r=request(ClockController::AdoptClock,clock.id);
            if (r!=Result::Success) {
                if (r==Result::Timeout || r==Result::HardwareError) uncertain=true;
                return r;
            }
        }
        if (!reset) { r=assertReset(); if (r!=Result::Success) return r; }
        for (unsigned i=5;i;--i) if (clocks[i-1].held) {
            r=changeClock(clocks[i-1],false); if (r!=Result::Success) return r;
        }
        r=request(ClockController::SetRate,163,targetHz); if (r!=Result::Success) return r;
        r=request(ClockController::SetRate,164,19200000); if (r!=Result::Success) return r;
        // BCR masks the controller-local clock handshake on real MSM8998.
        // Retain the fabric path for SMMU access while DMA remains reset;
        // the independent PHY has its own clock and regulator leases.
        for (auto &clock:clocks) if (clock.id==71 || clock.id==32) {
            r=changeClock(clock,true); if (r!=Result::Success) return r;
        }
        value=0; r=backend.request(ClockController::GetRate,116,value);
        if (r!=Result::Success) return r;
        if (value<60000000 || value>150000000) return Result::NotReady;
        measuredCore=uint32_t(value);
        value=0; r=backend.request(ClockController::GetRate,117,value);
        if (r!=Result::Success) return r;
        return value==19200000 ? Result::Success : Result::NotReady;
    }
    Result activate() {
        if (!domain || uncertain || !reset || !measuredCore) return Result::NotReady;
        for (const auto &clock:clocks)
            if ((clock.id==71 || clock.id==32) && !clock.held) return Result::NotReady;
        reset=false;
        const auto r=request(ClockController::DeassertReset,18);
        // A failed deassert has unknown controller state; release must assert
        // reset again and prove it before any clock or PHY may disappear.
        if (r!=Result::Success) return r;
        // Reset has erased the old rings and RUN/STOP state. No new native
        // controller is published yet. Establish its local reference/sleep
        // clocks before waiting for the master branch's NOC handshake.
        const unsigned local[]={4,3,1}; // mock UTMI, sleep, master
        for (auto index:local) if (!clocks[index].held) {
            const auto status=changeClock(clocks[index],true);
            if (status!=Result::Success) return status;
        }
        quiesced=false; return Result::Success;
    }
    Result isolate() {
        if (empty()) return Result::Success;
        if (uncertain) return Result::HardwareError;
        if (reset) return Result::Success;
        if (!quiesced) {
            uint64_t value=0;
            const auto r=backend.request(ClockController::GetResetState,18,value);
            if (r!=Result::Success) return r;
            if (value==1) { reset=resetTried=quiesced=true; return Result::Success; }
            if (value) return Result::HardwareError;
            for (const auto &clock:clocks) if (!clock.held) return Result::NotReady;
            const auto halted=backend.halt(); if (halted!=Result::Success) return halted;
            quiesced=true;
        }
        return assertReset();
    }
    Result release() {
        if (empty()) return Result::Success;
        auto r=isolate(); if (r!=Result::Success) return r;
        for (unsigned i=5;i;--i) if (clocks[i-1].held) {
            r=changeClock(clocks[i-1],false); if (r!=Result::Success) return r;
        }
        if (domain) {
            r=request(ClockController::ReleaseDomain,2);
            if (r!=Result::Success) { if (r==Result::Timeout || r==Result::HardwareError) uncertain=true; return r; }
            domain=false;
        }
        measuredCore=0; return Result::Success;
    }
};
}
