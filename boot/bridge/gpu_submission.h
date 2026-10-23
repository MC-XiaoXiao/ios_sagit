// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "../../drivers/gpu/QcomGPU/AdrenoSubmit.h"

namespace BridgeGPU {
// Initialization may intentionally clear stale reset status. Once the final
// bootstrap engine starts, remember every fatal bit, including one arriving
// during halt/drain just before Engine::stop acknowledges the interrupt.
template<class Registers> struct SubmissionIO {
    Registers &registers;
    uint32_t errors=0;
    uint32_t read(uint32_t offset) {
        const auto value=registers.read(offset);
        if(offset==Adreno::InterruptStatus)errors|=value&Adreno::ErrorIRQs;
        if(offset==Adreno::CPInterruptStatus)errors|=value&0x3f;
        return value;
    }
    void write(uint32_t offset,uint32_t value) {
        if(offset==Adreno::InterruptClear){(void)read(Adreno::InterruptStatus);(void)read(Adreno::CPInterruptStatus);}
        registers.write(offset,value);
    }
    void publish() {registers.publish();}
    void acquire() {registers.acquire();}
    void delay(unsigned us) {registers.delay(us);}
    bool faulted() {(void)read(Adreno::InterruptStatus);(void)read(Adreno::CPInterruptStatus);return errors!=0;}
};

// Four uncached, boot-reserved 4 KiB pages. Only the timestamp page is used by
// Engine::start; the IB has a read-only GPU mapping and the data pages are RW.
// A CPU address is never used as an IOVA. The caller owns the mappings and
// must stop/drain the engine after every result, including timeout or fault.
struct SubmissionProbe {
    static constexpr uint32_t PageBytes=4096, Bytes=4*PageBytes;
    static constexpr uint32_t IBOffset=PageBytes, SourceOffset=2*PageBytes, DestinationOffset=3*PageBytes;
    static constexpr uint32_t SourceWord=16, DestinationWord=32, Words=32;
    static constexpr uint32_t SourceGuard=0xa55a21e7, DestinationGuard=0x5aa5de18, Pattern=0x5408351a;

    template<class Registers,class Healthy>
    static GPU::Result run(Adreno::Engine<Registers> &engine,Registers &io,
        uint32_t *memory,uint32_t bytes,uint64_t fenceVA,Healthy healthy) {
        using GPU::Result;
        if(!memory || (reinterpret_cast<uintptr_t>(memory)&3) || bytes<Bytes ||
            (fenceVA&(Bytes-1)) || !Adreno::deviceRange(fenceVA,Bytes))return Result::Invalid;
        if(!engine.ready() || !healthy())return Result::Offline;
        auto *ib=memory+IBOffset/4,*source=memory+SourceOffset/4,*destination=memory+DestinationOffset/4;
        for(unsigned i=0;i<PageBytes/4;++i) {
            ib[i]=0;source[i]=SourceGuard;destination[i]=DestinationGuard;
        }
        Adreno::Encoder encoder(ib,PageBytes/4);
        if(encoder.fill(fenceVA+SourceOffset+4*SourceWord,4*Words,Pattern)!=Result::Success ||
            encoder.barrier()!=Result::Success ||
            encoder.copy(fenceVA+SourceOffset+4*SourceWord,fenceVA+DestinationOffset+4*DestinationWord,4*Words)!=Result::Success ||
            encoder.barrier()!=Result::Success)return Result::NoResources;
        uint32_t sequence=0;
        auto status=engine.post(fenceVA+IBOffset,encoder.size(),sequence);
        if(status!=Result::Success)return status;
        // poll() acquires GPU writes after the real CACHE_FLUSH_TS. An EFI
        // return, an empty software queue or a CPU-written stamp cannot pass.
        for(unsigned elapsed=0;elapsed<100000;elapsed+=10) {
            if(!healthy())return Result::Offline;
            uint32_t completed=0;status=engine.poll(completed);
            if(status!=Result::Success)return status;
            if(completed==sequence) {
                io.acquire();
                const volatile uint32_t *src=source,*dst=destination;
                for(unsigned i=0;i<PageBytes/4;++i) {
                    const uint32_t expectedSource=i>=SourceWord && i<SourceWord+Words?Pattern:SourceGuard;
                    const uint32_t expectedDestination=i>=DestinationWord && i<DestinationWord+Words?Pattern:DestinationGuard;
                    if(src[i]!=expectedSource || dst[i]!=expectedDestination)return Result::Fault;
                }
                return healthy()?Result::Success:Result::Offline;
            }
            io.delay(10);
        }
        return Result::Timeout;
    }
};
} // namespace BridgeGPU
