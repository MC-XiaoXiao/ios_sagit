// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "XHCIContexts.h"
#include "XHCIPower.h"

namespace XHCI {
constexpr uint32_t EvaluateNext=1u<<1,EventDataFlag=1u<<2;
struct TransferSegment { uint64_t address;uint32_t length; };
struct IsoSchedule {
    unsigned frame=0;
    bool asap=true,extendedTBC=false;
};
inline bool sameEndpoint(const EndpointConfiguration &a,const EndpointConfiguration &b) {
    return a.address==b.address && a.maxPacket==b.maxPacket && a.interval==b.interval && a.burst==b.burst &&
        a.attributes==b.attributes && a.bytesPerInterval==b.bytesPerInterval && a.type==b.type && a.speed==b.speed &&
        a.dequeue==b.dequeue && a.cycle==b.cycle && a.companion==b.companion && a.largeESIT==b.largeESIT;
}

// Immutable after construction. Buffers are the HCD's prepared DMA buffers,
// never addresses from the CPU-only native client DMA command. Every data TD
// ends in one IOC Event Data TRB; ISP/IOC are clear in all payload TRBs. ED
// events therefore carry an actual byte count and a software cookie, including
// on short transfers, without retiring a TD at an intermediate ISP event.
template<unsigned MaxTRBs=64> class TransferDescriptor {
    static_assert(MaxTRBs>=5 && MaxTRBs<=256,"bounded transfer descriptor");
    TRB items[MaxTRBs]{};
    EndpointConfiguration endpoint{};
    unsigned used=0,addressBits=0;
    unsigned padding=0;
    uint32_t length=0;
    bool input=false,twoStages=false;
public:
    unsigned count() const { return used; }
    unsigned prefix() const { return padding; }
    uint32_t bytes() const { return length; }
    unsigned bits() const { return addressBits; }
    bool in() const { return input; }
    bool staged() const { return twoStages; }
    const TRB *data() const { return items; }
    const EndpointConfiguration &configuration() const { return endpoint; }
    USB::Result scheduleIsochronous(const EndpointConfiguration &configuration,unsigned frame,bool asap) {
        if (endpoint.type!=EndpointType::Isochronous || !used || padding>=used || items[padding].type()!=Iso ||
            frame>2047 || (asap && frame) || !USB::dmaRange(configuration.dequeue,16,addressBits,16))
            return USB::Result::Invalid;
        auto comparable=configuration;comparable.dequeue=endpoint.dequeue;comparable.cycle=endpoint.cycle;
        if (!sameEndpoint(comparable,endpoint)) return USB::Result::Invalid;
        // The caller owns an unpublished descriptor. Endpoint replacement
        // can move its dequeue/cycle, while payload and packet policy stay
        // fixed. No DMA allocation or client copy occurs here.
        endpoint=configuration;
        auto &item=items[padding];item.control&=~((2047u<<20)|(1u<<31));
        item.control|=asap ? 1u<<31 : frame<<20;return USB::Result::Success;
    }
    template<class Ring> USB::Result alignForRing(const Ring &ring) {
        if (!used || !ring.startAddress()) return USB::Result::Busy;
        const auto at=(ring.nextAddress()-ring.startAddress())/sizeof(TRB);
        if (at>=ring.dataEntries()) return USB::Result::Invalid;
        const unsigned original=used-padding,remaining=ring.dataEntries()-unsigned(at),prefix=original>remaining ? remaining : 0;
        if (prefix+original>MaxTRBs || prefix+original>ring.available()) return USB::Result::Busy;
        // An unpublished ISO TD can wait for queue capacity while another
        // request advances the producer. Recompute its padding for that new
        // position instead of permanently pinning it to an obsolete cursor.
        if (padding) for (unsigned i=0;i<original;++i) items[i]=items[i+padding];
        used=original;
        // Each no-op is a complete, non-IOC TD. Put the entire real request
        // after the Link rather than splitting a packet/burst at that Link.
        for (unsigned i=used;i>0;--i) items[i-1+prefix]=items[i-1];
        for (unsigned i=0;i<prefix;++i) items[i]=trb(0,0,TransferNoop<<10);
        used+=prefix;padding=prefix;return USB::Result::Success;
    }
    template<class Ring> USB::Result alignForEmptyRing(const Ring &ring) {
        return ring.pending() ? USB::Result::Busy : alignForRing(ring);
    }
    USB::Result build(const EndpointConfiguration &configuration,const TransferSegment *segments,unsigned segmentCount,
        uint32_t bytes,unsigned bits,const USB::SetupPacket *setup=nullptr,IsoSchedule schedule={},bool zeroPacket=false) {
        uint32_t words[8]{};
        auto result=encodeEndpoint(configuration,bits,words);
        if (result!=USB::Result::Success) return result;
        const bool control=configuration.type==EndpointType::Control,iso=configuration.type==EndpointType::Isochronous;
        const unsigned packet=configuration.maxPacket&2047;
        const auto controlPacket=setup ? *setup : USB::SetupPacket{};
        const bool receive=control ? controlPacket.in() : bool(configuration.address&128);
        if (bytes>0xffffff || segmentCount>MaxTRBs || bool(bytes)!=bool(segmentCount) || (segmentCount && !segments) ||
            bool(setup)!=control || (control && controlPacket.length!=bytes) ||
            (zeroPacket && (configuration.type!=EndpointType::Bulk || receive || !bytes || bytes%packet))) return USB::Result::Invalid;
        if (!iso && (schedule.frame || !schedule.asap || schedule.extendedTBC)) return USB::Result::Invalid;
        if (iso && (schedule.frame>2047 || (schedule.asap && schedule.frame))) return USB::Result::Invalid;
        if (schedule.extendedTBC && !configuration.largeESIT) return USB::Result::Invalid;
        if (iso && bytes>((words[0]>>24)<<16 | words[4]>>16)) return USB::Result::Invalid;
        // A segment array embedded in this descriptor would be overwritten
        // while being read during the second pass. Reject it before writing.
        const auto source=reinterpret_cast<uintptr_t>(segments),destination=reinterpret_cast<uintptr_t>(this);
        if (segmentCount && (source>UINTPTR_MAX-sizeof(TransferSegment)*segmentCount ||
            (source<destination+sizeof(*this) && source+sizeof(TransferSegment)*segmentCount>destination))) return USB::Result::Invalid;
        uint64_t sum=0;unsigned payloadTRBs=bytes ? 0 : control ? 0 : 1;
        for (unsigned i=0;i<segmentCount;++i) {
            const auto &segment=segments[i];
            if (!segment.address || !USB::dmaRange(segment.address,segment.length,bits) || sum+segment.length>bytes)
                return USB::Result::Invalid;
            sum+=segment.length;
            payloadTRBs+=unsigned(((segment.address&65535)+segment.length+65535)/65536);
        }
        if (sum!=bytes) return USB::Result::Invalid;
        const unsigned required=payloadTRBs+(control ? bytes ? 4 : 3 : zeroPacket ? 3 : 1);
        if (required>MaxTRBs) return USB::Result::Busy;
        unsigned packets=(bytes+packet-1)/packet;if (!packets) packets=1;
        const unsigned burstWidth=configuration.speed==USB::Speed::Super ? configuration.burst+1 : 1;
        const unsigned bursts=iso && configuration.speed==USB::Speed::Super ? (packets+burstWidth-1)/burstWidth-1 : 0;
        const unsigned lastBurst=configuration.speed==USB::Speed::Super ? (packets-1)%burstWidth : packets-1;
        if (iso && (bursts>(schedule.extendedTBC ? 31u : 3u) || lastBurst>15)) return USB::Result::Invalid;
        // Validation is complete. No failing operation follows this point.
        endpoint=configuration;addressBits=bits;length=bytes;input=receive;twoStages=(control && bytes) || zeroPacket;
        used=padding=0;
        if (control) {
            const uint64_t wire=uint64_t(controlPacket.requestType)|uint64_t(controlPacket.request)<<8|
                uint64_t(controlPacket.value)<<16|uint64_t(controlPacket.index)<<32|uint64_t(controlPacket.length)<<48;
            items[used++]=trb(wire,8,Setup<<10|Immediate|(bytes ? (receive ? 3u : 2u)<<16 : 0));
        }
        uint32_t sent=0;unsigned payload=0;
        for (unsigned i=0;i<segmentCount;++i) {
            auto address=segments[i].address;uint32_t left=segments[i].length;
            while (left) {
                const uint32_t boundary=65536-uint32_t(address&65535),chunk=left<boundary ? left : boundary;
                const bool first=payload==0,last=payload+1==payloadTRBs;
                const unsigned type=first && control ? Data : first && iso ? Iso : Normal;
                unsigned remaining=last ? 0 : (bytes+packet-1)/packet-(sent+chunk)/packet;
                if (remaining>31) remaining=31;
                uint32_t flags=type<<10|Chain;
                if (type==Data && receive) flags|=1u<<16;
                if (type==Iso) {
                    flags|=lastBurst<<16|(schedule.asap ? 1u<<31 : schedule.frame<<20);
                    if (schedule.extendedTBC) remaining=bursts;else flags|=bursts<<7;
                }
                if (last) flags|=EvaluateNext;
                items[used++]=trb(address,chunk|remaining<<17,flags);
                address+=chunk;left-=chunk;sent+=chunk;++payload;
            }
        }
        if (!control && !bytes) {
            uint32_t flags=(iso ? Iso : Normal)<<10|Chain|EvaluateNext;
            if (iso) flags|=schedule.asap ? 1u<<31 : schedule.frame<<20;
            items[used++]=trb(0,0,flags);
        }
        if (bytes || !control) items[used++]=trb(0,0,EventData<<10|IOC);
        if (control) {
            // SETUP, DATA and STATUS are separate TDs. A DATA short packet
            // cannot skip STATUS. Its ED cookie is different from final ED.
            const bool statusIn=!bytes || !receive;
            items[used++]=trb(0,0,Status<<10|Chain|EvaluateNext|(statusIn ? 1u<<16 : 0));
            items[used++]=trb(0,0,EventData<<10|IOC);
        } else if (zeroPacket) {
            items[used++]=trb(0,0,Normal<<10|Chain|EvaluateNext);
            items[used++]=trb(0,0,EventData<<10|IOC);
        }
        return USB::Result::Success;
    }
};

enum class TransferPhase : uint8_t { Empty,Pending,Completed,Quarantined,Isolated };
enum class EndpointActivity : uint8_t { Running,Pausing,Paused };
struct TransferRecord {
    uint64_t ticket=0,first=0,last=0,deadline=0;
    uint64_t isoFirst=0;
    uintptr_t owner=0;
    unsigned trbs=0,completionCode=0,isoTRBs=0;
    uint32_t requested=0,transferred=0;
    TransferPhase phase=TransferPhase::Empty;
    USB::Result result=USB::Result::Pending;
    bool input=false,staged=false,dataSeen=false,shortPacket=false,dataValid=false;
};

// One bounded queue per configured endpoint, all calls under the controller
// gate. Commands is the REAL controller CommandQueue, not copied command
// records. Recovery requires new successful Stop/Reset Endpoint and Set TR
// Dequeue Pointer commands issued after this queue closed admission. The HCD
// keeps the endpoint/ring alive independently; retain(owner) pins each request
// and its hardware buffers before the producer cycle makes them visible.
// acknowledge returns the owner for release/copy/native completion OUTSIDE
// the hardware gate. A nonzero ticket remains owned even after submit fails.
template<class Backend,class Commands,unsigned Capacity=64> class TransferQueue {
    static_assert(Capacity && Capacity<=256,"bounded transfer ownership");
    Backend &backend;Commands &commands;
    TransferRecord records[Capacity]{};
    unsigned order[Capacity]{},head=0,pending=0,slot=0,id=0,bits=0;
    uint64_t epoch=0,lastTime=0,recoveryFloor=0;
    uintptr_t endpointOwner=0;
    EndpointConfiguration endpoint{};
    bool active=false,fault=false;
    bool nativeHalt=false,nativeRecoveryProved=false;
    EndpointActivity activity=EndpointActivity::Running;
    uint64_t pauseFloor=0,pauseStop=0,stoppedAt=0;
    bool pauseProof=false;
    TransferRecord *find(uint64_t ticket) {
        for (auto &record:records) if (record.phase!=TransferPhase::Empty && record.ticket==ticket) return &record;
        return nullptr;
    }
    USB::Result poison(USB::Result reason) {
        if (!fault) recoveryFloor=commands.lastTicket();
        fault=true;
        for (auto &record:records) if (record.phase==TransferPhase::Pending) {
            record.phase=TransferPhase::Quarantined;record.result=reason;
        }
        return reason;
    }
    bool belongsRange(uint64_t start,unsigned count,uint64_t address) const {
        const auto base=backend.ring().startAddress();const auto entries=backend.ring().dataEntries();
        if (!entries || address<base || (address-base)&15 || address-base>=uint64_t(entries)*16) return false;
        const auto first=(start-base)/16,at=(address-base)/16;
        return (at+entries-first)%entries<count;
    }
    bool belongs(const TransferRecord &record,uint64_t address) const { return belongsRange(record.first,record.trbs,address); }
    bool isoTransferred(const TransferRecord &record,uint64_t address,uint32_t residue,uint32_t &actual) {
        if (!belongsRange(record.isoFirst,record.isoTRBs,address)) return false;
        const auto base=backend.ring().startAddress();const auto entries=backend.ring().dataEntries();
        const auto start=unsigned((record.isoFirst-base)/16);uint32_t sum=0;
        for (unsigned i=0;i<record.isoTRBs;++i) {
            const auto at=base+uint64_t((start+i)%entries)*16;TRB item;
            if (!backend.ring().copyPending(at,item) || item.type()!=(i ? Normal : Iso)) return false;
            const auto bytes=item.status&0x1ffff;
            if (bytes>record.requested-sum) return false;
            sum+=bytes;
            if (at==address) { if (residue>bytes) return false;actual=sum-residue;return true; }
        }
        return false;
    }
    USB::Result retirePending(TransferPhase phase,USB::Result reason) {
        unsigned trbs=0;
        for (unsigned i=0;i<pending;++i) trbs+=records[order[(head+i)%Capacity]].trbs;
        if (backend.ring().pending()!=trbs || (trbs && !backend.ring().retire(trbs,records[order[(head+pending-1)%Capacity]].last)))
            return poison(USB::Result::HardwareError);
        for (auto &record:records) if (record.phase==TransferPhase::Pending || record.phase==TransferPhase::Quarantined) {
            record.phase=phase;if (record.result==USB::Result::Pending) record.result=reason;
        }
        pending=0;head=0;return USB::Result::Success;
    }
public:
    TransferQueue(Backend &operations,Commands &commandQueue):backend(operations),commands(commandQueue) {}
    TransferQueue(const TransferQueue &)=delete;
    TransferQueue &operator=(const TransferQueue &)=delete;
    bool faulted() const { return fault; }
    bool requiresNativeClear() const { return nativeHalt; }
    // Native clearStall also cancels live ordinary I/O. Close publication
    // before draining workers; an empty software queue claims no DMA proof.
    USB::Result beginNativeClear() {
        if (!active || activity!=EndpointActivity::Running || !backend.admitted(epoch) ||
            !commands.accepting() || commands.generation()!=epoch) return USB::Result::NotReady;
        if (!nativeHalt) { nativeHalt=true;nativeRecoveryProved=!fault && !pending && !backend.ring().pending(); }
        return USB::Result::Success;
    }
    USB::Result finishNativeClear() {
        if (!active || fault || pending || retained() || backend.ring().pending() ||
            activity!=EndpointActivity::Running || !backend.admitted(epoch) || !commands.accepting() ||
            commands.generation()!=epoch || (nativeHalt && !nativeRecoveryProved)) return USB::Result::NotReady;
        nativeHalt=nativeRecoveryProved=false;return USB::Result::Success;
    }
    bool pausing() const { return activity==EndpointActivity::Pausing; }
    bool paused() const { return activity==EndpointActivity::Paused; }
    bool pausedStopProof() const { return paused() && pauseProof; }
    uint64_t pausedAt() const { return stoppedAt; }
    unsigned outstanding() const { return pending; }
    unsigned retained() const { unsigned n=0;for (const auto &record:records) n+=record.phase!=TransferPhase::Empty;return n; }
    bool initialized() const { return epoch!=0; }
    uint64_t generation() const { return epoch; }
    uint64_t recoveryCommandFloor() const { return recoveryFloor; }
    USB::Result initialize(uint64_t generation,unsigned slotID,const EndpointConfiguration &configuration,unsigned addressBits,uintptr_t owner) {
        if (epoch) return USB::Result::Busy;
        uint32_t words[8]{};
        if (!generation || !slotID || slotID>commands.maxSlots() || addressBits!=commands.dmaBits() ||
            !owner || commands.generation()!=generation || !commands.accepting() ||
            encodeEndpoint(configuration,addressBits,words)!=USB::Result::Success ||
            configuration.dequeue!=backend.ring().nextAddress() || backend.ring().pending() ||
            !backend.ring().available() || uint32_t(configuration.cycle)!=backend.ring().producerCycle()) return USB::Result::Invalid;
        if (!backend.admitted(generation)) return USB::Result::NotReady;
        epoch=generation;slot=slotID;id=endpointID(configuration.address,configuration.type);bits=addressBits;
        endpoint=configuration;endpointOwner=owner;active=true;return USB::Result::Success;
    }
    USB::Result reconfigure(const EndpointConfiguration &configuration) {
        if (!epoch || !active || fault || pending || retained() || activity!=EndpointActivity::Running) return USB::Result::Busy;
        uint32_t words[8]{};
        if (endpointID(configuration.address,configuration.type)!=id || configuration.address!=endpoint.address ||
            configuration.type!=endpoint.type || configuration.speed!=endpoint.speed ||
            encodeEndpoint(configuration,bits,words)!=USB::Result::Success ||
            configuration.dequeue!=backend.ring().nextAddress() ||
            configuration.cycle!=bool(backend.ring().producerCycle())) return USB::Result::Invalid;
        if (!backend.admitted(epoch) || !commands.accepting()) return USB::Result::NotReady;
        endpoint=configuration;return USB::Result::Success;
    }
    bool ownsEvent(uint64_t ticket,const TRB &event,uint64_t eventEpoch) {
        auto *record=find(ticket);
        if (!record || eventEpoch!=epoch || event.type()!=TransferEvent ||
            (event.control&~(Cycle|EventDataFlag|(63u<<10)|(31u<<16)|(255u<<24))) ||
            (event.control>>24)!=slot || ((event.control>>16)&31)!=id) return false;
        if (!(event.control&EventDataFlag)) return belongs(*record,event.parameter());
        return (event.parameter()>>1)==ticket && ((event.parameter()&1) || record->staged);
    }
    bool stoppedEvent(uint64_t ticket,const TRB &event,uint64_t eventEpoch) {
        if (!epoch || !fault || eventEpoch!=epoch || ticket<=recoveryFloor || commands.generation()!=epoch ||
            event.type()!=TransferEvent || (event.control&~(Cycle|(63u<<10)|(31u<<16)|(255u<<24))) ||
            (event.control>>24)!=slot || ((event.control>>16)&31)!=id) return false;
        const auto code=event.status>>24;
        if (code<26 || code>28) return false;
        const auto address=event.parameter();
        if (address!=backend.ring().nextAddress() &&
            address!=backend.ring().startAddress()+uint64_t(backend.ring().dataEntries())*sizeof(TRB)) return false;
        CommandRecord stopped{};
        if (!commands.copy(ticket,stopped) || stopped.owner!=endpointOwner || stopped.command.type!=StopEndpoint ||
            stopped.command.slot!=slot || stopped.command.endpoint!=id || stopped.command.parameter || stopped.command.option)
            return false;
        // A forced Stopped event can reference the producer gap or Link after
        // all payloads executed. It never retires DMA; the actual successful
        // Stop and Set Dequeue completion records remain the recovery proof.
        return stopped.phase==CommandPhase::Pending || (stopped.phase==CommandPhase::Completed &&
            stopped.result==USB::Result::Success && stopped.completionCode==1);
    }
    USB::Result beginPause() {
        if (!epoch || !active || activity!=EndpointActivity::Running || !commands.accepting() || !backend.admitted(epoch))
            return USB::Result::NotReady;
        pauseFloor=commands.lastTicket();pauseStop=stoppedAt=0;pauseProof=false;
        activity=EndpointActivity::Pausing;return USB::Result::Success;
    }
    bool pauseEvent(uint64_t stoppedTicket,const TRB &event,uint64_t eventEpoch) {
        if (!pausing() || eventEpoch!=epoch || stoppedTicket<=pauseFloor || commands.generation()!=epoch ||
            event.type()!=TransferEvent || (event.control&~(Cycle|(63u<<10)|(31u<<16)|(255u<<24))) ||
            (event.control>>24)!=slot || ((event.control>>16)&31)!=id) return false;
        const auto code=event.status>>24;if (code<26 || code>28) return false;
        CommandRecord stop{};
        if (!commands.copy(stoppedTicket,stop) || stop.owner!=endpointOwner || stop.command.type!=StopEndpoint ||
            stop.command.slot!=slot || stop.command.endpoint!=id || stop.command.parameter || !stop.command.option ||
            (stop.phase!=CommandPhase::Pending && !(stop.phase==CommandPhase::Completed &&
                stop.result==USB::Result::Success && stop.completionCode==1))) return false;
        const auto address=event.parameter();
        for (const auto &record:records) if ((record.phase==TransferPhase::Pending || record.phase==TransferPhase::Quarantined) &&
            belongs(record,address)) return true;
        return address==backend.ring().nextAddress() ||
            address==backend.ring().startAddress()+uint64_t(backend.ring().dataEntries())*sizeof(TRB);
    }
    USB::Result confirmPause(uint64_t stoppedTicket,uint64_t now) {
        if (!pausing() || stoppedTicket<=pauseFloor || commands.generation()!=epoch || !backend.admitted(epoch))
            return USB::Result::NotReady;
        if (now<lastTime) return USB::Result::Invalid;
        CommandRecord stop{};
        if (!commands.copy(stoppedTicket,stop) || stop.phase!=CommandPhase::Completed || stop.result!=USB::Result::Success ||
            stop.completionCode!=1 || stop.owner!=endpointOwner || stop.command.type!=StopEndpoint || !stop.command.option ||
            stop.command.slot!=slot || stop.command.endpoint!=id || stop.command.parameter) return USB::Result::Invalid;
        // Inspect the actual retained Stop(SP) record once. This proof stays
        // valid only while this queue continuously prohibits all doorbells.
        // The command may then be acknowledged without exhausting the shared
        // command pool during a long device suspend.
        pauseStop=stoppedTicket;stoppedAt=lastTime=now;pauseProof=true;activity=EndpointActivity::Paused;
        return USB::Result::Success;
    }
    USB::Result pauseEmpty(uint64_t now) {
        if (!epoch || !active || fault || pending || retained() || backend.ring().pending() || !backend.admitted(epoch) ||
            activity!=EndpointActivity::Running) return USB::Result::Busy;
        if (now<lastTime) return USB::Result::Invalid;
        // HCD has checked a non-running output endpoint context. There is no
        // payload ownership to retire using this empty-queue state.
        pauseStop=0;stoppedAt=lastTime=now;pauseProof=false;activity=EndpointActivity::Paused;return USB::Result::Success;
    }
    void abandonPause() {
        if (pausing()) { activity=EndpointActivity::Running;pauseFloor=pauseStop=stoppedAt=0;pauseProof=false; }
    }
    USB::Result recoverPaused(uint64_t dequeueTicket) {
        if (!pausedStopProof() || !active || !fault || commands.generation()!=epoch || !commands.accepting() ||
            !backend.admitted(epoch) || dequeueTicket<=recoveryFloor || dequeueTicket<=pauseStop) return USB::Result::NotReady;
        CommandRecord dequeue{};
        if (!commands.copy(dequeueTicket,dequeue) || dequeue.phase!=CommandPhase::Completed ||
            dequeue.result!=USB::Result::Success || dequeue.completionCode!=1 || dequeue.owner!=endpointOwner ||
            dequeue.command.type!=SetDequeue || dequeue.command.slot!=slot || dequeue.command.endpoint!=id ||
            dequeue.command.option || dequeue.command.parameter!=(backend.ring().nextAddress()|backend.ring().producerCycle()))
            return USB::Result::Invalid;
        const auto result=retirePending(TransferPhase::Completed,USB::Result::Aborted);
        if (result==USB::Result::Success) { fault=false;nativeRecoveryProved=nativeHalt; }
        return result;
    }
    USB::Result resume(uint64_t now,uint64_t timeoutPause) {
        if (!paused() || fault || !active || !commands.accepting() || commands.generation()!=epoch || !backend.admitted(epoch))
            return USB::Result::NotReady;
        if (now<lastTime) return USB::Result::Invalid;
        if (now<stoppedAt || now-stoppedAt<SuspendMinimumUS) return USB::Result::Pending;
        for (const auto &record:records) if (record.phase==TransferPhase::Pending && record.deadline &&
            timeoutPause>UINT64_MAX-record.deadline) return USB::Result::Invalid;
        if (pending) {
            // No Set Dequeue: opaque partial-TRB/short-packet/EDTLA state is
            // retained by the xHC. Our terminating ED TRBs complete normally
            // before a Stop completion if the controller stopped on an ED.
            const auto result=backend.doorbell(slot,id);
            if (result!=USB::Result::Success) { pauseProof=false;return poison(result); }
        }
        for (auto &record:records) if (record.phase==TransferPhase::Pending && record.deadline) record.deadline+=timeoutPause;
        lastTime=now;activity=EndpointActivity::Running;pauseFloor=pauseStop=stoppedAt=0;pauseProof=false;
        return USB::Result::Success;
    }
    template<unsigned N> USB::Result submit(const TransferDescriptor<N> &descriptor,uintptr_t owner,
        uint64_t now,uint64_t timeoutUS,uint64_t &ticket) {
        ticket=0;
        if (!active || fault) return fault ? USB::Result::HardwareError : USB::Result::NotReady;
        if (activity!=EndpointActivity::Running || nativeHalt) return USB::Result::Busy;
        if (!owner || now<lastTime || timeoutUS>UINT64_MAX-now || !descriptor.count() || descriptor.bits()!=bits ||
            !sameEndpoint(descriptor.configuration(),endpoint)) return USB::Result::Invalid;
        if (!backend.admitted(epoch)) return poison(USB::Result::Aborted);
        if (!commands.accepting() || commands.generation()!=epoch) return poison(USB::Result::HardwareError);
        unsigned index=Capacity;
        for (unsigned i=0;i<Capacity;++i) if (records[i].phase==TransferPhase::Empty) { index=i;break; }
        if (index==Capacity || pending==Capacity || descriptor.count()>backend.ring().available()) return USB::Result::Busy;
        const auto base=backend.ring().startAddress();const auto end=base+backend.ring().allocationBytes();
        const auto &ring=backend.ring();
        for (unsigned i=0;i<descriptor.count();++i) {
            const auto &item=descriptor.data()[i];const auto bytes=item.status&0x1ffff;
            if ((item.type()==Normal || item.type()==Data || item.type()==Iso) && bytes &&
                item.parameter()<end && item.parameter()+bytes>base) return USB::Result::Invalid;
            // Link boundaries may not split a USB packet across two payload
            // fragments. A bounded, suitably aligned HCD bounce buffer avoids
            // this case; reject before publication when repacking is needed.
            if (i+1<descriptor.count() && ring.following(i+1)==base && (item.control&Chain)) {
                uint32_t transferred=0;
                for (unsigned j=0;j<=i;++j) {
                    const auto &part=descriptor.data()[j];
                    if (part.type()==EventData || !(part.control&Chain)) transferred=0;
                    else if (part.type()==Normal || part.type()==Data || part.type()==Iso) transferred+=part.status&0x1ffff;
                }
                bool moreData=false;
                for (unsigned j=i+1;j<descriptor.count() && descriptor.data()[j].type()!=EventData;++j)
                    moreData|=bool(descriptor.data()[j].status&0x1ffff);
                const unsigned burst=endpoint.speed==USB::Speed::Super ? endpoint.burst : (endpoint.maxPacket>>11)&3;
                if (moreData && transferred%((endpoint.maxPacket&2047)*(burst+1))) return USB::Result::Unsupported;
            }
        }
        TRB items[N];for (unsigned i=0;i<descriptor.count();++i) items[i]=descriptor.data()[i];
        const auto allocated=commands.allocateTransferTicket(ticket);
        if (allocated!=USB::Result::Success) return allocated;
        lastTime=now;
        for (unsigned i=0;i<descriptor.count();++i) if (items[i].type()==EventData)
            items[i]=trb(ticket<<1|uint64_t(i+1==descriptor.count()),0,items[i].control);
        auto &record=records[index];record={};
        record.ticket=ticket;record.first=ring.nextAddress();record.last=ring.following(descriptor.count()-1);
        if (endpoint.type==EndpointType::Isochronous) {
            record.isoFirst=ring.following(descriptor.prefix());record.isoTRBs=descriptor.count()-descriptor.prefix()-1;
        }
        record.deadline=timeoutUS ? now+timeoutUS : 0;record.owner=owner;record.trbs=descriptor.count();record.requested=descriptor.bytes();
        record.phase=TransferPhase::Pending;record.input=descriptor.in();record.staged=descriptor.staged();
        backend.retain(owner);order[(head+pending)%Capacity]=index;++pending;
        uint64_t last=0;const auto submitted=backend.ring().submit(items,descriptor.count(),last);
        if (submitted!=USB::Result::Success) {
            --pending;record.phase=TransferPhase::Completed;record.result=submitted;return submitted;
        }
        if (last!=record.last) return poison(USB::Result::HardwareError);
        const auto result=backend.doorbell(slot,id);
        return result==USB::Result::Success ? result : poison(result);
    }
    USB::Result complete(const TRB &event,uint64_t eventEpoch,TransferRecord &out) {
        if (!epoch || eventEpoch!=epoch) return USB::Result::Aborted;
        if (!active || fault) return fault ? USB::Result::HardwareError : USB::Result::NotReady;
        if (!backend.admitted(epoch)) return poison(USB::Result::Aborted);
        if (!commands.accepting()) return poison(USB::Result::HardwareError);
        if (event.type()!=TransferEvent || (event.control&~(Cycle|EventDataFlag|(63u<<10)|(31u<<16)|(255u<<24))) ||
            (event.control>>24)!=slot || ((event.control>>16)&31)!=id) return poison(USB::Result::HardwareError);
        const unsigned code=event.status>>24;const uint32_t bytes=event.status&0xffffff;
        if (!(event.control&EventDataFlag)) {
            if (endpoint.type==EndpointType::Isochronous && !event.parameter() && !bytes && (code==14 || code==15)) return USB::Result::Pending;
            if (endpoint.type==EndpointType::Isochronous && code==23 && pending) {
                // A Missed Service event points into the skipped TD. xHCI
                // may report only the last of consecutive skipped TDs after
                // event-ring pressure. Its ordered prefix is no longer DMA
                // owned; do not reset an otherwise running ISO endpoint.
                unsigned skipped=0;
                for (unsigned i=0;i<pending;++i) {
                    const auto &record=records[order[(head+i)%Capacity]];
                    uint32_t ignored=0;
                    if (isoTransferred(record,event.parameter(),bytes,ignored)) {
                        skipped=i+1;break;
                    }
                }
                if (!skipped) return poison(USB::Result::HardwareError);
                for (unsigned i=0;i<skipped;++i) {
                    auto &record=records[order[head]];
                    if (record.phase!=TransferPhase::Pending || !backend.ring().retire(record.trbs,record.last))
                        return poison(USB::Result::HardwareError);
                    record.phase=TransferPhase::Completed;record.result=USB::Result::Timeout;
                    record.completionCode=23;record.transferred=0;out=record;
                    head=(head+1)%Capacity;--pending;
                }
                return USB::Result::Success;
            }
            if (endpoint.type==EndpointType::Isochronous && pending && (code==2 || code==3 || code==4 || code==31)) {
                auto &record=records[order[head]];uint32_t actual=0;
                if (record.phase!=TransferPhase::Pending || (code==31 && !record.input) ||
                    !isoTransferred(record,event.parameter(),bytes,actual) ||
                    !backend.ring().retire(record.trbs,record.last)) return poison(USB::Result::HardwareError);
                // ISO USB errors terminate this TD without halting the
                // endpoint (xHCI 4.10.2). The reported residue identifies
                // the valid prefix, including prior payload fragments.
                record.phase=TransferPhase::Completed;record.completionCode=code;
                record.result=code==3 || code==31 ? USB::Result::Overflow : USB::Result::HardwareError;
                record.transferred=actual;record.dataValid=true;out=record;
                head=(head+1)%Capacity;--pending;return USB::Result::Success;
            }
            if (!pending || !belongs(records[order[head]],event.parameter())) return poison(USB::Result::HardwareError);
            records[order[head]].completionCode=code;
            if (endpoint.type!=EndpointType::Isochronous && (code==2 || code==3 || code==4 || code==6 || code==36)) {
                nativeHalt=true;nativeRecoveryProved=false;
            }
            // No payload TRB requests an ordinary event. Any physical event
            // is an error/stop or a controller anomaly, never a DMA release.
            return poison(USB::Result::HardwareError);
        }
        const uint64_t ticket=event.parameter()>>1;const bool final=event.parameter()&1;
        auto *record=find(ticket);
        if (!ticket || ticket>commands.lastTransferTicket()) return poison(USB::Result::HardwareError);
        if (!record || record->phase==TransferPhase::Completed || record->phase==TransferPhase::Isolated) return USB::Result::Aborted;
        if (!pending || record!=&records[order[head]] || record->phase!=TransferPhase::Pending) return poison(USB::Result::HardwareError);
        if ((code!=1 && code!=13) || (code==13 && !record->input)) {
            if (endpoint.type!=EndpointType::Isochronous && (code==2 || code==3 || code==4 || code==6 || code==36)) {
                nativeHalt=true;nativeRecoveryProved=false;
            }
            record->completionCode=code;return poison(USB::Result::HardwareError);
        }
        const uint32_t expected=final && record->staged ? 0 : record->requested;
        if (bytes>expected || (code==1 && bytes!=expected) || (final && record->staged && (code!=1 || !record->dataSeen)) ||
            (!final && (!record->staged || record->dataSeen))) return poison(USB::Result::HardwareError);
        record->transferred+=bytes;record->shortPacket|=code==13;record->completionCode=record->shortPacket ? 13 : code;
        if (!final) { record->dataSeen=true;out=*record;return USB::Result::Pending; }
        if (!backend.ring().retire(record->trbs,record->last)) return poison(USB::Result::HardwareError);
        record->phase=TransferPhase::Completed;record->result=USB::Result::Success;record->dataValid=true;
        out=*record;head=(head+1)%Capacity;--pending;return USB::Result::Success;
    }
    USB::Result expire(uint64_t now) {
        if (!active || fault) return fault ? USB::Result::HardwareError : USB::Result::NotReady;
        if (now<lastTime) return USB::Result::Invalid;
        lastTime=now;
        if (!backend.admitted(epoch)) return poison(USB::Result::Aborted);
        if (!commands.accepting()) return poison(USB::Result::HardwareError);
        if (activity!=EndpointActivity::Running) return USB::Result::Success;
        for (const auto &record:records) if (record.phase==TransferPhase::Pending && record.deadline && now>=record.deadline)
            return poison(USB::Result::Timeout);
        return USB::Result::Success;
    }
    // Endpoint-wide quiescence, not a claim of selective request cancellation.
    USB::Result quiesce() { return epoch && active ? poison(USB::Result::Aborted) : USB::Result::NotReady; }
    USB::Result recover(uint64_t stoppedTicket,uint64_t dequeueTicket) {
        if (!epoch || !active || !fault || activity!=EndpointActivity::Running ||
            commands.generation()!=epoch || !commands.accepting()) return USB::Result::NotReady;
        CommandRecord stopped{},dequeue{};
        if (stoppedTicket<=recoveryFloor || dequeueTicket<=stoppedTicket || !commands.copy(stoppedTicket,stopped) ||
            !commands.copy(dequeueTicket,dequeue)) return USB::Result::Invalid;
        const auto valid=[&](const CommandRecord &record) {
            return record.phase==CommandPhase::Completed && record.result==USB::Result::Success && record.completionCode==1 &&
                record.owner==endpointOwner && record.command.slot==slot && record.command.endpoint==id && !record.command.option;
        };
        if (!valid(stopped) || !valid(dequeue) || (stopped.command.type!=StopEndpoint && stopped.command.type!=ResetEndpoint) ||
            dequeue.command.type!=SetDequeue || dequeue.command.parameter!=(backend.ring().nextAddress()|backend.ring().producerCycle()))
            return USB::Result::Invalid;
        const auto result=retirePending(TransferPhase::Completed,USB::Result::Aborted);
        if (result==USB::Result::Success) { fault=false;active=backend.admitted(epoch);nativeRecoveryProved=nativeHalt; }
        return result;
    }
    USB::Result isolate() {
        if (!epoch) return USB::Result::NotReady;
        const auto proof=backend.isolation(epoch);if (proof!=USB::Result::Success) return proof;
        const auto result=retirePending(TransferPhase::Isolated,USB::Result::Aborted);
        if (result==USB::Result::Success) {
            fault=false;active=false;nativeHalt=nativeRecoveryProved=false;
            activity=EndpointActivity::Running;pauseFloor=pauseStop=stoppedAt=0;pauseProof=false;
        }
        return result;
    }
    bool copy(uint64_t ticket,TransferRecord &out) { auto *record=find(ticket);if (!record) return false;out=*record;return true; }
    USB::Result acknowledge(uint64_t ticket,uintptr_t expectedOwner,uintptr_t &releasedOwner) {
        releasedOwner=0;auto *record=find(ticket);
        if (!record || record->owner!=expectedOwner) return USB::Result::Invalid;
        if (record->phase!=TransferPhase::Completed && record->phase!=TransferPhase::Isolated) return USB::Result::Busy;
        releasedOwner=record->owner;*record={};return USB::Result::Success;
    }
};
}
