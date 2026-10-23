// SPDX-License-Identifier: BSD-2-Clause
#include "DWC3Transfer.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

using USB::Result;
struct Controller {
    struct Call { unsigned ep; DWC3::Command cmd; uint32_t p0, p1, p2, options; };
    std::vector<Call> calls;
    bool halted = false;
    unsigned index = 7;
    Result startResult = Result::Success, endResult = Result::Pending;
    unsigned endpointCount() const { return 16; }
    unsigned dmaBits() const { return 40; }
    bool stopped() const { return halted; }
    Result startTransfer(unsigned ep, uint64_t address, DWC3::TRBType, uint32_t &reply) {
        return command(ep, DWC3::Command::Start, uint32_t(address >> 32), uint32_t(address), 0, 0, reply);
    }
    Result command(unsigned ep, DWC3::Command cmd, uint32_t p0, uint32_t p1,
                   uint32_t p2, uint32_t options, uint32_t &reply) {
        calls.push_back({ep,cmd,p0,p1,p2,options}); reply = index << 16;
        return cmd == DWC3::Command::Start ? startResult : endResult;
    }
};
struct Order {
    unsigned reads = 0, writes = 0;
    void dmaReadBarrier() { ++reads; }
    void dmaWriteBarrier() { ++writes; }
};
DWC3::Event complete(unsigned ep) { return {ep << 1 | 1u << 6}; }
DWC3::Event ended(unsigned ep) { return {ep << 1 | 7u << 6 | 8u << 24}; }

static void inTransfer() {
    Controller c; Order o; alignas(16) std::array<DWC3::TRB, 8> memory{};
    DWC3::Transfer<Controller,Order> transfer(c,o); DWC3::TransferCompletion done{};
    assert(transfer.initialize(3,memory.data(),0x123450000,8,2) == Result::Success);
    DWC3::DMASegment segments[] = {{0x100000010,512},{0x100001000,1024}};
    assert(transfer.submit(segments,2,1536,512,DWC3::TRBType::Normal,99) == Result::Success);
    assert(transfer.ownsBuffers() && transfer.transferResource() == 7 && o.writes == 3);
    assert(c.calls.back().p0 == 1 && c.calls.back().p1 == 0x23450000);
    assert(memory[0].addressHi == 1 && memory[0].addressLo == 16 && memory[0].size == 512);
    assert(memory[0].control & DWC3::Chain && !(memory[0].control & DWC3::Last));
    assert(memory[1].control & DWC3::Last && memory[1].control & DWC3::InterruptComplete);
    assert(transfer.submit(segments,2,1536,512,DWC3::TRBType::Normal,100) == Result::NotReady);
    memory[0].control &= ~DWC3::Owned; memory[0].size = 0;
    memory[1].control &= ~DWC3::Owned; memory[1].size = 0;
    assert(transfer.handle(complete(3),1,done) == Result::Invalid && transfer.ownsBuffers());
    assert(transfer.handle(complete(4),2,done) == Result::Invalid && transfer.ownsBuffers());
    assert(transfer.handle(complete(3),2,done) == Result::Success);
    assert(done.result == Result::Success && done.bytes == 1536 && done.cookie == 99 && done.epoch == 2);
    assert(!transfer.ownsBuffers());
    assert(transfer.handle(complete(3),2,done) == Result::NotReady);
    assert(transfer.submit(segments,2,1536,512,DWC3::TRBType::Normal,100,true) == Result::Success);
    assert(memory[2].size == 0 && memory[2].addressLo == 0 && memory[2].control & DWC3::Last);
    for(unsigned i=0;i<3;++i){memory[i].control &= ~DWC3::Owned;memory[i].size=0;}
    assert(transfer.handle(complete(3),2,done) == Result::Success && done.bytes == 1536);
    assert(transfer.submit(segments,1,512,512,DWC3::TRBType::ControlData,101) == Result::Invalid);
}

static void outTransfer() {
    Controller c; Order o; alignas(16) std::array<DWC3::TRB, 4> memory{};
    DWC3::Transfer<Controller,Order> transfer(c,o); DWC3::TransferCompletion done{};
    assert(transfer.initialize(2,memory.data(),0x10000,4,1) == Result::Success);
    DWC3::DMASegment segments[]={{0x20000,512},{0x21000,512}};
    assert(transfer.submit(segments,2,700,512,DWC3::TRBType::Normal,1) == Result::Success);
    // A short packet terminates the TD; hardware leaves its unused suffix
    // owned. That suffix is safe only because XferComplete stopped the TD.
    memory[0].control &= ~DWC3::Owned;memory[0].size=400;
    assert(transfer.handle(complete(2),1,done) == Result::Success);
    assert(done.bytes == 112 && done.result == Result::Success);
    assert(memory[1].control & DWC3::Owned);
    assert(transfer.submit(segments,2,700,512,DWC3::TRBType::Normal,2) == Result::Success);
    memory[0].control &= ~DWC3::Owned;memory[0].size=0;
    memory[1].control &= ~DWC3::Owned;memory[1].size=0;
    assert(transfer.handle(complete(2),1,done) == Result::Success);
    assert(done.bytes == 700 && done.result == Result::Overflow);
    auto invalid=segments[0];invalid.length=700;
    unsigned before=unsigned(c.calls.size());
    assert(transfer.submit(&invalid,1,700,512,DWC3::TRBType::Normal,3) == Result::Invalid);
    assert(transfer.submit(segments,2,700,512,DWC3::TRBType::Normal,3,true) == Result::Invalid);
    invalid.address=(1ull<<40)-256; invalid.length=512;
    assert(transfer.submit(&invalid,1,512,512,DWC3::TRBType::Normal,3) == Result::Invalid);
    invalid.address=0x10000;invalid.length=512;
    assert(transfer.submit(&invalid,1,512,512,DWC3::TRBType::Normal,3) == Result::Invalid);
    assert(transfer.submit(reinterpret_cast<DWC3::DMASegment *>(memory.data()),1,512,512,
        DWC3::TRBType::Normal,3) == Result::Invalid);
    assert(c.calls.size()==before);
    assert(transfer.submit(segments,1,0,512,DWC3::TRBType::Normal,4) == Result::Success);
    memory[0].control &= ~DWC3::Owned;memory[0].size=512;
    assert(transfer.handle(complete(2),1,done)==Result::Success && done.bytes==0);
    invalid={0x20000,513};
    assert(transfer.submit(&invalid,1,1,512,DWC3::TRBType::Normal,5)==Result::Invalid);
}

static void cancellation() {
    Controller c; Order o; alignas(16) std::array<DWC3::TRB,4> memory{};
    DWC3::Transfer<Controller,Order> transfer(c,o); DWC3::TransferCompletion done{};
    assert(transfer.initialize(3,memory.data(),0x10000,4,1)==Result::Success);
    DWC3::DMASegment seg{0x20000,512};
    assert(transfer.submit(&seg,1,512,512,DWC3::TRBType::Normal,1)==Result::Success);
    assert(transfer.cancel(Result::Timeout)==Result::Pending);
    auto call=c.calls.back();
    assert(call.cmd==DWC3::Command::End && call.options==(7u<<16|DWC3::CommandForce|DWC3::CommandInterrupt));
    assert(transfer.handle(complete(3),1,done)==Result::Pending && transfer.ownsBuffers());
    unsigned before=unsigned(c.calls.size());
    assert(transfer.cancel()==Result::Pending && c.calls.size()==before);
    assert(transfer.handle(ended(3),1,done)==Result::Success && !transfer.ownsBuffers());
    assert(done.result==Result::Timeout && done.cookie==1);
    assert(transfer.submit(&seg,1,512,512,DWC3::TRBType::Normal,2)==Result::Success);
    c.endResult=Result::Timeout;
    assert(transfer.cancel()==Result::Timeout && transfer.ownsBuffers());
    assert(transfer.currentState()==DWC3::TransferState::Fault);
    assert(transfer.handle(ended(3),1,done)==Result::NotReady && transfer.ownsBuffers());
    assert(transfer.reclaimAfterHalt(done)==Result::NotReady);
    c.halted=true;
    assert(transfer.reclaimAfterHalt(done)==Result::Success && done.cookie==2);
    assert(!transfer.ownsBuffers());
    c.startResult=Result::Timeout; c.halted=false;
    assert(transfer.submit(&seg,1,512,512,DWC3::TRBType::Normal,3)==Result::Timeout);
    assert(transfer.ownsBuffers() && transfer.reclaimAfterHalt(done)==Result::NotReady);
    c.halted=true;
    assert(transfer.reclaimAfterHalt(done)==Result::Success && done.result==Result::HardwareError);
}

static void malformedCompletion() {
    Controller c;Order o;alignas(16) std::array<DWC3::TRB,4> memory{};
    DWC3::Transfer<Controller,Order> transfer(c,o);DWC3::TransferCompletion done{};
    assert(transfer.initialize(3,memory.data(),0x10000,4,1)==Result::Success);
    DWC3::DMASegment seg{0x20000,512};
    for(unsigned malformed=0;malformed<3;++malformed){
        assert(transfer.submit(&seg,1,512,512,DWC3::TRBType::Normal,1)==Result::Success);
        if(malformed)memory[0].control &= ~DWC3::Owned;
        memory[0].size=malformed==1?513:0;
        if(malformed==2){
            memory[0].size=2u<<28;
            assert(transfer.handle(complete(3),1,done)==Result::Success);
            assert(done.result==Result::HardwareError && !transfer.ownsBuffers());
        }else{
            assert(transfer.handle(complete(3),1,done)==Result::HardwareError && transfer.ownsBuffers());
            c.halted=true;assert(transfer.reclaimAfterHalt(done)==Result::Success);c.halted=false;
        }
    }
}

static void controlStages() {
    Controller c;Order o;alignas(16) std::array<DWC3::TRB,2> memory{};
    DWC3::Transfer<Controller,Order> out(c,o);DWC3::TransferCompletion done{};
    assert(out.initialize(0,memory.data(),0x10000,2,1)==Result::Success);
    DWC3::DMASegment setup{0x20000,8};
    assert(out.submit(&setup,1,8,64,DWC3::TRBType::Setup,1)==Result::Success);
    assert((memory[0].control>>4&63)==unsigned(DWC3::TRBType::Setup));
    memory[0].control &= ~DWC3::Owned;memory[0].size=0;
    assert(out.handle(complete(0),1,done)==Result::Success && done.bytes==8);
    assert(out.submit(nullptr,0,0,64,DWC3::TRBType::Status3,2)==Result::Success);
    memory[0].control &= ~DWC3::Owned;
    assert(out.handle(complete(0),1,done)==Result::Success && done.bytes==0);
    assert(out.submit(nullptr,0,1,64,DWC3::TRBType::Status3,3)==Result::Invalid);
    assert(out.submit(&setup,1,7,64,DWC3::TRBType::Setup,3)==Result::Invalid);
}

int main(){
    inTransfer();outTransfer();cancellation();malformedCompletion();controlStages();
    std::puts("DWC3 transfers: SG descriptors, OUT capacity, short packets, ZLP, control stages, cancellation and DMA quarantine passed");
}
