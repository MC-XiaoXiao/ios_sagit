// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "XHCIPorts.h"
#include "XHCICommands.h"

namespace XHCI {
enum class EndpointType : uint8_t { Control, Isochronous, Bulk, Interrupt };
// Device-creation speed values differ from both native PORT status fields
// and USB::Speed. The current Family's port dispatch table is [1,0,2,3,4,5].
inline USB::Speed speedFromFamily(unsigned value) {
    switch (value) {
    case 0:return USB::Speed::Low;
    case 1:return USB::Speed::Full;
    case 2:return USB::Speed::High;
    case 3:return USB::Speed::Super;
    default:return USB::Speed::Unknown;
    }
}
inline bool speedToFamily(USB::Speed speed,uint8_t &value) {
    switch (speed) {
    case USB::Speed::Low:value=0;return true;
    case USB::Speed::Full:value=1;return true;
    case USB::Speed::High:value=2;return true;
    case USB::Speed::Super:value=3;return true;
    default:return false;
    }
}
struct SlotConfiguration {
    uint32_t route=0,rootPort=0,speedID=0,entries=1;
    USB::Speed speed=USB::Speed::Unknown;
    uint32_t ports=0,ttHub=0,ttPort=0,ttThink=0,maxExitLatency=0;
    bool hub=false,multiTT=false;
};
struct FamilyLocation {
    unsigned bus=0,rootPort=0,depth=0;
    uint32_t route=0;
};
// Native locationID appends root/hub ports from the highest available nibble
// downwards. xHCI's Route String uses the opposite nibble order and excludes
// the root port. Preserve the controller's bus identity instead of treating a
// Native locationID as a slot ID or a ready-made xHCI Route String.
inline USB::Result decodeFamilyLocation(uint32_t location,uint32_t controllerLocation,
    unsigned maxPorts,FamilyLocation &out) {
    if ((controllerLocation&0xffffff) || (location>>24)!=(controllerLocation>>24) || !maxPorts || maxPorts>127)
        return USB::Result::Invalid;
    FamilyLocation value{location>>24,(location>>20)&15,0,0};
    if (!value.rootPort || value.rootPort>maxPorts) return USB::Result::Invalid;
    bool end=false;
    for (unsigned n=0;n<5;++n) {
        const auto port=(location>>(16-4*n))&15;
        if (!port) end=true;
        else {
            if (end) return USB::Result::Invalid;
            value.route|=port<<(4*n);++value.depth;
        }
    }
    out=value;return USB::Result::Success;
}
struct EndpointConfiguration {
    uint32_t address=0,maxPacket=0,interval=0,burst=0,attributes=0,bytesPerInterval=0;
    EndpointType type=EndpointType::Control;
    USB::Speed speed=USB::Speed::Unknown;
    uint64_t dequeue=0;
    bool cycle=true,companion=false,largeESIT=false;
};
inline unsigned endpointID(unsigned address,EndpointType type) {
    if (address&~0x8fu) return 0;
    const unsigned n=address&15;
    if (type==EndpointType::Control) return 2*n+1;
    return n ? 2*n+((address&128) ? 1 : 0) : 0;
}
inline unsigned exponent(unsigned value) {
    unsigned n=0;while (value>1) { value>>=1;++n; }return n;
}
// Pure formatter shared by input contexts and transfer construction. Invalid
// endpoint parameters leave the output untouched.
inline USB::Result encodeEndpoint(const EndpointConfiguration &c,unsigned bits,uint32_t (&out)[8]) {
    const unsigned id=endpointID(c.address,c.type),packet=c.maxPacket&2047,transactions=(c.maxPacket>>11)&3;
    const bool control=c.type==EndpointType::Control,iso=c.type==EndpointType::Isochronous;
    const bool bulk=c.type==EndpointType::Bulk,periodic=iso || c.type==EndpointType::Interrupt;
    if (bits<32 || bits>40 || !id || unsigned(c.type)>3 || c.speed==USB::Speed::Unknown || unsigned(c.speed)>4 ||
        !packet || (c.maxPacket&~8191u) || transactions==3 || c.interval>255 ||
        !c.dequeue || !USB::dmaRange(c.dequeue,16,bits,16) ||
        (transactions && (c.speed!=USB::Speed::High || !periodic))) return USB::Result::Invalid;
    if (c.speed==USB::Speed::Low && (iso || bulk || packet>8)) return USB::Result::Invalid;
    if (control) {
        if ((c.speed==USB::Speed::Low && packet!=8) ||
            (c.speed==USB::Speed::Full && packet!=8 && packet!=16 && packet!=32 && packet!=64) ||
            (c.speed==USB::Speed::High && packet!=64) || (c.speed==USB::Speed::Super && packet!=512)) return USB::Result::Invalid;
    } else if (bulk) {
        if ((c.speed==USB::Speed::Full && packet!=8 && packet!=16 && packet!=32 && packet!=64) ||
            (c.speed==USB::Speed::High && packet!=512) || (c.speed==USB::Speed::Super && packet!=1024)) return USB::Result::Invalid;
    } else if ((c.speed==USB::Speed::Full && packet>(iso ? 1023u : 64u)) || packet>1024)
        return USB::Result::Invalid;
    unsigned interval=0,burst=0,mult=0,payload=0;
    if (c.speed==USB::Speed::Super) {
        if (!control && !c.companion) return USB::Result::Invalid;
        if (c.burst>15 || (control && (c.burst || c.attributes || c.bytesPerInterval))) return USB::Result::Invalid;
        if (iso) {
            if ((c.attributes&~3u) || (c.attributes&3)==3) return USB::Result::Invalid;
            mult=c.attributes&3;
        } else if (bulk) {
            if ((c.attributes&~31u) || c.attributes>16 || c.bytesPerInterval) return USB::Result::Invalid;
            // Stream capability is not an instruction to enable streams.
        } else if (c.attributes) return USB::Result::Invalid;
        burst=c.burst;
        if (periodic) {
            if (!c.bytesPerInterval || c.bytesPerInterval>packet*(burst+1)*(mult+1)) return USB::Result::Invalid;
            payload=c.bytesPerInterval;
        }
        if (c.largeESIT) mult=0;
    } else {
        if (c.companion || c.burst || c.attributes || c.bytesPerInterval || c.largeESIT) return USB::Result::Invalid;
        if (transactions && (c.speed!=USB::Speed::High || !periodic)) return USB::Result::Invalid;
        burst=transactions;if (periodic) payload=packet*(burst+1);
    }
    if (periodic) {
        if (!c.interval) return USB::Result::Invalid;
        if (c.speed==USB::Speed::High || c.speed==USB::Speed::Super || iso) {
            if (c.interval>16) return USB::Result::Invalid;
            interval=c.interval-1+(c.speed==USB::Speed::Full ? 3 : 0);
        } else interval=exponent(c.interval*8);
    } else if (c.speed==USB::Speed::High && c.interval) interval=exponent(c.interval);
    const unsigned type=control ? 4 : iso ? 1 : bulk ? 2 : 3;
    const unsigned hwType=control ? type : type+((c.address&128) ? 4 : 0);
    const uint64_t dequeue=c.dequeue|uint64_t(c.cycle);
    const uint32_t encoded[8]={(mult<<8)|(interval<<16)|((payload>>16)<<24),
        (iso ? 0u : 3u<<1)|(hwType<<3)|(burst<<8)|(packet<<16),uint32_t(dequeue),uint32_t(dequeue>>32),
        (payload<<16)|(control ? 8u : payload),0,0,0};
    for (unsigned i=0;i<8;++i) out[i]=encoded[i];
    return USB::Result::Success;
}
// Views never allocate/free DMA. Native owners keep both contexts and every
// endpoint ring pinned through Disable Slot or a real controller reset. A
// command completion permits reusing its INPUT context, not freeing the slot.
class InputContext {
    volatile uint32_t *cpu=nullptr;
    unsigned stride=0,bits=0,slots=0,ports=0;
    uint64_t dma=0,ticket=0;
    volatile uint32_t *entry(unsigned index) { return cpu+index*(stride/4); }
    void store(unsigned index,const uint32_t words[8]) {
        auto *p=entry(index);for (unsigned i=0;i<stride/4;++i) p[i]=i<8 ? words[i] : 0;
    }
public:
    uint64_t address() const { return dma; }
    unsigned bytes() const { return 33*stride; }
    bool frozen() const { return ticket!=0; }
    uint64_t commandTicket() const { return ticket; }
    uint32_t dropMask() const { return cpu ? cpu[0] : 0; }
    uint32_t addMask() const { return cpu ? cpu[1] : 0; }
    USB::Result bind(void *memory,unsigned length,uint64_t deviceAddress,unsigned contextStride,
                     unsigned addressBits,unsigned maxSlots,unsigned maxPorts) {
        if (cpu) return USB::Result::Busy;
        if (!memory || !deviceAddress || (reinterpret_cast<uintptr_t>(memory)&63) || (contextStride!=32 && contextStride!=64) ||
            length<33*contextStride || addressBits<32 || addressBits>40 || !maxSlots || maxSlots>255 ||
            !maxPorts || maxPorts>127 || !USB::dmaRange(deviceAddress,33*contextStride,addressBits,64)) return USB::Result::Invalid;
        cpu=static_cast<volatile uint32_t *>(memory);stride=contextStride;bits=addressBits;slots=maxSlots;ports=maxPorts;dma=deviceAddress;
        return clear();
    }
    USB::Result clear() {
        if (!cpu) return USB::Result::NotReady;
        if (ticket) return USB::Result::Busy;
        for (unsigned i=0;i<33*stride/4;++i) cpu[i]=0;
        return USB::Result::Success;
    }
    USB::Result slot(const SlotConfiguration &c,const PortProtocol &protocol) {
        if (!cpu) return USB::Result::NotReady;
        if (ticket) return USB::Result::Busy;
        if (c.route>0xfffff || !c.rootPort || c.rootPort>ports || !c.speedID || c.speedID>15 ||
            (protocol.major!=2 && protocol.major!=3) || c.speed==USB::Speed::Unknown || unsigned(c.speed)>4 ||
            protocol.speeds[c.speedID]!=c.speed || !c.entries || c.entries>31 ||
            c.ports>255 || c.ttHub>slots || c.ttPort>255 || c.ttThink>3 || c.maxExitLatency>65535 ||
            (c.hub ? (!c.ports || c.speed==USB::Speed::Low) : c.ports) ||
            (bool(c.ttHub)!=bool(c.ttPort)) || (c.ttHub && (!c.route || (c.speed!=USB::Speed::Low && c.speed!=USB::Speed::Full))) ||
            (c.ttThink && (!c.hub || c.speed!=USB::Speed::High)) ||
            (c.hub && c.speed!=USB::Speed::High && c.multiTT) ||
            (c.multiTT && !c.ttHub && !(c.hub && c.speed==USB::Speed::High))) return USB::Result::Invalid;
        bool end=false;
        for (unsigned n=0;n<5;++n) {
            const auto port=(c.route>>(4*n))&15;
            if (!port) end=true;else if (end) return USB::Result::Invalid;
        }
        const uint32_t words[8]={c.route|(c.speedID<<20)|(uint32_t(c.multiTT)<<25)|(uint32_t(c.hub)<<26)|(c.entries<<27),
            c.maxExitLatency|(c.rootPort<<16)|(c.ports<<24),c.ttHub|(c.ttPort<<8)|(c.ttThink<<16),0,0,0,0,0};
        store(1,words);cpu[1]|=1;return USB::Result::Success;
    }
    USB::Result endpoint(const EndpointConfiguration &c,bool replace=false) {
        if (!cpu) return USB::Result::NotReady;
        if (ticket) return USB::Result::Busy;
        const unsigned id=endpointID(c.address,c.type);
        if (replace && id==1) return USB::Result::Invalid;
        uint32_t words[8]{};
        const auto result=encodeEndpoint(c,bits,words);
        if (result!=USB::Result::Success) return result;
        store(id+1,words);cpu[1]|=1u<<id;
        if (replace) cpu[0]|=1u<<id;else cpu[0]&=~(1u<<id);
        return USB::Result::Success;
    }
    USB::Result drop(unsigned id) {
        if (!cpu) return USB::Result::NotReady;
        if (ticket) return USB::Result::Busy;
        if (id<2 || id>31) return USB::Result::Invalid;
        cpu[0]|=1u<<id;cpu[1]&=~(1u<<id);return USB::Result::Success;
    }
    USB::Result freeze(uint64_t command) {
        if (!cpu || !command) return USB::Result::Invalid;
        if (ticket) return USB::Result::Busy;
        ticket=command;return USB::Result::Success;
    }
    // Only after a verified matching command completion or reset isolation.
    USB::Result retire(uint64_t command) {
        if (!ticket || command!=ticket) return USB::Result::Invalid;
        ticket=0;return USB::Result::Success;
    }
    template<class Queue> USB::Result submit(Queue &queue,Type type,unsigned slot,bool option,
        uintptr_t owner,uint64_t now,uint64_t timeout,uint64_t &commandTicket) {
        commandTicket=0;
        if (!cpu) return USB::Result::NotReady;
        if (ticket) return USB::Result::Busy;
        if (type!=AddressDevice && type!=ConfigureEndpoint && type!=EvaluateContext) return USB::Result::Invalid;
        if ((type==AddressDevice && (cpu[0] || cpu[1]!=3 || (entry(1)[0]>>27)!=1)) ||
            (type==EvaluateContext && cpu[0])) return USB::Result::Invalid;
        Command command;command.type=type;command.parameter=dma;command.slot=slot;command.option=option;
        const auto result=queue.submit(command,owner,now,timeout,commandTicket);
        // Even a doorbell failure can leave a published input context. A
        // nonzero ticket keeps it frozen until completion/reset isolation.
        if (commandTicket && freeze(commandTicket)!=USB::Result::Success) return USB::Result::HardwareError;
        return result;
    }
    template<class Queue> USB::Result finish(Queue &queue,uintptr_t owner,uintptr_t &releasedOwner) {
        releasedOwner=0;CommandRecord record{};
        if (!ticket || !queue.copy(ticket,record) || record.owner!=owner || record.command.parameter!=dma)
            return USB::Result::Invalid;
        if (record.phase!=CommandPhase::Completed && record.phase!=CommandPhase::Isolated) return USB::Result::Busy;
        const auto command=ticket;
        const auto result=queue.acknowledge(command,owner,releasedOwner);
        if (result!=USB::Result::Success) return result;
        return retire(command); // returned owner is released outside the gate
    }
};

struct SlotSnapshot {
    unsigned address=0,state=0,rootPort=0,speedID=0,entries=0;
    uint32_t route=0,ports=0,ttHub=0,ttPort=0,ttThink=0;
    bool hub=false,multiTT=false;
};
inline bool sameSlotConfiguration(const SlotSnapshot &out,const SlotConfiguration &c) {
    return out.rootPort==c.rootPort && out.speedID==c.speedID && out.entries==c.entries && out.route==c.route &&
        out.ports==c.ports && out.ttHub==c.ttHub && out.ttPort==c.ttPort && out.ttThink==c.ttThink &&
        out.hub==c.hub && out.multiTT==c.multiTT;
}
struct EndpointSnapshot { unsigned state,maxPacket;uint64_t dequeue; };
class DeviceContext {
    const volatile uint32_t *cpu=nullptr;
    unsigned stride=0;
public:
    USB::Result bind(const void *memory,unsigned bytes,unsigned contextStride) {
        if (cpu) return USB::Result::Busy;
        if (!memory || (reinterpret_cast<uintptr_t>(memory)&63) || (contextStride!=32 && contextStride!=64) ||
            bytes<32*contextStride) return USB::Result::Invalid;
        cpu=static_cast<const volatile uint32_t *>(memory);stride=contextStride;return USB::Result::Success;
    }
    template<class Order> USB::Result slot(Order &order,SlotSnapshot &out) const {
        if (!cpu) return USB::Result::NotReady;
        order.dmaReadBarrier();const uint32_t state=cpu[3],info=cpu[0],info2=cpu[1],tt=cpu[2];
        out={state&255,state>>27,(info2>>16)&255,(info>>20)&15,info>>27,
            info&0xfffff,info2>>24,tt&255,(tt>>8)&255,(tt>>16)&3,bool(info&(1u<<26)),bool(info&(1u<<25))};
        return out.state<=3 && out.address<=127 ? USB::Result::Success : USB::Result::HardwareError;
    }
    template<class Order> USB::Result endpoint(Order &order,unsigned id,unsigned &state,uint64_t &dequeue) const {
        EndpointSnapshot value{};
        const auto result=endpoint(order,id,value);
        state=value.state;dequeue=value.dequeue;return result;
    }
    template<class Order> USB::Result endpoint(Order &order,unsigned id,EndpointSnapshot &out) const {
        if (!cpu) return USB::Result::NotReady;
        if (!id || id>31) return USB::Result::Invalid;
        order.dmaReadBarrier();const auto *p=cpu+id*(stride/4);
        out={p[0]&7,p[1]>>16,uint64_t(p[3])<<32|p[2]};
        // An output context state alone never proves transfer DMA stopped.
        return out.state<=4 ? USB::Result::Success : USB::Result::HardwareError;
    }
};
}
