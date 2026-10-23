// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBProtocol.h"
#include "USBPhyTables.h"

namespace USBPHY {
using Orientation = USB::Orientation;
enum class State : uint8_t { Off, Starting, Ready, Fault };
// Board analog values are data, selected by data role, never by class name.
// Bound the table to QUSB2 tuning/PLL registers; it cannot address interrupts,
// unrelated peripherals or the controller. Host sequences own power-down/up.
struct USB2Tuning {
    struct Entry { uint32_t offset,value; } settings[16]{};
    unsigned count=0;
    bool valid(bool host) const {
        if (count>16) return false;
        for (unsigned i=0;i<count;++i) {
            const auto s=settings[i];
            if (s.value>255) return false;
            switch (s.offset) {
            case 0:case 4:case 0x2c:case 0xb4:case 0x184:case 0x18c:
            case 0x218:case 0x21c:case 0x23c:case 0x240:break;
            case 0x210:
                if (!host || (i && i!=count-1)) return false;
                break;
            default:return false;
            }
        }
        return !host || !count || (count>=3 && settings[0].offset==0x210 &&
            (settings[0].value&1) && settings[count-1].offset==0x210 && !(settings[count-1].value&1));
    }
};
// Resources owns real rails and GCC holds. reset() and pipe() must finish
// their hardware operation before returning. These classes run on a sleepable
// resource worker, with controller DMA already stopped, never in an IRQ action.
template<class IO, class Resources> class USB2 {
    IO &io;
    Resources &resources;
    State state = State::Off;
    USB2Tuning deviceTuning{},hostTuning{};
    bool resetReleased = false;
    USB::Result update(uint32_t offset, uint32_t mask, uint32_t bits) {
        const auto before = io.read(offset);
        if (before == UINT32_MAX) return USB::Result::HardwareError;
        io.write(offset, (before & ~mask) | (bits & mask));
        const auto after = io.read(offset);
        return after != UINT32_MAX && (after & mask) == (bits & mask) ?
            USB::Result::Success : USB::Result::HardwareError;
    }
    USB::Result failed(USB::Result reason) {
        // Do not allow rails/clocks to be released on an unproven reset.
        const auto stopped = stop();
        return stopped == USB::Result::Success ? reason : stopped;
    }
public:
    static constexpr uint32_t Bytes = 0x2a8, Power = 0x210, Status = 0x1a0, Tune1 = 0x23c;
    explicit USB2(IO &registers, Resources &holds) : io(registers), resources(holds) {}
    State currentState() const { return state; }
    bool configure(const USB2Tuning &device, const USB2Tuning &host) {
        if (state!=State::Off || !device.valid(false) || !host.valid(true)) return false;
        deviceTuning=device;hostTuning=host;return true;
    }
    USB::Result start(unsigned trim, USB::Role role=USB::Role::Device) {
        if (state != State::Off) return USB::Result::Busy;
        if (trim > 15 || (role!=USB::Role::Device && role!=USB::Role::Host)) return USB::Result::Invalid;
        if (!resources.powered()) return USB::Result::NotReady;
        state = State::Starting;
        auto result = resources.reset(true);
        if (result != USB::Result::Success) { state = State::Fault; return result; }
        io.delay(100);
        result = resources.reset(false);
        if (result != USB::Result::Success) return failed(result);
        resetReleased = true;
        if (role==USB::Role::Host && hostTuning.count) {
            // Vendor Host tuning is a complete sequence. In particular its
            // HSTX value must not be replaced with the Device OTP override.
            for (unsigned i=0;i<hostTuning.count;++i) io.write(hostTuning.settings[i].offset,hostTuning.settings[i].value);
        } else {
            if (update(Power, 1, 1) != USB::Result::Success) return failed(USB::Result::HardwareError);
            if (deviceTuning.count) {
                for (unsigned i=0;i<deviceTuning.count;++i) io.write(deviceTuning.settings[i].offset,deviceTuning.settings[i].value);
            } else {
                constexpr Setting settings[] = {{0x004,0x13},{0x18c,0x7c},{0x02c,0x80},
                    {0x184,0x0a},{Tune1,0xa5},{0x240,0x09},{0x0b4,0x19}};
                for (auto setting : settings) io.write(setting.offset, setting.value);
            }
            // Zero is an unprogrammed OTP trim; retain the board's full byte.
            if (trim && update(Tune1, 0xf0, trim << 4) != USB::Result::Success)
                return failed(USB::Result::HardwareError);
            if (update(Power, 1, 0) != USB::Result::Success) return failed(USB::Result::HardwareError);
        }
        io.delay(250); // required 150us and subsequent 100us PLL settling
        for (unsigned elapsed = 0; elapsed <= 10000; elapsed += 10) {
            const auto status = io.read(Status);
            if (status == UINT32_MAX) return failed(USB::Result::HardwareError);
            if (status & 1) { state = State::Ready; return USB::Result::Success; }
            if (elapsed != 10000) io.delay(10);
        }
        return failed(USB::Result::Timeout);
    }
    bool ready() {
        if (state != State::Ready) return false;
        const auto status = io.read(Status);
        return status != UINT32_MAX && (status & 1);
    }
    USB::Result stop() {
        if (state == State::Off) return USB::Result::Success;
        // Assert the external reset even if MMIO has become inaccessible.
        if (resetReleased) update(Power, 1, 1);
        auto result = resources.reset(true);
        if (result != USB::Result::Success) { state = State::Fault; return result; }
        resetReleased = false; state = State::Off; return USB::Result::Success;
    }
};

template<class IO, class Resources> class USB3 {
    IO &io;
    Resources &resources;
    State state = State::Off;
    bool resetReleased = false, pipeHeld = false;
    USB::Result update(uint32_t offset, uint32_t mask, uint32_t bits) {
        const auto before = io.read(offset);
        if (before == UINT32_MAX) return USB::Result::HardwareError;
        io.write(offset, (before & ~mask) | (bits & mask));
        const auto after = io.read(offset);
        return after != UINT32_MAX && (after & mask) == (bits & mask) ?
            USB::Result::Success : USB::Result::HardwareError;
    }
    template<unsigned N> void configure(uint32_t base, const Setting (&settings)[N]) {
        for (auto setting : settings) io.write(base + setting.offset, setting.value);
    }
    USB::Result failed(USB::Result reason) {
        const auto stopped = stop();
        return stopped == USB::Result::Success ? reason : stopped;
    }
public:
    static constexpr uint32_t Bytes = 0x1000, PCSBase = 0xc00, Misc = 0xa00;
    static constexpr uint32_t Reset = PCSBase, Power = PCSBase + 4, Start = PCSBase + 8, Status = PCSBase + 0x174;
    explicit USB3(IO &registers, Resources &holds) : io(registers), resources(holds) {}
    State currentState() const { return state; }
    USB::Result start(Orientation orientation) {
        if (state != State::Off) return USB::Result::Busy;
        if (orientation != Orientation::Normal && orientation != Orientation::Reverse) return USB::Result::Invalid;
        if (!resources.powered()) return USB::Result::NotReady;
        state = State::Starting;
        auto result = resources.reset(true);
        if (result != USB::Result::Success) { state = State::Fault; return result; }
        result = resources.reset(false);
        if (result != USB::Result::Success) return failed(result);
        resetReleased = true;
        if (update(Power, 1, 1) != USB::Result::Success) return failed(USB::Result::HardwareError);
        const uint32_t select = orientation == Orientation::Reverse ? 3 : 2;
        io.write(Misc, select);
        if (io.read(Misc) != select) return failed(USB::Result::HardwareError);
        configure(0, Serdes);
        result = resources.pipe(true);
        if (result != USB::Result::Success) return failed(result);
        pipeHeld = true;
        // Both Type-C lanes need calibration even though only one carries
        // USB at a time. Orientation selects the lane; it is not a DP mode.
        configure(0x200, Transmit); configure(0x400, Receive);
        configure(0x600, Transmit); configure(0x800, Receive);
        configure(PCSBase, PCS);
        if (update(Reset, 1, 0) != USB::Result::Success || update(Start, 3, 3) != USB::Result::Success)
            return failed(USB::Result::HardwareError);
        for (unsigned elapsed = 0; elapsed <= 10000; elapsed += 200) {
            const auto status = io.read(Status);
            if (status == UINT32_MAX) return failed(USB::Result::HardwareError);
            if (!(status & 0x40)) { state = State::Ready; return USB::Result::Success; }
            if (elapsed != 10000) io.delay(200);
        }
        return failed(USB::Result::Timeout);
    }
    bool ready() {
        if (state != State::Ready) return false;
        const auto status = io.read(Status);
        return status != UINT32_MAX && !(status & 0x40);
    }
    USB::Result stop() {
        if (state == State::Off) return USB::Result::Success;
        // An external reset is the final stop proof. Keep every clock/rail
        // if it cannot be asserted; a failed RMW alone is not such a proof.
        if (resetReleased) {
            update(Reset, 1, 1); update(Start, 3, 0); update(Power, 1, 0);
        }
        auto result = resources.reset(true);
        if (result != USB::Result::Success) { state = State::Fault; return result; }
        resetReleased = false;
        if (pipeHeld) {
            result = resources.pipe(false);
            if (result != USB::Result::Success) { state = State::Fault; return result; }
            pipeHeld = false;
        }
        state = State::Off; return USB::Result::Success;
    }
};
}
