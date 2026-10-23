// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBProtocol.h"

namespace XHCI {
namespace PortSC {
constexpr uint32_t Connected=1,Enabled=2,OverCurrent=8,Reset=16,LinkMask=15u<<5,Power=1u<<9;
constexpr uint32_t LinkWrite=1u<<16,ConnectChange=1u<<17,WarmResetChange=1u<<19;
constexpr uint32_t ResetChange=1u<<21,LinkChange=1u<<22,Changes=127u<<17,WarmReset=1u<<31;
constexpr uint32_t Persistent=LinkMask|Power|(3u<<14)|(7u<<25);
constexpr uint32_t Strobes=Enabled|Reset|LinkWrite|Changes|WarmReset;
}
struct PortProtocol {
    uint8_t major=0,minor=0,slotType=0;
    USB::Speed speeds[16]{};
};

// A Supported Protocol capability, not the physical port number, identifies
// USB2 versus USB3 and the speed IDs used in the slot context. Never assume
// that port1 is USB2 or that all controllers use the default speed IDs.
class PortProtocols {
    PortProtocol ports[128]{};
    unsigned count=0;
    bool ready=false;
    static USB::Speed speed(uint64_t rate,unsigned major) {
        if (major==2) {
            if (rate==1500000) return USB::Speed::Low;
            if (rate==12000000) return USB::Speed::Full;
            if (rate==480000000) return USB::Speed::High;
        } else if (rate==5000000000ULL) return USB::Speed::Super;
        return USB::Speed::Unknown;
    }
    template<class IO> static bool read(IO &io,unsigned offset,uint32_t &value) {
        if ((offset&3) || offset>=0xc100) return false;
        value=io.read(offset);return value!=UINT32_MAX;
    }
public:
    const PortProtocol *get(unsigned number) const {
        return ready && number && number<=count ? &ports[number] : nullptr;
    }
    template<class IO> USB::Result initialize(IO &io,unsigned number) {
        if (ready) return USB::Result::Busy;
        if (!number || number>127) return USB::Result::Invalid;
        for (auto &port:ports) port={};
        uint32_t hcc=0;
        if (!read(io,16,hcc)) return USB::Result::HardwareError;
        unsigned offset=(hcc>>16)*4,visited=0;
        if (!offset) return USB::Result::Unsupported;
        while (offset) {
            if (++visited>64 || offset<0x20) return USB::Result::Invalid;
            uint32_t header=0;
            if (!read(io,offset,header)) return USB::Result::HardwareError;
            const unsigned id=header&255,next=((header>>8)&255)*4;
            if (!id || (next && (next<4 || offset+next>=0xc100))) return USB::Result::Invalid;
            // This embedded path has no BIOS ownership handoff implementation.
            if (id==1) return USB::Result::Unsupported;
            if (id==2) {
                uint32_t name=0,range=0,slot=0;
                if (!read(io,offset+4,name) || !read(io,offset+8,range) || !read(io,offset+12,slot))
                    return USB::Result::HardwareError;
                const unsigned major=header>>24,minor=(header>>16)&255;
                const unsigned first=range&255,n=(range>>8)&255,psi=range>>28;
                const unsigned bytes=16+psi*4;
                if (name!=0x20425355 || (major!=2 && major!=3)) return USB::Result::Unsupported;
                if (!first || !n || first>number || n>number-first+1 || offset>0xc100-bytes || (next && next<bytes))
                    return USB::Result::Invalid;
                PortProtocol protocol{};protocol.major=major;protocol.minor=minor;protocol.slotType=slot&31;
                if (!psi) {
                    if (major==2) {
                        protocol.speeds[1]=USB::Speed::Full;protocol.speeds[2]=USB::Speed::Low;protocol.speeds[3]=USB::Speed::High;
                    } else protocol.speeds[4]=USB::Speed::Super;
                }
                for (unsigned i=0;i<psi;++i) {
                    uint32_t entry=0;if (!read(io,offset+16+4*i,entry)) return USB::Result::HardwareError;
                    const unsigned sid=entry&15,exponent=(entry>>4)&3;
                    uint64_t rate=entry>>16;for (unsigned k=0;k<exponent;++k) rate*=1000;
                    // SDM835 exposes symmetric USB2 or SuperSpeed Gen1 links.
                    // Wider/asymmetric capabilities need additional scheduling.
                    if (!sid || ((entry>>6)&3) || protocol.speeds[sid]!=USB::Speed::Unknown)
                        return USB::Result::Unsupported;
                    protocol.speeds[sid]=speed(rate,major);
                    if (protocol.speeds[sid]==USB::Speed::Unknown) return USB::Result::Unsupported;
                }
                for (unsigned p=first;p<first+n;++p) {
                    if (ports[p].major) return USB::Result::Invalid;
                    ports[p]=protocol;
                }
            }
            offset=next ? offset+next : 0;
        }
        for (unsigned p=1;p<=number;++p) if (!ports[p].major) return USB::Result::Unsupported;
        count=number;ready=true;return USB::Result::Success;
    }
};

// A DWC3 role lease covers one physical connector and one USB2/USB3 PHY
// pair. Supported Protocol capabilities identify the logical root ports;
// they do not authorize pairing an arbitrary multi-connector xHC by index.
class ConnectorPorts {
    unsigned usb2=0,usb3=0;
    uint8_t speeds=0;
    bool ready=false,super=false;
public:
    USB::Result initialize(const PortProtocols &protocols,unsigned count,unsigned maximumSpeed) {
        if (ready) return USB::Result::Busy;
        if (!count || count>2 || (maximumSpeed!=3 && maximumSpeed!=4)) return USB::Result::Unsupported;
        unsigned high=0,ss=0;uint8_t supported=0;
        for (unsigned n=1;n<=count;++n) {
            const auto *p=protocols.get(n);
            if (!p) return USB::Result::NotReady;
            if (p->major==2) { if (high) return USB::Result::Unsupported;high=n; }
            else if (p->major==3) { if (ss) return USB::Result::Unsupported;ss=n; }
            else return USB::Result::Unsupported;
            for (auto speed:p->speeds) switch (speed) {
            case USB::Speed::Low:supported|=1;break;
            case USB::Speed::Full:supported|=2;break;
            case USB::Speed::High:supported|=4;break;
            case USB::Speed::Super:supported|=8;break;
            default:break;
            }
        }
        if (!high || (maximumSpeed==4 && (!ss || !(supported&8)))) return USB::Result::Unsupported;
        usb2=high;usb3=ss;speeds=supported;super=maximumSpeed==4;ready=true;
        return USB::Result::Success;
    }
    bool routed(unsigned port) const {
        return ready && (port==usb2 || (super && port==usb3));
    }
    unsigned companion(unsigned source,unsigned nativeSpeed) const {
        // Native creation/query speeds: Low0, Full1, High2, Super3.
        // SuperPlus4/5 are outside this controller's advertised capability.
        if (!ready || !super || !usb3 || nativeSpeed>3 || !(speeds&(1u<<nativeSpeed))) return 0;
        if (source==usb2 && nativeSpeed==3) return usb3;
        if (source==usb3 && nativeSpeed<3) return usb2;
        return 0;
    }
};

enum class PortPhase : uint8_t { Idle,PowerOn,PowerOff,Reset,Recovery,Suspend,ResumeSignal,ResumeReady,ResumeRecovery,Fault };
// Timer-driven hardware operations. The adapter serializes these with its
// controller/role gate. No wait or sleep here; no operation releases DMA or
// asserts ownership of a PMIC source. Slot/transfer retirement belongs to HCD.
template<class Core> class Port {
    Core &core;
    PortProtocol protocol{};
    unsigned number=0;
    PortPhase phase=PortPhase::Idle;
    uint64_t last=0,deadline=0,readyAt=0;
    uint32_t completion=0;
    USB::Result fail(USB::Result r) { phase=PortPhase::Fault;return r; }
    USB::Result read(uint32_t &raw) {
        if (phase==PortPhase::Fault) return USB::Result::HardwareError;
        if (!number) return USB::Result::NotReady;
        const auto r=core.readPort(number,raw);
        return r==USB::Result::Success ? r : fail(r);
    }
    USB::Result write(uint32_t mask,uint32_t value,uint32_t strobes=0) {
        const auto r=core.writePort(number,mask,value,strobes);
        return r==USB::Result::Success ? r : fail(r);
    }
    USB::Result start(uint64_t now,uint64_t duration,uint32_t &raw,bool allowOverCurrent=false) {
        if (phase!=PortPhase::Idle) return phase==PortPhase::Fault ? USB::Result::HardwareError : USB::Result::Busy;
        if (now>UINT64_MAX-duration || now<last) return USB::Result::Invalid;
        auto r=read(raw);if (r!=USB::Result::Success) return r;
        if ((raw&PortSC::OverCurrent) && !allowOverCurrent) return USB::Result::HardwareError;
        last=now;deadline=now+duration;return USB::Result::Success;
    }
    USB::Result pending(PortPhase next,USB::Result r) {
        if (r!=USB::Result::Success) return r;
        phase=next;return USB::Result::Pending;
    }
public:
    explicit Port(Core &controller):core(controller) {}
    PortPhase currentPhase() const { return phase; }
    const PortProtocol &capabilities() const { return protocol; }
    USB::Result initialize(unsigned port,const PortProtocols &map) {
        if (number) return USB::Result::Busy;
        const auto *p=map.get(port);
        if (!p || port>core.maxPorts()) return USB::Result::Invalid;
        protocol=*p;number=port;return USB::Result::Success;
    }
    USB::Result status(uint32_t &value) {
        uint32_t raw=0;auto r=read(raw);if (r!=USB::Result::Success) return r;
        value=0;
        if (raw&PortSC::Connected) {
            const unsigned sid=(raw>>10)&15;
            if (!sid) return USB::Result::Pending; // link has not resolved its speed yet
            const auto speed=protocol.speeds[sid];
            if (speed==USB::Speed::Unknown) return USB::Result::Unsupported;
            // Native Host Family encodes Full=1, Low=2; USB::Speed uses
            // Low=1, Full=2. Never cast the software enum into this field.
            const unsigned nativeSpeed=speed==USB::Speed::Full ? 1u : speed==USB::Speed::Low ? 2u : unsigned(speed);
            value=nativeSpeed<<8;
        }
        if (raw&PortSC::Power) value|=0x800;
        if (raw&PortSC::Enabled) value|=0x1000;
        const unsigned link=(raw>>5)&15;
        if (link==3 || link==15) value|=0x2000;
        if (raw&PortSC::OverCurrent) value|=0x4000;
        if (raw&(PortSC::Reset|PortSC::WarmReset)) value|=0x8000;
        return USB::Result::Success;
    }
    USB::Result acknowledgeChanges(uint32_t &observed) {
        observed=0;
        uint32_t raw=0;auto r=read(raw);if (r!=USB::Result::Success) return r;
        observed=raw&PortSC::Changes;
        // Reset/recovery owns its newly asserted completion bit. Returning a
        // Port Status Change event must not erase that operation's evidence.
        const auto owned=(phase==PortPhase::Reset || phase==PortPhase::Recovery) ? completion : 0;
        const auto clear=observed&~owned;
        return clear ? write(0,0,clear) : USB::Result::Success;
    }
    USB::Result power(bool on,uint64_t now) {
        uint32_t raw=0;auto r=start(now,20000,raw,!on);if (r!=USB::Result::Success) return r;
        if (bool(raw&PortSC::Power)==on) return USB::Result::Success;
        if (!core.portPowerControlSupported()) return USB::Result::Unsupported;
        return pending(on ? PortPhase::PowerOn : PortPhase::PowerOff,
            write(PortSC::Power,on ? PortSC::Power : 0));
    }
    USB::Result reset(uint64_t now,bool warm=false) {
        if (warm && protocol.major!=3) return USB::Result::Unsupported;
        uint32_t raw=0;auto r=start(now,300000,raw);if (r!=USB::Result::Success) return r;
        if (!(raw&PortSC::Connected) || !(raw&PortSC::Power)) return USB::Result::NotReady;
        if (raw&(PortSC::Reset|PortSC::WarmReset)) return USB::Result::Busy;
        completion=warm ? PortSC::WarmResetChange : PortSC::ResetChange;
        // Retire an old completion before the new reset is issued. The
        // subsequent state machine requires a newly asserted completion bit.
        r=write(0,0,completion);if (r!=USB::Result::Success) return r;
        r=read(raw);if (r!=USB::Result::Success) return r;
        if (raw&completion) return fail(USB::Result::HardwareError);
        if (!(raw&PortSC::Connected)) return USB::Result::Aborted;
        if (raw&PortSC::OverCurrent) return USB::Result::HardwareError;
        if (!(raw&PortSC::Power)) return USB::Result::NotReady;
        if (raw&(PortSC::Reset|PortSC::WarmReset)) return USB::Result::Busy;
        return pending(PortPhase::Reset,write(0,0,warm ? PortSC::WarmReset : PortSC::Reset));
    }
    USB::Result suspend(uint64_t now) {
        uint32_t raw=0;auto r=start(now,10000,raw);if (r!=USB::Result::Success) return r;
        if ((raw&(PortSC::Connected|PortSC::Enabled|PortSC::Power))!=(PortSC::Connected|PortSC::Enabled|PortSC::Power) ||
            (raw&(PortSC::Reset|PortSC::WarmReset))) return USB::Result::NotReady;
        const unsigned link=(raw>>5)&15;
        if (link==3) return USB::Result::Success;
        if (link) return USB::Result::Busy;
        return pending(PortPhase::Suspend,write(PortSC::LinkMask,3u<<5,PortSC::LinkWrite));
    }
    USB::Result resume(uint64_t now) {
        uint32_t raw=0;auto r=start(now,40000,raw);if (r!=USB::Result::Success) return r;
        if ((raw&(PortSC::Connected|PortSC::Enabled|PortSC::Power))!=(PortSC::Connected|PortSC::Enabled|PortSC::Power) ||
            (raw&(PortSC::Reset|PortSC::WarmReset))) return USB::Result::NotReady;
        const unsigned link=(raw>>5)&15;
        if (!link) return USB::Result::Success;
        if (link!=3 && (protocol.major!=2 || link!=15)) return USB::Result::Busy;
        if (protocol.major==3) return pending(PortPhase::ResumeReady,write(PortSC::LinkMask,0,PortSC::LinkWrite));
        readyAt=now+20000;
        return pending(PortPhase::ResumeSignal,write(PortSC::LinkMask,15u<<5,PortSC::LinkWrite));
    }
    USB::Result advance(uint64_t now) {
        if (phase==PortPhase::Fault) return USB::Result::HardwareError;
        if (phase==PortPhase::Idle) return USB::Result::NotReady;
        if (now<last) return USB::Result::Invalid;
        last=now;
        if (now>=deadline) return fail(USB::Result::Timeout);
        uint32_t raw=0;auto r=read(raw);if (r!=USB::Result::Success) return r;
        if ((raw&PortSC::OverCurrent) && phase!=PortPhase::PowerOff) return fail(USB::Result::HardwareError);
        if (phase==PortPhase::PowerOn || phase==PortPhase::PowerOff) {
            if (bool(raw&PortSC::Power)==(phase==PortPhase::PowerOn)) { phase=PortPhase::Idle;return USB::Result::Success; }
        } else {
            if (!(raw&PortSC::Connected)) { phase=PortPhase::Idle;return USB::Result::Aborted; }
            if (!(raw&PortSC::Power)) return fail(USB::Result::HardwareError);
            const unsigned link=(raw>>5)&15;
            if (phase==PortPhase::Reset && !(raw&(PortSC::Reset|PortSC::WarmReset)) &&
                (raw&completion) && (raw&PortSC::Enabled) && !link) {
                if (now>=deadline-10000) return fail(USB::Result::Timeout);
                readyAt=now+10000;phase=PortPhase::Recovery;
            }
            if (phase==PortPhase::Recovery && now>=readyAt) {
                if (!(raw&PortSC::Enabled) || link || (raw&(PortSC::Reset|PortSC::WarmReset)))
                    return fail(USB::Result::HardwareError);
                r=write(0,0,completion);if (r!=USB::Result::Success) return r;
                phase=PortPhase::Idle;return USB::Result::Success;
            }
            if (phase==PortPhase::Suspend && link==3 && (raw&PortSC::Enabled)) {
                phase=PortPhase::Idle;return USB::Result::Success;
            }
            if (phase==PortPhase::ResumeSignal && now>=readyAt) {
                r=write(PortSC::LinkMask,0,PortSC::LinkWrite);if (r!=USB::Result::Success) return r;
                phase=PortPhase::ResumeReady;return USB::Result::Pending;
            }
            if (phase==PortPhase::ResumeReady && !link && (raw&PortSC::Enabled)) {
                if (protocol.major==3) { phase=PortPhase::Idle;return USB::Result::Success; }
                if (now>=deadline-10000) return fail(USB::Result::Timeout);
                readyAt=now+10000;phase=PortPhase::ResumeRecovery;
            }
            if (phase==PortPhase::ResumeRecovery && now>=readyAt) {
                if (link || !(raw&PortSC::Enabled)) return fail(USB::Result::HardwareError);
                phase=PortPhase::Idle;return USB::Result::Success;
            }
        }
        return USB::Result::Pending;
    }
};
}
