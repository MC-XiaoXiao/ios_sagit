// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "../../usb/QcomUSB/USBRole.h"
#include "PMICTypeC.h"

// PMI8998 hardware only. Apple IOKit ownership, matching and notifications live
// in QcomUSBConnector. Register definitions/ordering are referenced to the
// pinned smb-reg.h, smb-lib.c and qcom-spmi-rradc.c in docs/usb/reference.
// Bus: Result read/write(uint16_t, uint8_t*, unsigned), delay(ms), nowMS(),
// edges(), faults(), suppressCC(bool). Caller serializes this object and owns
// USBIN/OTG/DCIN/RRADC and the two specified charger MISC register fields.
namespace PMICUSB {
using USB::Result;
struct Config {
    uint32_t sid = 0, adcBase = 0, sourceMicroamps = 0;
    uint32_t vconn = 0; // 1: board routes the PMI8998 SMB2 VCONN switch
    bool valid() const {
        return sid <= 15 && vconn<=1 && adcBase >= 0x4000 && adcBase <= 0xfe00 && !(adcBase & 0xff) &&
            (!sourceMicroamps || (sourceMicroamps >= 500000 && sourceMicroamps <= 1500000 &&
                                 !(sourceMicroamps % 250000)));
    }
};
namespace Reg {
constexpr uint16_t Type = 0x0104, Subtype = 0x0105;
constexpr uint16_t OTGStatus = 0x1109, OTGIRQ = 0x1110, OTGCommand = 0x1140;
constexpr uint16_t OTGLimit = 0x1152, OTGConfig = 0x1153, OTGEngineering = 0x11c0;
constexpr uint16_t TypeCStatus = 0x130b, USBIRQ = 0x1310, USBSuspend = 0x1340;
constexpr uint16_t TypeCConfig = 0x1358, TypeCInterrupts = 0x1367;
constexpr uint16_t SoftStart = 0x14c1;
constexpr uint8_t ADCControl = 0x52, ADCLog = 0x53, ADCTrigger = 0x91, ADCStatus = 0x92, ADCData = 0x94;
}

// Powered cables carry USB only with an explicit board VCONN path. Audio and
// debug accessory policy is independent and never presented as a USB route.
inline Result decode(const uint8_t bytes[5], uint8_t otg, uint8_t usb, USB::ConnectorState &s, bool poweredCables=false) {
    s = {};
    s.vbus = bytes[3] & 0x40;
    s.overCurrent = (otg & 7) || (usb & 8) || (bytes[3] & 0x24);
    if (!(bytes[3] & 1)) return Result::Success;
    if (!(bytes[3] & 0x10)) return Result::NotReady; // CC debounce not complete
    const bool host = bytes[3] & 0x80;
    if (host ? ((bytes[1]&15)!=8 && (!poweredCables || (bytes[1]&15)!=4)) :
        (bytes[0] != 0x80 && bytes[0] != 0x40 && bytes[0] != 0x20))
        return Result::Unsupported;
    s.attached = true; s.host = host;
    s.orientation = bytes[3] & 2 ? USB::Orientation::Reverse : USB::Orientation::Normal;
    s.power = host ? USB::PowerRole::Source : USB::PowerRole::Sink;
    return Result::Success;
}

template<class Bus> class Hardware {
    friend class TypeC<Hardware>;
    Bus &bus;
    TypeC<Hardware> typec{*this};
    Config config{};
    USB::ConnectorState previous{};
    uint64_t lastEdge = 0, lastFault = 0, sequence = 0;
    bool observed = false, disrupted = false, configured = false;
    bool maySource = false, savedSuspend = false;
    bool mayVCONN = false, poweredCable = false;
    uint8_t suspendValue = 0, halt = 0, adcControl = 0, adcTrigger = 0;
    bool adcDirty = false, savedHalt = false;
    bool faultLatched = false, uncertain = false, paused = false;
    void delay(unsigned ms) { bus.delay(ms); }
    uint64_t nowMS() { return bus.nowMS(); }
    void suppressCC(bool value) { bus.suppressCC(value); }
    Result read(uint16_t address, uint8_t &value) { return bus.read(address, &value, 1); }
    Result update(uint16_t address, uint8_t mask, uint8_t value) {
        uint8_t before = 0, after = 0;
        auto r = read(address, before); if (r != Result::Success) return r;
        const uint8_t next = (before & ~mask) | (value & mask);
        if (next != before) {
            if ((address & 0xff) >= 0xa0) {
                uint8_t key = 0xa5;
                r = bus.write((address & 0xff00) | 0xd0, &key, 1);
                if (r != Result::Success) return r;
            }
            r = bus.write(address, &next, 1); if (r != Result::Success) return r;
        }
        r = read(address, after);
        return r != Result::Success ? r : ((after & mask) == (value & mask) ? Result::Success : Result::HardwareError);
    }
    Result restoreADC() {
        if (!adcDirty) return Result::Success;
        // Attempt every restoration even if a preceding ACK was lost.
        const auto a = update(config.adcBase + Reg::ADCControl, 8, adcControl);
        const auto b = update(config.adcBase + Reg::ADCTrigger, 0x80, adcTrigger);
        const auto c = update(config.adcBase + Reg::ADCLog, 1, 0);
        if (a != Result::Success || b != Result::Success || c != Result::Success) return Result::HardwareError;
        adcDirty = false; return Result::Success;
    }
    Result voltage(uint32_t &millivolts) {
        if (adcDirty) return Result::Busy;
        uint8_t log = 0;
        auto r = read(config.adcBase + Reg::ADCControl, adcControl);
        if (r == Result::Success) r = read(config.adcBase + Reg::ADCTrigger, adcTrigger);
        if (r == Result::Success) r = read(config.adcBase + Reg::ADCLog, log);
        if (r != Result::Success) return r;
        if (log & 1) return Result::Busy;
        adcDirty = true;
        r = update(config.adcBase + Reg::ADCTrigger, 0x80, 0x80);
        if (r == Result::Success) r = update(config.adcBase + Reg::ADCLog, 1, 1);
        if (r == Result::Success) r = update(config.adcBase + Reg::ADCLog, 1, 0);
        if (r == Result::Success) r = update(config.adcBase + Reg::ADCControl, 8, 8);
        if (r == Result::Success) {
            r = Result::Timeout;
            // Keep pre-source measurement bounded; a slow ADC must fail the
            // attachment rather than delaying VBUS turn-on indefinitely.
            for (unsigned i = 0; i < 50; ++i) {
                uint8_t ready = 0;
                const auto status = read(config.adcBase + Reg::ADCStatus, ready);
                if (status != Result::Success) { r = status; break; }
                if (ready & 3) { r = Result::Success; break; }
                bus.delay(1);
            }
        }
        if (r == Result::Success) {
            r = Result::Busy;
            for (unsigned i = 0; i < 5; ++i) {
                uint8_t a[2]{}, b[2]{};
                auto status = bus.read(config.adcBase + Reg::ADCData, a, 2);
                if (status == Result::Success) status = bus.read(config.adcBase + Reg::ADCData, b, 2);
                if (status != Result::Success) { r = status; break; }
                if (a[0] != b[0] || a[1] != b[1]) continue;
                const unsigned raw = unsigned(a[0]) | unsigned(a[1]) << 8;
                if (raw >= 1024) { r = Result::HardwareError; break; }
                millivolts = (raw * 20000u + 512u) / 1024u;
                r = Result::Success; break;
            }
        }
        return restoreADC() == Result::Success ? r : Result::HardwareError;
    }
    Result current(const USB::ConnectorState &target, USB::ConnectorState &now) {
        auto r = sample(now);
        if (r == Result::Busy || r == Result::NotReady || r == Result::Unsupported) return Result::Aborted;
        return r != Result::Success ? r : (now.sameRoute(target) && now.desired() == target.desired() &&
            now.desired() != USB::Role::None ? Result::Success : Result::Aborted);
    }
public:
    explicit Hardware(Bus &b) : bus(b) {}
    bool holds() const { return configured || maySource || mayVCONN || savedSuspend || savedHalt || adcDirty || typec.holds(); }
    bool sourcing() const { return maySource; }
    bool vconnSourcing() const { return mayVCONN; }
    bool failed() const { return uncertain || adcDirty || typec.failed(); }
    bool sourceAuthorized() const { return configured && !paused && typec.ready() && config.sourceMicroamps && !faultLatched && !failed(); }
    Result configure(const Config &c) {
        if (holds()) return Result::Busy;
        if (!c.valid()) return Result::Invalid;
        config = c;
        uint8_t type = 0, subtype = 0, mode = 0, command = 0, status = 0;
        auto r = read(Reg::Type, type);
        if (r == Result::Success) r = read(Reg::Subtype, subtype);
        if (r == Result::Success) r = read(Reg::TypeCConfig, mode);
        if (r == Result::Success) r = read(Reg::OTGCommand, command);
        if (r == Result::Success) r = read(Reg::OTGStatus, status);
        if (r != Result::Success) return r;
        if (type != 0x51 || subtype != 0x15 || (mode & 1)) return Result::Unsupported;
        if (status & 7) return Result::Busy; // Includes autonomous firmware sources.
        configured = true;
        if (command & 1) {
            // The exclusive PMIC owner can inherit an armed firmware request
            // while the boost state machine is off. Disarm only this dormant
            // request, preserving all other command bits. Keep ownership on
            // an ambiguous write or a newly active source until stop proves
            // source-off; never mistake a live firmware source for idle.
            r = update(Reg::OTGCommand, 1, 0);
            if (r == Result::Success) r = read(Reg::OTGStatus, status);
            if (r != Result::Success || (status & 7)) {
                uncertain = true;
                return r != Result::Success ? r : Result::HardwareError;
            }
        }
        // The hardware's VBUS<1V interlock remains enabled (QUICKSTART=0).
        // Software owns VBUS; concurrent/debug sourcing stays disabled.
        r = update(Reg::OTGConfig, 0x37, 0x20);
        if (r == Result::Success) r = typec.configure(config.sourceMicroamps,config.vconn);
        poweredCable=false;
        observed = false; disrupted = true; previous = {}; lastEdge = bus.edges();
        return r;
    }
    Result sample(USB::ConnectorState &out) {
        if (!configured || paused) return Result::NotReady;
        if (failed()) return Result::HardwareError;
        const auto edge = bus.edges();
        const auto fault = bus.faults();
        uint8_t bytes[5]{}, otg = 0, usb = 0;
        auto r = bus.read(Reg::TypeCStatus, bytes, 5);
        if (r == Result::Success) r = read(Reg::OTGIRQ, otg);
        if (r == Result::Success) r = read(Reg::USBIRQ, usb);
        if (fault != lastFault || (r == Result::Success && ((otg & 7) || (usb & 8) || (bytes[3] & 0x24))))
            faultLatched = true;
        USB::ConnectorState now{};
        if (r == Result::Success) r = typec.process(bytes, usb);
        if (r == Result::Success) r = decode(bytes, otg, usb, now,config.vconn);
        const bool powered=now.attached && now.host && (bytes[1]&15)==4;
        if (r == Result::Success && (bus.edges() != edge || bus.faults() != fault)) r = Result::Busy;
        if (r != Result::Success || edge == UINT64_MAX || edge < lastEdge || fault == UINT64_MAX || fault < lastFault) {
            disrupted = true; out = {};
            // A temporarily unreadable route cannot leave our source enabled.
            if ((maySource || mayVCONN) && stop() != Result::Success) return Result::HardwareError;
            return r != Result::Success ? r : Result::Overflow;
        }
        if (now.power == USB::PowerRole::Sink) now.vbus = now.vbus && (usb & 0x10);
        if (!now.attached && !maySource && !mayVCONN && !now.overCurrent) faultLatched = false;
        now.overCurrent = now.overCurrent || faultLatched;
        lastFault = fault;
        // sameRoute includes connection; compare the newly decoded identity
        // at the previous sequence before deciding whether it changed.
        now.connection = previous.connection;
        const bool route = !observed || disrupted || powered!=poweredCable || !now.sameRoute(previous) ||
            (previous.power == USB::PowerRole::Sink && previous.vbus && !now.vbus);
        uint64_t advance = edge - lastEdge;
        if (!advance && route) advance = 1;
        if (UINT64_MAX - sequence < advance) {
            disrupted = true; out = {};
            return (maySource || mayVCONN) && stop() != Result::Success ? Result::HardwareError : Result::Overflow;
        }
        if ((maySource || mayVCONN) && (advance || now.desired() == USB::Role::None || now.power != USB::PowerRole::Source)) {
            if (stop() != Result::Success) { disrupted = true; out = {}; return Result::HardwareError; }
            // The bytes predate source-off; the next sample proves VBUS again.
            now.vbus = false;
        }
        sequence += advance; lastEdge = edge;
        now.connection = sequence; previous = out = now; observed = true; disrupted = false;poweredCable=powered;
        return Result::Success;
    }
    Result stop() {
        if (paused) return Result::Success; // suspend already proved source-off and cleaned ADC
        if (!holds()) return Result::Success;
        // No VBUS discharge command: an external source may still be attached.
        const auto vconn = update(0x1368,0x18,0x10);
        if (vconn==Result::Success) mayVCONN=false;
        auto r = update(Reg::OTGConfig, 2, 0);
        const auto command = update(Reg::OTGCommand, 1, 0);
        if (r == Result::Success) r = command;
        bool off = false;
        if (r == Result::Success) for (unsigned i = 0; i < 100; ++i) {
            uint8_t status = 0;
            r = read(Reg::OTGStatus, status); if (r != Result::Success) break;
            if (!(status & 7)) { off = true; break; }
            bus.delay(1);
        }
        if (r == Result::Success && !off) r = Result::Timeout;
        if (off) {
            maySource = false;
            if (savedHalt) {
                const auto restored = update(Reg::OTGEngineering, 1, halt);
                if (restored == Result::Success) savedHalt = false;
                if (r == Result::Success) r = restored;
            }
            if (savedSuspend) {
                const auto restored = update(Reg::USBSuspend, 1, suspendValue);
                if (restored == Result::Success) savedSuspend = false;
                if (r == Result::Success) r = restored;
            }
        }
        const auto adc = restoreADC();
        if (r == Result::Success) r = adc;
        if (r == Result::Success) r = vconn;
        uncertain = r != Result::Success;
        return r;
    }
    Result release() {
        auto r = stop();
        if (r == Result::Success) r = typec.shutdown();
        if (r == Result::Success) { configured = false; paused = false; }
        return r;
    }
    Result suspend() {
        if (!configured) return holds() ? Result::HardwareError : Result::Success;
        auto r = stop();
        if (r == Result::Success) r = typec.shutdown();
        if (r == Result::Success) paused = true;
        disrupted = true; return r;
    }
    Result resume() {
        if (!configured || !paused) return failed() ? Result::HardwareError : Result::Success;
        auto r = typec.configure(config.sourceMicroamps,config.vconn);
        if (r == Result::Success) paused = false;
        disrupted = true; return r;
    }
    Result safePower(USB::Role next, const USB::ConnectorState &target) {
        if (next == USB::Role::None) return stop();
        if (target.desired() != next) return Result::Invalid;
        USB::ConnectorState now{};
        auto r = current(target, now); if (r != Result::Success) return r;
        if (now.power == USB::PowerRole::Sink && maySource) return Result::Busy;
        return Result::Success;
    }
    Result source(const USB::ConnectorState &target) {
        if (!configured || paused) return Result::NotReady;
        if (!config.sourceMicroamps) return Result::Unsupported;
        if (target.power != USB::PowerRole::Source || !target.host) return Result::Invalid;
        USB::ConnectorState now{};
        auto r = current(target, now); if (r != Result::Success) return r;
        if (maySource) {
            uint8_t status = 0;
            r = read(Reg::OTGStatus, status);
            if (r!=Result::Success || !now.vbus || (status&15)!=10) {
                const auto reason=r!=Result::Success ? r : Result::NotReady;
                return stop()==Result::Success ? reason : Result::HardwareError;
            }
            if (poweredCable) {
                uint8_t control=0;
                r=read(0x1368,control);
                const uint8_t expected=0x18 | (now.orientation==USB::Orientation::Normal ? 0x40 : 0);
                if (r!=Result::Success || !mayVCONN || (control&0x58)!=expected) {
                    stop();return Result::HardwareError;
                }
            }
            return Result::Success;
        }
        if (savedHalt || savedSuspend || adcDirty) return Result::Busy;
        uint32_t mv = 0;
        r = voltage(mv); if (r != Result::Success) return r;
        if (mv >= 800) return Result::Busy; // vSafe0V, never infer from a GPIO.
        r = current(target, now); if (r != Result::Success) return r;
        if (now.vbus) return Result::Aborted;
        r = read(Reg::USBSuspend, suspendValue); if (r != Result::Success) return r;
        savedSuspend = true;
        r = update(Reg::USBSuspend, 1, 1);
        if (r == Result::Success) r = update(Reg::SoftStart, 7, 3);
        if (r == Result::Success) r = update(Reg::OTGLimit, 7, uint8_t(config.sourceMicroamps / 250000 - 1));
        if (r == Result::Success) {
            r = read(Reg::OTGEngineering, halt);
            if (r == Result::Success) { savedHalt = true; r = update(Reg::OTGEngineering, 1, 1); }
        }
        if (r == Result::Success) r = current(target, now);
        if (r == Result::Success) {
            maySource = true; // A lost write ACK is not proof of an off source.
            r = update(Reg::OTGCommand, 1, 1);
        }
        if (r == Result::Success) {
            r = Result::Timeout;
            for (unsigned i = 0; i < 100; ++i) {
                uint8_t status = 0, fault = 0;
                auto statusResult = read(Reg::OTGStatus, status);
                if (statusResult == Result::Success) statusResult = read(Reg::OTGIRQ, fault);
                if (statusResult != Result::Success) { r = statusResult; break; }
                if (fault & 7) { faultLatched = true; r = Result::Aborted; break; }
                if ((status & 15) == 10) { r = Result::Success; break; }
                bus.delay(1);
            }
        }
        if (r == Result::Success) r = current(target, now);
        if (r == Result::Success) {
            r = voltage(mv);
            if (r == Result::Success && (mv < 4750 || mv > 5250 || !now.vbus)) r = Result::NotReady;
            if (r == Result::Success) r = current(target, now);
            if (r == Result::Success && !now.vbus) r = Result::NotReady;
        }
        if (r==Result::Success && poweredCable) {
            // SMB2 supplies VCONN on the inactive CC: Normal uses CC2,
            // Reverse uses CC1. Treat a lost ACK as potentially energized.
            mayVCONN=true;
            r=update(0x1368,0x58,0x18 | (now.orientation==USB::Orientation::Normal ? 0x40 : 0));
            if (r==Result::Success) r=current(target,now); // fresh OC/route after energizing
        }
        if (r != Result::Success && stop() != Result::Success) return Result::HardwareError;
        return r;
    }
};
}
