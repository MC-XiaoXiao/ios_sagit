// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "XHCIRing.h"

namespace XHCI {
constexpr uint32_t MicroFrameMask=0x3fff;
constexpr uint64_t MicroFrameNanoseconds=125000;
// Native Family supplies a rollover count and an unextended microframe
// sample. One xHCI rollover spans 2^11 millisecond frames.
constexpr uint64_t frameNumber(uint64_t rollovers,uint64_t raw) {
    return (rollovers<<11)+((raw&MicroFrameMask)>>3);
}
inline bool rolloverNanoseconds(uint64_t now,unsigned raw,uint64_t &stamp) {
    if (raw>MicroFrameMask || now<uint64_t(raw)*MicroFrameNanoseconds) return false;
    stamp=now-uint64_t(raw)*MicroFrameNanoseconds;
    return true;
}
inline bool validMicroFrameWrapEvent(const TRB &event) {
    return event.type()==MicroFrameWrapEvent && !event.lo && !event.hi && event.status==(1u<<24) &&
        !(event.control&~(Cycle|(63u<<10)));
}

// Read-only observer of the real event segment. The IRQ filter advances this
// cursor before the action thread can return event slots to hardware. It
// changes neither ERDP nor the EventRing consumer. Serialize observe/close
// with an IRQ-safe lock and close before releasing the DMA allocation.
class FrameEvents {
    const volatile TRB *memory=nullptr;
    uint64_t epoch=0,wraps=0;
    unsigned entries=0,at=0;
    uint32_t cycle=Cycle;
public:
    bool initialize(const TRB *cpu,unsigned count,uint64_t generation) {
        if (epoch || !cpu || (reinterpret_cast<uintptr_t>(cpu)&63) || !generation || count<16 || count>4096)
            return false;
        memory=cpu;entries=count;epoch=generation;return true;
    }
    bool active() const { return memory!=nullptr; }
    uint64_t observedWraps() const { return wraps; }
    void close() { memory=nullptr;entries=0; }
    template<class Order,class Publish> USB::Result observe(uint64_t generation,Order &order,Publish publish) {
        if (generation!=epoch) return USB::Result::Invalid;
        if (!memory) return USB::Result::NotReady;
        for (unsigned count=0;count<entries;++count) {
            const auto control=memory[at].control;
            if ((control&Cycle)!=cycle) break;
            order.dmaReadBarrier();
            const TRB event{memory[at].lo,memory[at].hi,memory[at].status,control};
            if (event.type()==MicroFrameWrapEvent) {
                if (!validMicroFrameWrapEvent(event) || wraps==UINT64_MAX) return USB::Result::HardwareError;
                const auto result=publish();
                if (result!=USB::Result::Success) return result;
                ++wraps;
            }
            if (++at==entries) { at=0;cycle^=Cycle; }
        }
        return USB::Result::Success;
    }
};
}
