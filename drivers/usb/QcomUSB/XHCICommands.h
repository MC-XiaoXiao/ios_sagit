// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "XHCIRing.h"

namespace XHCI {
struct Command {
    Type type=CommandNoop;
    uint64_t parameter=0;
    unsigned slot=0,endpoint=0;
    bool option=false;
    USB::Result encode(unsigned slots,unsigned stride,unsigned bits,TRB &out) const {
        if (!slots || slots>255 || (stride!=32 && stride!=64) || bits<32 || bits>40 || slot>slots || endpoint>31)
            return USB::Result::Invalid;
        uint32_t control=unsigned(type)<<10|slot<<24;
        switch (type) {
        case EnableSlot:
            if (slot || parameter || option) return USB::Result::Invalid;
            control|=endpoint<<16;break; // endpoint field holds Supported Protocol slot type
        case CommandNoop:
            if (slot || endpoint || parameter || option) return USB::Result::Invalid;
            break;
        case DisableSlot:case ResetDevice:
            if (!slot || endpoint || parameter || option) return USB::Result::Invalid;
            break;
        case AddressDevice:case ConfigureEndpoint:case EvaluateContext:
            if (!slot || endpoint || !parameter || !USB::dmaRange(parameter,33*stride,bits,64) ||
                (type==EvaluateContext && option)) return USB::Result::Invalid;
            if (option) control|=1u<<9;
            break;
        case ResetEndpoint:case StopEndpoint:
            if (!slot || !endpoint || parameter) return USB::Result::Invalid;
            control|=endpoint<<16;
            if (option) control|=type==StopEndpoint ? 1u<<23 : 1u<<9;
            break;
        case SetDequeue:
            if (!slot || !endpoint || option || !(parameter&~uint64_t(15)) || (parameter&14) ||
                !USB::dmaRange(parameter&~uint64_t(15),16,bits,16)) return USB::Result::Invalid;
            control|=endpoint<<16;break; // no stream context or stream ID
        default:return USB::Result::Unsupported;
        }
        out=trb(parameter,0,control);return USB::Result::Success;
    }
};
enum class CommandPhase : uint8_t { Empty,Pending,Completed,Quarantined,Isolated };
struct CommandRecord {
    uint64_t ticket=0,address=0,deadline=0;
    uintptr_t owner=0;
    Command command{};
    CommandPhase phase=CommandPhase::Empty;
    unsigned completionCode=0,slot=0;
    USB::Result result=USB::Result::Pending;
};

// One queue/ring per controller epoch. Serialized with the controller gate.
// Backend supplies ProducerRing&, an actual admission check, doorbell, retain,
// and fresh reset/old-ring isolation proof. Caller keeps the command payload
// prepared and strongly held throughout submit, even before retain is taken.
// acknowledge returns a held owner for release OUTSIDE the controller gate.
template<class Backend,unsigned Capacity=64> class CommandQueue {
    static_assert(Capacity && Capacity<=256,"bounded command metadata");
    Backend &backend;
    CommandRecord records[Capacity]{};
    unsigned order[Capacity]{},head=0,pending=0,slots=0,stride=0,bits=0;
    uint64_t epoch=0,serial=0,last=0,transferSerial=0;
    bool active=false,fault=false;
    USB::Result poison(USB::Result r) {
        fault=true;
        for (auto &record:records) if (record.phase==CommandPhase::Pending) record.phase=CommandPhase::Quarantined;
        return r;
    }
    CommandRecord *find(uint64_t ticket) {
        if (!ticket) return nullptr;
        for (auto &record:records) if (record.phase!=CommandPhase::Empty && record.ticket==ticket) return &record;
        return nullptr;
    }
    static USB::Result completion(unsigned code) {
        switch (code) {
        case 1:return USB::Result::Success;
        case 5:case 10:case 17:case 19:case 34:return USB::Result::Invalid;
        case 7:case 8:case 9:case 18:case 35:return USB::Result::Busy;
        case 11:case 12:return USB::Result::NotReady;
        case 22:case 29:return USB::Result::Unsupported;
        default:return USB::Result::HardwareError;
        }
    }
    static bool terminalCode(unsigned code) {
        switch (code) {
        case 1:case 2:case 3:case 4:case 5:case 6:case 7:case 8:case 9:case 10:case 11:case 12:
        case 17:case 18:case 19:case 20:case 22:case 29:case 34:case 35:case 36:return true;
        default:return false;
        }
    }
public:
    explicit CommandQueue(Backend &operations):backend(operations) {}
    CommandQueue(const CommandQueue &)=delete;
    CommandQueue &operator=(const CommandQueue &)=delete;
    bool faulted() const { return fault; }
    bool accepting() const { return active && !fault; }
    uint64_t generation() const { return epoch; }
    uint64_t lastTicket() const { return serial; }
    unsigned maxSlots() const { return slots; }
    unsigned dmaBits() const { return bits; }
    uint64_t lastTransferTicket() const { return transferSerial; }
    // All endpoint queues of this controller share ED cookies. Recreating an
    // endpoint must not make an old ED event match a new endpoint's ticket1.
    USB::Result allocateTransferTicket(uint64_t &ticket) {
        ticket=0;
        if (!active || fault) return fault ? USB::Result::HardwareError : USB::Result::NotReady;
        if (transferSerial==(UINT64_MAX>>1)) return USB::Result::Overflow;
        ticket=++transferSerial;return USB::Result::Success;
    }
    unsigned outstanding() const { return pending; }
    unsigned retained() const {
        unsigned n=0;for (const auto &r:records) n+=r.phase!=CommandPhase::Empty;return n;
    }
    USB::Result initialize(uint64_t generation,unsigned maxSlots,unsigned contextStride,unsigned addressBits) {
        if (epoch) return USB::Result::Busy;
        if (!generation || !maxSlots || maxSlots>255 || (contextStride!=32 && contextStride!=64) ||
            addressBits<32 || addressBits>40 || !backend.ring().startAddress() || backend.ring().pending() ||
            !backend.ring().available()) return USB::Result::Invalid;
        if (!backend.admitted(generation)) return USB::Result::NotReady;
        epoch=generation;slots=maxSlots;stride=contextStride;bits=addressBits;active=true;
        return USB::Result::Success;
    }
    USB::Result submit(const Command &command,uintptr_t owner,uint64_t now,uint64_t timeout,uint64_t &ticket) {
        ticket=0;
        if (!active || fault) return fault ? USB::Result::HardwareError : USB::Result::NotReady;
        if (!owner || !timeout || timeout>5000000 || now<last || now>UINT64_MAX-timeout) return USB::Result::Invalid;
        if (!backend.admitted(epoch)) return poison(USB::Result::Aborted);
        TRB item{};const auto valid=command.encode(slots,stride,bits,item);if (valid!=USB::Result::Success) return valid;
        if (serial==UINT64_MAX) return USB::Result::Overflow;
        unsigned index=Capacity;
        for (unsigned i=0;i<Capacity;++i) if (records[i].phase==CommandPhase::Empty) { index=i;break; }
        if (index==Capacity || pending==Capacity || !backend.ring().available()) return USB::Result::Busy;
        const auto address=backend.ring().nextAddress();last=now;ticket=++serial;
        records[index]={ticket,address,now+timeout,owner,command,CommandPhase::Pending,0,0,USB::Result::Pending};
        backend.retain(owner);
        order[(head+pending)%Capacity]=index;++pending;
        uint64_t published=0;const auto submitted=backend.ring().submit(&item,1,published);
        if (submitted!=USB::Result::Success) {
            --pending;records[index].phase=CommandPhase::Completed;records[index].result=submitted;return submitted;
        }
        if (published!=address) return poison(USB::Result::HardwareError);
        // A running ring may fetch before doorbell. Both metadata and retain
        // therefore precede the first valid cycle bit, not only the MMIO write.
        const auto r=backend.doorbell();return r==USB::Result::Success ? r : poison(r);
    }
    USB::Result complete(const TRB &event,uint64_t eventEpoch,CommandRecord &out) {
        if (!epoch || eventEpoch!=epoch) return USB::Result::Aborted;
        if (!active || fault) return fault ? USB::Result::HardwareError : USB::Result::NotReady;
        if (event.type()!=CommandEvent || (event.control&~(Cycle|(63u<<10)|(255u<<24)))) return poison(USB::Result::HardwareError);
        const unsigned code=event.status>>24,slot=event.control>>24;
        // Stopped/aborted command-ring events are not ordinary command
        // completions. Do not turn an unknown context update into free memory.
        if (!terminalCode(code) || !pending) return poison(USB::Result::HardwareError);
        auto &record=records[order[head]];
        if (record.phase!=CommandPhase::Pending || event.parameter()!=record.address ||
            (record.command.type==EnableSlot ? (code==1 ? (!slot || slot>slots) : slot!=0) : slot!=record.command.slot))
            return poison(USB::Result::HardwareError);
        if (!backend.ring().retire(1,record.address)) return poison(USB::Result::HardwareError);
        record.completionCode=code;record.slot=slot;record.result=completion(code);record.phase=CommandPhase::Completed;
        out=record;head=(head+1)%Capacity;--pending;return USB::Result::Success;
    }
    USB::Result expire(uint64_t now) {
        if (!active || fault) return fault ? USB::Result::HardwareError : USB::Result::NotReady;
        if (now<last) return USB::Result::Invalid;
        last=now;
        if (!backend.admitted(epoch)) return poison(USB::Result::Aborted);
        for (const auto &record:records) if (record.phase==CommandPhase::Pending && now>=record.deadline)
            return poison(USB::Result::Timeout);
        return USB::Result::Success;
    }
    bool copy(uint64_t ticket,CommandRecord &out) {
        const auto *record=find(ticket);if (!record) return false;out=*record;return true;
    }
    bool copyRetained(unsigned index,CommandRecord &out) const {
        if (index>=Capacity || records[index].phase==CommandPhase::Empty) return false;
        out=records[index];return true;
    }
    USB::Result quarantine(USB::Result reason) {
        if (!epoch) return USB::Result::NotReady;
        if (reason==USB::Result::Success || reason==USB::Result::Pending) return USB::Result::Invalid;
        return poison(reason);
    }
    USB::Result isolate() {
        if (!epoch) return USB::Result::NotReady;
        const auto r=backend.isolation(epoch);if (r!=USB::Result::Success) return r;
        // Only software ring bookkeeping changes after actual reset proof;
        // no TRB memory, CRCR or event source is reinstalled here.
        if (backend.ring().pending()!=pending) return poison(USB::Result::HardwareError);
        if (pending && !backend.ring().retire(pending,records[order[(head+pending-1)%Capacity]].address))
            return poison(USB::Result::HardwareError);
        pending=0;head=0;active=false;fault=false;
        for (auto &record:records) if (record.phase!=CommandPhase::Empty) {
            record.phase=CommandPhase::Isolated;record.result=USB::Result::Aborted;
        }
        return USB::Result::Success;
    }
    USB::Result acknowledge(uint64_t ticket,uintptr_t expectedOwner,uintptr_t &releasedOwner) {
        releasedOwner=0;auto *record=find(ticket);
        if (!record || record->owner!=expectedOwner) return USB::Result::Invalid;
        if (record->phase!=CommandPhase::Completed && record->phase!=CommandPhase::Isolated) return USB::Result::Busy;
        releasedOwner=record->owner;*record={};return USB::Result::Success;
    }
};
}
