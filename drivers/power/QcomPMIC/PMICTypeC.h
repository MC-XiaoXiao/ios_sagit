// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "../../usb/QcomUSB/USBProtocol.h"

namespace PMICUSB {
// PMI8998 CC controller. The register/erratum sequence follows the pinned
// Qualcomm SMB2 sources, not a Linux Type-C or USB driver interface.
// Access supplies checked read/update, delay, nowMS and suppressCC operations.
template<class Access> class TypeC {
    Access &io;
    struct Setting { uint16_t address; uint8_t mask, value, saved; } settings[5] = {
        {0x1359, 0x0a, 0x02, 0}, // force UFP, disable Try.SRC
        {0x135a, 0x04, 0, 0},    // disable Try.SNK during CC2 test
        {0x1364, 0x80, 0x80, 0}, // software CC_OUT
        {0x1652, 0x20, 0x20, 0}, // 20 ms tCCDebounce
        {0x1368, 0x47, 0, 0},    // CC orientation; save role commands too
    };
    enum class Phase : uint8_t { Idle, Both, Sink, Fault } phase = Phase::Idle;
    uint64_t deadline = 0, lastTime = 0;
    bool initialized = false, dirty = false, revision2 = false, timeSeen = false;
    USB::Result restore() {
        if (!dirty) return USB::Result::Success;
        bool ok = true;
        // Restore the FSM command last, after the analog/debounce settings.
        // Retry every field after an ambiguous write, including unchanged ones.
        for (auto &s : settings) if (io.update(s.address, s.mask, s.saved) != USB::Result::Success) ok = false;
        if (!ok) { phase = Phase::Fault; return USB::Result::HardwareError; }
        dirty = false; phase = Phase::Idle; io.suppressCC(false);
        return USB::Result::Success;
    }
    USB::Result begin(uint64_t now) {
        for (auto &s : settings) {
            const auto r = io.read(s.address, s.saved);
            if (r != USB::Result::Success) return r;
        }
        dirty = true; io.suppressCC(true);
        for (auto &s : settings) {
            // Only orientation is changed in SW_CTRL at this stage. Role
            // commands are saved for exact restoration after the test.
            const uint8_t mask = s.address == 0x1368 ? 0x40 : s.mask;
            const auto r = io.update(s.address, mask, s.value);
            if (r != USB::Result::Success) { phase = Phase::Fault; return r; }
        }
        const auto r = io.update(0x1368, 6, 6);
        if (r != USB::Result::Success) { phase = Phase::Fault; return r; }
        phase = Phase::Both; deadline = now + 10;
        return USB::Result::NotReady;
    }
public:
    explicit TypeC(Access &access) : io(access) {}
    bool holds() const { return initialized || dirty; }
    bool recovering() const { return dirty; }
    bool failed() const { return phase == Phase::Fault; }
    bool ready() const { return initialized && phase == Phase::Idle; }
    USB::Result configure(uint32_t sourceMicroamps, bool vconn=false) {
        if (holds()) return USB::Result::Busy;
        uint8_t revision = 0;
        auto r = io.read(0x0103, revision);
        if (r != USB::Result::Success) return r;
        if (!revision || revision == 0xff) return USB::Result::Unsupported;
        revision2 = revision == 2;
        initialized = true; timeSeen = false;
        // Hold the CC FSM idle and prove software VCONN off before changing
        // policy. The connector owner selects the inactive CC only later.
        r = io.update(0x1368, 0xbf, 0x11);
        if (r == USB::Result::Success) io.delay(200);
        if (r == USB::Result::Success) r = io.update(0x1358, 0x22, 0);
        if (r == USB::Result::Success) r = io.update(0x1359, 0x0f, sourceMicroamps == 1500000 ? 1 : 0);
        if (r == USB::Result::Success) r = io.update(0x135a, 0x64, sourceMicroamps ? 4 : 0);
        if (r == USB::Result::Success && vconn) r = io.update(0x135a,0x30,0x30); // vendor VCONN soft-start
        if (r == USB::Result::Success) r = io.update(0x1364, 0x80, 0); // hardware CC_OUT
        if (r == USB::Result::Success) r = io.update(0x1367, 0xff, 4); // CC-only aggregate IRQ
        // Keep the PMIC PBS workaround enabled for DRP, disabled for forced
        // Sink. Full-byte DTEST4 values are specified by the vendor sequence.
        if (r == USB::Result::Success) r = io.update(0x16e9, 0xff, sourceMicroamps ? 0xa5 : 0);
        if (r == USB::Result::Success) r = io.update(0x1368, 0xbf,
            (sourceMicroamps ? 0x10 : 0x94) | (vconn ? 0x20 : 0));
        if (r != USB::Result::Success) phase = Phase::Fault;
        return r;
    }
    USB::Result process(const uint8_t status[5], uint8_t usbIRQ) {
        if (!initialized) return USB::Result::NotReady;
        if (failed()) return USB::Result::HardwareError;
        const auto now = io.nowMS();
        if ((timeSeen && now < lastTime) || now > UINT64_MAX - 30) {
            phase = Phase::Fault; return USB::Result::Overflow;
        }
        lastTime = now; timeSeen = true;
        if (dirty) {
            // A new supply cancels removal recovery. Return an invalid sample
            // even after restoration: the bytes above predate that change.
            if (usbIRQ & 0x10) {
                const auto r = restore();
                return r == USB::Result::Success ? USB::Result::NotReady : r;
            }
            if (now < deadline) return USB::Result::NotReady;
            if (phase == Phase::Both) {
                const auto r = io.update(0x1368, 6, 4);
                if (r != USB::Result::Success) { phase = Phase::Fault; return r; }
                phase = Phase::Sink; deadline = now + 30;
                return USB::Result::NotReady;
            }
            if (!(status[3] & 0x10) && !(status[4] & 0x10)) {
                const auto r = restore();
                return r == USB::Result::Success ? USB::Result::NotReady : r;
            }
            const auto r = io.update(0x1368, 6, 6);
            if (r != USB::Result::Success) { phase = Phase::Fault; return r; }
            phase = Phase::Both; deadline = now + 10;
            return USB::Result::NotReady;
        }
        const bool ordinarySink = status[0] == 0x80 || status[0] == 0x40 || status[0] == 0x20;
        if (revision2 && ordinarySink && (status[3] & 0x93) == 0x13 && !(usbIRQ & 0x10)) return begin(now);
        return USB::Result::Success;
    }
    USB::Result shutdown() {
        if (!holds()) return USB::Result::Success;
        auto r = restore();
        if (r != USB::Result::Success) return r;
        // Leave a sink-only, VCONN-off port. Never restore a firmware source
        // command after releasing our VBUS ownership.
        r = io.update(0x16e9, 0xff, 0);
        const auto role = io.update(0x1368, 0xbf, 0x94);
        if (r == USB::Result::Success) r = role;
        if (r == USB::Result::Success) { initialized = false; phase = Phase::Idle; }
        else phase = Phase::Fault;
        return r;
    }
};
}
