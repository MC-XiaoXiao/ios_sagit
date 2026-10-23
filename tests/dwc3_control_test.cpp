// SPDX-License-Identifier: BSD-2-Clause
#include "DWC3Control.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

using USB::Result;
using DWC3::ControlState;
using Kind = DWC3::ControlNotificationKind;
struct Controller {
    struct Call { unsigned ep; DWC3::Command cmd; uint32_t options; };
    std::vector<Call> calls;
    Result startResult = Result::Success, endResult = Result::Pending, stallResult = Result::Success;
    bool halted = false;
    unsigned endpointCount() const { return 16; }
    unsigned dmaBits() const { return 40; }
    bool stopped() const { return halted; }
    Result startTransfer(unsigned ep, uint64_t address, DWC3::TRBType, uint32_t &reply) {
        return command(ep, DWC3::Command::Start, uint32_t(address >> 32), uint32_t(address), 0, 0, reply);
    }
    Result command(unsigned ep, DWC3::Command cmd, uint32_t, uint32_t, uint32_t,
                   uint32_t options, uint32_t &reply) {
        calls.push_back({ep,cmd,options});
        reply = ep ? 3u << 16 : 0; // resource 0 is valid for EP0 OUT
        if (cmd == DWC3::Command::Start) return startResult;
        if (cmd == DWC3::Command::End) return endResult;
        return stallResult;
    }
};
struct Order {
    unsigned reads = 0, writes = 0;
    void dmaReadBarrier() { ++reads; }
    void dmaWriteBarrier() { ++writes; }
};
DWC3::Event completed(unsigned ep) { return {ep << 1 | 1u << 6}; }
DWC3::Event ended(unsigned ep) { return {ep << 1 | 7u << 6 | 8u << 24}; }
DWC3::Event ready(unsigned ep, unsigned phase) { return {ep << 1 | 3u << 6 | phase << 12}; }

struct Fixture {
    Controller controller;
    Order order;
    alignas(16) std::array<DWC3::TRB,4> out{}, in{};
    alignas(8) std::array<uint8_t,8> setup{};
    DWC3::Control<Controller,Order> control{controller,order};
    DWC3::ControlNotification notification{};
    DWC3::ControlMemory memory{out.data(),in.data(),0x10000,0x11000,4,setup.data(),0x12000};
    Fixture() {
        assert(control.initialize(memory,64,8)==Result::Success);
        assert(control.start()==Result::Success && control.currentState()==ControlState::Setup);
        assert(controller.calls.back().ep==0);
    }
    void markComplete(unsigned ep, unsigned count=1, uint32_t remaining=0, unsigned status=0) {
        auto &trbs = ep ? in : out;
        for(unsigned i=0;i<count;++i){
            trbs[i].control &= ~DWC3::Owned;
            trbs[i].size = i+1==count ? remaining | status << 28 : 0;
        }
    }
    uint64_t receiveSetup(bool direction, uint16_t length, uint8_t request=6) {
        setup={uint8_t(direction?0x80:0),request,0,1,0,0,uint8_t(length),uint8_t(length>>8)};
        markComplete(0);
        assert(control.handle(completed(0),8,notification)==Result::Success);
        assert(notification.kind==Kind::Setup && notification.setup.length==length);
        assert(control.currentState()==ControlState::Dispatch);
        return notification.transaction;
    }
    Result event(DWC3::Event e) { return control.handle(e,8,notification); }
};

static void noData() {
    Fixture f;
    auto token=f.receiveSetup(false,0,5);
    assert(f.event(ready(1,2))==Result::Pending);
    assert(f.notification.kind==Kind::None && f.controller.calls.size()==1);
    assert(f.control.allowStatus(token+1)==Result::Invalid);
    assert(f.control.allowStatus(token)==Result::Success && f.control.currentState()==ControlState::Status);
    assert(((f.in[0].control>>4)&63)==unsigned(DWC3::TRBType::Status2));
    assert(f.controller.calls.back().ep==1);
    f.markComplete(1);
    assert(f.event(completed(1))==Result::Success);
    assert(f.notification.kind==Kind::Status && f.notification.transaction==token);
    assert(f.control.currentState()==ControlState::Setup && f.control.ownsBuffers());
    assert(f.control.allowStatus(token)==Result::Invalid);
    // wLength=0 always has an IN status, irrespective of bmRequestType.
    token=f.receiveSetup(true,0,1);
    assert(f.control.allowStatus(token)==Result::Pending);
    assert(f.event(ready(1,2))==Result::Success);
    assert(f.controller.calls.back().ep==1);
}

static void readData() {
    Fixture f;
    auto token=f.receiveSetup(true,255);
    DWC3::DMASegment data{0x20000,64};
    // Queue DATA immediately; do not wait for XferNotReady(DATA), which
    // would deadlock on a back-to-back SETUP replacement.
    assert(f.control.supplyData(token,&data,1,64)==Result::Success);
    assert(f.control.currentState()==ControlState::Data && f.controller.calls.back().ep==1);
    assert((f.in[0].control&DWC3::Chain) && (f.in[1].control&DWC3::Last));
    assert(f.in[1].size==0); // short response of an exact MPS requires a ZLP
    assert(f.control.allowStatus(token)==Result::Pending);
    assert(f.event(ready(1,1))==Result::Pending);
    f.markComplete(1,2);
    assert(f.event(completed(1))==Result::Success);
    assert(f.notification.kind==Kind::Data && f.notification.bytes==64);
    assert(f.control.currentState()==ControlState::WaitingStatus);
    assert(f.event(ready(0,2))==Result::Success && f.controller.calls.back().ep==0);
    assert(((f.out[0].control>>4)&63)==unsigned(DWC3::TRBType::Status3));
    f.markComplete(0);
    assert(f.event(completed(0))==Result::Success && f.notification.kind==Kind::Status);
    auto old=token;
    token=f.receiveSetup(true,1);
    assert(token!=old && f.control.supplyData(old,nullptr,0,0)==Result::Invalid);
    assert(f.control.supplyData(token,nullptr,0,0)==Result::Success); // empty response is a data ZLP
    assert(f.in[0].size==0 && (f.in[0].control&DWC3::Last));
}

static void writeData() {
    Fixture f;
    auto token=f.receiveSetup(false,100);
    DWC3::DMASegment data{0x20000,128};
    assert(f.control.supplyData(token,&data,1,99)==Result::Invalid);
    assert(f.control.supplyData(token,&data,1,100)==Result::Success);
    assert(f.controller.calls.back().ep==0); // OUT uses resource zero
    f.markComplete(0,1,78); // received 50 bytes, host terminated with a short packet
    assert(f.event(completed(0))==Result::Success);
    assert(f.notification.kind==Kind::Data && f.notification.bytes==50);
    assert(f.event(ready(1,2))==Result::Pending); // wait for Family to consume data
    assert(f.control.currentState()==ControlState::WaitingStatus);
    assert(f.control.allowStatus(token)==Result::Success);
    f.markComplete(1);
    assert(f.event(completed(1))==Result::Success && f.notification.kind==Kind::Status);
}

static void replacementSetup() {
    Fixture f;
    auto token=f.receiveSetup(true,64);
    DWC3::DMASegment data{0x20000,64};
    assert(f.control.supplyData(token,&data,1,64)==Result::Success);
    f.markComplete(1,1,64,2); // TRBSTS_SETUP_PENDING
    assert(f.event(completed(1))==Result::Success);
    assert(f.notification.kind==Kind::Aborted && f.notification.result==Result::SetupPending);
    assert(f.notification.transaction==token && f.control.currentTransaction()==0);
    assert(f.control.allowStatus(token)==Result::Invalid);
    assert(f.event(ready(0,2))==Result::Success);
    assert(f.notification.kind==Kind::None); // do not complete the old request twice
    assert(f.control.currentState()==ControlState::Setup);
    auto &calls=f.controller.calls;
    assert(calls[calls.size()-2].cmd==DWC3::Command::SetStall && calls[calls.size()-2].ep==0);
    token=f.receiveSetup(false,0);
    assert(f.control.allowStatus(token)==Result::Pending && f.event(ready(1,2))==Result::Success);
    f.markComplete(1,1,0,2);
    assert(f.event(completed(1))==Result::Success && f.notification.kind==Kind::Aborted);
    assert(f.control.currentState()==ControlState::Setup);
}

static void cancellationAndFaults() {
    Fixture f;
    auto token=f.receiveSetup(false,64);
    DWC3::DMASegment data{0x20000,64};
    assert(f.control.supplyData(token,&data,1,64)==Result::Success);
    assert(f.event(ready(1,1))==Result::Pending); // wrong direction: cancel first
    assert(f.control.currentState()==ControlState::Recovering && f.control.ownsBuffers());
    assert(f.controller.calls.back().cmd==DWC3::Command::End);
    assert((f.controller.calls.back().options>>16)==0); // accepted zero resource index
    assert(f.notification.kind==Kind::None);
    f.markComplete(0);
    assert(f.event(completed(0))==Result::Pending && f.notification.kind==Kind::None);
    assert(f.event(ended(0))==Result::Success && f.notification.kind==Kind::Aborted);
    assert(f.notification.transaction==token && f.control.currentState()==ControlState::Setup);
    assert(f.control.stop(f.notification,Result::Timeout)==Result::Pending);
    assert(f.event(ended(0))==Result::Success && f.control.currentState()==ControlState::Stopped);
    assert(!f.control.ownsBuffers());
    assert(f.control.start()==Result::Success);
    token=f.receiveSetup(true,64);
    f.controller.startResult=Result::Timeout;
    assert(f.control.supplyData(token,&data,1,64)==Result::Timeout);
    assert(f.control.currentState()==ControlState::Fault && f.control.ownsBuffers());
    assert(f.control.resetAfterHalt(f.notification)==Result::NotReady);
    f.controller.halted=true;
    assert(f.control.resetAfterHalt(f.notification)==Result::Success);
    assert(f.notification.kind==Kind::Aborted && f.notification.transaction==token && !f.control.ownsBuffers());
    assert(f.control.currentState()==ControlState::Stopped);
    f.controller.startResult=Result::Success;f.controller.halted=false;
    assert(f.control.start()==Result::Success);
    f.markComplete(0);
    assert(f.control.handle(completed(0),7,f.notification)==Result::Invalid);
    assert(f.control.currentState()==ControlState::Setup && f.notification.kind==Kind::None);
}

static void malformedAndShortIN() {
    Fixture f;
    auto token=f.receiveSetup(true,64);
    DWC3::DMASegment data{0x20000,64};
    assert(f.control.supplyData(token,&data,1,65)==Result::Invalid);
    assert(f.control.supplyData(token,&data,1,64)==Result::Success);
    f.markComplete(1,1,1); // IN must send the complete response supplied by Family
    assert(f.event(completed(1))==Result::Success && f.notification.kind==Kind::Aborted);
    assert(f.notification.result==Result::HardwareError && f.control.currentState()==ControlState::Setup);
    auto count=f.controller.calls.size();
    assert(f.control.handle(completed(2),8,f.notification)==Result::Invalid);
    assert(f.controller.calls.size()==count);

    Controller c;Order o;alignas(16) std::array<DWC3::TRB,4> out{},in{};
    alignas(8) std::array<uint8_t,8> setup{};
    DWC3::Control<Controller,Order> control(c,o);
    DWC3::ControlMemory memory{out.data(),in.data(),0x10000,0x10000,4,setup.data(),0x20000};
    assert(control.initialize(memory,64,1)==Result::Invalid); // DMA alias
    memory.inDMA=0x11000;memory.inTRBs=out.data();
    assert(control.initialize(memory,64,1)==Result::Invalid); // CPU alias
    memory.inTRBs=in.data();memory.setupDMA=0x11008;
    assert(control.initialize(memory,64,1)==Result::Invalid); // SETUP overlays descriptors
    memory.setupDMA=0x20000;
    assert(control.initialize(memory,128,1)==Result::Invalid);
    assert(control.initialize(memory,64,1)==Result::Success); // failed validation did not mutate either pool
}

int main() {
    noData();readData();writeData();replacementSetup();cancellationAndFaults();malformedAndShortIN();
    std::puts("DWC3 EP0: SETUP/DATA/STATUS, delayed status, short response/ZLP, setup replacement, zero resource and cancellation passed");
}
