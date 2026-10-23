// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBProtocol.h"
#include <ClockControllerProtocol.h>

namespace USBPHY {
struct Supply { uint32_t id, microvolts; };
// The production backend dispatches to real GCC/RPM IOService platform
// functions. This common code also exercises partial-acquire/release faults
// offline. It does not substitute a DT readiness flag for any hardware hold.
template<class Backend> class Resources {
    Backend &backend;
    bool configured = false, superSpeed = false, differential = false;
    bool uncertainClock = false, pipeHeld = false, resetAttempted = false, resetAsserted = false, handedOff = false;
    bool clampHeld = false;
    unsigned railCount = 0, clockCount = 0;
    struct Rail { Supply supply{}; bool held = false; } rails[4];
    struct Clock { uint32_t id = 0; bool held = false; } clocks[3];
    USB::Result request(ClockController::Controller controller, ClockController::Operation operation,
                        uint32_t id, uint64_t value = 0) {
        return backend.request(controller, operation, id, value);
    }
public:
    explicit Resources(Backend &operations) : backend(operations) {}
    bool configure(bool usb3, bool differentialRef, const Supply *supplies, unsigned count) {
        if (configured || !supplies || (usb3 ? count!=2 : (count!=3 && count!=4)) || (usb3 && differentialRef)) return false;
        for (unsigned i = 0; i < count; ++i) {
            if (!supplies[i].id || !supplies[i].microvolts || supplies[i].microvolts > 4000000) return false;
            for (unsigned j = 0; j < i; ++j) if (supplies[i].id == supplies[j].id) return false;
        }
        superSpeed = usb3; differential = differentialRef; railCount = count;
        for (unsigned i = 0; i < count; ++i) rails[i].supply = supplies[i];
        clocks[0].id = 121; clockCount = 1;
        if (usb3) { clocks[1].id = 119; clocks[2].id = 166; clockCount = 3; }
        else if (differential) { clocks[1].id = 170; clockCount = 2; }
        configured = true; return true;
    }
    bool empty() const {
        if (uncertainClock || pipeHeld || clampHeld || (resetAttempted && !resetAsserted)) return false;
        for (const auto &rail : rails) if (rail.held) return false;
        for (const auto &clock : clocks) if (clock.held) return false;
        return true;
    }
    bool unknownClockState() const { return uncertainClock; }
    bool powered() const {
        if (!configured || uncertainClock || (!superSpeed && !clampHeld)) return false;
        for (unsigned i = 0; i < railCount; ++i) if (!rails[i].held) return false;
        for (unsigned i = 0; i < clockCount; ++i) if (!clocks[i].held) return false;
        return true;
    }
    USB::Result reset(bool asserted) {
        if (!configured) return USB::Result::NotReady;
        resetAttempted = true; resetAsserted = false;
        const uint32_t ids[] = {superSpeed ? 70u : 106u, 69u};
        auto first = USB::Result::Success;
        for (unsigned i = 0; i < (superSpeed ? 2u : 1u); ++i) {
            const auto r = request(ClockController::GCC,
                asserted ? ClockController::AssertReset : ClockController::DeassertReset, ids[i]);
            if (r != USB::Result::Success && first == USB::Result::Success) first = r;
            if (r != USB::Result::Success && !asserted) break;
        }
        resetAsserted = asserted && first == USB::Result::Success;
        return first;
    }
    USB::Result pipe(bool enabled) {
        if (!configured || !superSpeed) return USB::Result::NotReady;
        if (enabled == pipeHeld) return USB::Result::Success;
        if (enabled) {
            uint64_t hz = 0;
            auto r = backend.request(ClockController::GCC, ClockController::GetRate, 120, hz);
            if (r != USB::Result::Success || hz != 125000000) return USB::Result::NotReady;
        }
        const auto r = request(ClockController::GCC,
            enabled ? ClockController::RetainClock : ClockController::ReleaseClock, 120);
        if (r == USB::Result::Success) pipeHeld = enabled;
        else if (r == USB::Result::Timeout || r == USB::Result::HardwareError) uncertainClock = true;
        return r;
    }
    USB::Result acquire() {
        if (!configured) return USB::Result::NotReady;
        if (!empty()) return USB::Result::Busy;
        const ClockController::Controller peers[] = {ClockController::GCC, ClockController::RPM};
        for (auto controller : peers) {
            uint64_t caps = 0;
            const auto r = backend.request(controller, ClockController::GetCapabilities, 0, caps);
            if (r != USB::Result::Success) return r;
            const uint64_t required = controller == ClockController::GCC ?
                ClockController::Clocks | ClockController::Resets | ClockController::ClockHandoff : ClockController::Voltage;
            if ((caps & required) != required) return USB::Result::Unsupported;
        }
        if (!handedOff) {
            uint64_t isolated=0;
            auto r=backend.request(ClockController::GCC,ClockController::GetResetState,18,isolated);
            if (r!=USB::Result::Success) return r;
            if (isolated!=1) return USB::Result::NotReady;
            r=reset(true); if (r!=USB::Result::Success) return r;
            const uint32_t leaves[]={superSpeed ? 119u : 170u,120u,166u};
            for (unsigned i=0;i<(superSpeed ? 3u:1u);++i) {
                r=request(ClockController::GCC,ClockController::AdoptClock,leaves[i]);
                if (r!=USB::Result::Success) {
                    if (r==USB::Result::Timeout || r==USB::Result::HardwareError) uncertainClock=true;
                    return r;
                }
            }
            handedOff=true;
        }
        if (!superSpeed) {
            // The digital clamp is a separate TCSR resource. Vendor cold
            // sequencing deasserts it before enabling the PHY's supplies.
            // Keep ownership even on an ambiguous write acknowledgement.
            clampHeld=true;
            const auto r=backend.clamp(true);if (r!=USB::Result::Success) return r;
        }
        for (unsigned i = 0; i < railCount; ++i) {
            auto &rail = rails[i]; uint64_t value = rail.supply.microvolts;
            const auto r = backend.request(ClockController::RPM, ClockController::SetVoltage, rail.supply.id, value);
            if (r != USB::Result::Success) {
                // A missing acknowledgement can still mean the vote reached
                // RPM. Confirm zero before releasing the owner on this path.
                if (r == USB::Result::Timeout || r == USB::Result::HardwareError) rail.held = true;
                return r;
            }
            rail.held = true;
            if (value < rail.supply.microvolts) return USB::Result::HardwareError;
        }
        if (superSpeed) {
            uint64_t hz = 0;
            auto r = backend.request(ClockController::GCC, ClockController::GetRate, 165, hz);
            if (r != USB::Result::Success || hz != 1200000) {
                hz = 1200000;
                r = backend.request(ClockController::GCC, ClockController::SetRate, 165, hz);
                if (r != USB::Result::Success) return r;
            }
        }
        for (unsigned i = 0; i < clockCount; ++i) {
            const auto r = request(ClockController::GCC, ClockController::RetainClock, clocks[i].id);
            if (r == USB::Result::Success) clocks[i].held = true;
            else {
                if (r == USB::Result::Timeout || r == USB::Result::HardwareError) uncertainClock = true;
                return r;
            }
        }
        return USB::Result::Success;
    }
    // Call only after Hardware::stop succeeded. A reset/clock failure must
    // retain the remaining holds and owner; retries resume at the failed row.
    USB::Result release() {
        if (!configured) return USB::Result::NotReady;
        if (empty()) return USB::Result::Success;
        const auto r = reset(true);
        if (r != USB::Result::Success) return r;
        if (uncertainClock || pipeHeld) return USB::Result::HardwareError;
        if (clampHeld) {
            const auto status=backend.clamp(false);if (status!=USB::Result::Success) return status;
            clampHeld=false;
        }
        for (unsigned i = clockCount; i; --i) if (clocks[i-1].held) {
            const auto status = request(ClockController::GCC, ClockController::ReleaseClock, clocks[i-1].id);
            if (status != USB::Result::Success) return status;
            clocks[i-1].held = false;
        }
        for (unsigned i = railCount; i; --i) if (rails[i-1].held) {
            const auto status = request(ClockController::RPM, ClockController::SetVoltage, rails[i-1].supply.id, 0);
            if (status != USB::Result::Success) return status;
            rails[i-1].held = false;
        }
        return USB::Result::Success;
    }
};
}
