// SPDX-License-Identifier: BSD-2-Clause
#include "BluetoothController.h"
#include <assert.h>
#include <stdio.h>
#include <vector>
#include <array>
#include <algorithm>
using namespace Bluetooth;

static const uint8_t complete[] = {Event,0x0e,4,1,3,0x0c,0};
struct Packets {
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint8_t> signals;
    Result result = Result::Success;
    Result packet(const uint8_t *p, size_t n) { frames.emplace_back(p,p + n); return result; }
    Result signal(uint8_t value) { signals.push_back(value); return result; }
};
struct Backend {
    std::vector<unsigned> acquired, released;
    std::vector<uint8_t> controls;
    std::vector<std::vector<uint8_t>> writes;
    unsigned failAcquire = 0, failRelease = 0;
    uint32_t owned = 0;
    bool clock = false;
    unsigned packets = 0;
    Result writeResult = Result::Success, controlResult = Result::Success, voteResult = Result::Success;
    Result packetResult = Result::Success;
    Result acquire(Resource r, uint64_t epoch) {
        assert(epoch); acquired.push_back(r);
        if (r == failAcquire) return Result::NotReady;
        assert(!(owned & r)); owned |= r; return Result::Success;
    }
    Result release(Resource r, uint64_t epoch) {
        assert(epoch && (owned & r)); released.push_back(r);
        if (r == failRelease) return Result::Timeout;
        owned &= ~uint32_t(r); return Result::Success;
    }
    Result write(const uint8_t *p, size_t n, uint64_t epoch) {
        assert(epoch && (owned & UART)); writes.emplace_back(p,p + n); return writeResult;
    }
    Result packet(const uint8_t *, size_t, uint64_t epoch) { assert(epoch); ++packets; return packetResult; }
    Result bootstrapPacket(const uint8_t *,size_t,uint64_t,uint64_t) { return Result::Success; }
    Result vote(bool on, uint64_t epoch) {
        assert(epoch);
        if (voteResult != Result::Success) return voteResult;
        assert(clock != on); clock = on; return Result::Success;
    }
    Result control(uint8_t byte, uint64_t epoch) { assert(epoch && clock); controls.push_back(byte); return controlResult; }
};

static void framing() {
    const std::vector<uint8_t> stream = {Event,0xff,3,0xfc,0xfd,0xfe,ACL,0x01,0x20,3,0,0xfc,0xfd,0xfe,SCO,1,0,0};
    for (size_t split = 0; split <= stream.size(); ++split) {
        H4Decoder decoder; decoder.reset(true); Packets sink;
        assert(decoder.feed(stream.data(),split,sink) == Result::Success);
        assert(decoder.feed(stream.data() + split,stream.size() - split,sink) == Result::Success);
        assert(sink.frames.size() == 3 && sink.signals.empty() && decoder.atBoundary());
        for (const auto &p : sink.frames) assert(validPacket(p.data(),p.size(),false));
    }
    H4Decoder decoder; decoder.reset(true); Packets sink;
    for (uint8_t byte : stream) assert(decoder.feed(&byte,1,sink) == Result::Success);
    const uint8_t signals[] = {WakeIndication,WakeAcknowledgement,SleepIndication};
    assert(decoder.feed(signals,3,sink) == Result::Success && sink.signals.size() == 3);
    assert(decoder.feed(nullptr,0,sink) == Result::Success);
    assert(decoder.feed(nullptr,1,sink) == Result::Invalid);
    for (uint8_t bad : {uint8_t(0),uint8_t(Command),uint8_t(5),uint8_t(0xff)}) {
        decoder.reset(); assert(decoder.feed(&bad,1,sink) == Result::Invalid);
        assert(decoder.poisoned() && decoder.feed(complete,sizeof(complete),sink) == Result::Fault);
    }
    const uint8_t oversized[] = {ACL,1,0,1,0x10};
    decoder.reset(); assert(decoder.feed(oversized,sizeof(oversized),sink) == Result::Invalid);
    std::vector<uint8_t> maximum(MaxH4Frame,0xfc);
    maximum[0] = ACL; maximum[1] = 1; maximum[2] = 0;
    write16(maximum.data() + 3,MaxACLPayload);
    decoder.reset(true);
    assert(decoder.feed(maximum.data(),maximum.size(),sink) == Result::Success);
    assert(sink.frames.back().size() == MaxH4Frame && decoder.atBoundary());
    decoder.reset(); const size_t before = sink.frames.size();
    assert(decoder.feed(complete,sizeof(complete) - 1,sink) == Result::Success && !decoder.atBoundary());
    assert(sink.frames.size() == before);
    assert(decoder.feed(complete + sizeof(complete) - 1,1,sink) == Result::Success);
    decoder.reset(); sink.result = Result::Fault;
    assert(decoder.feed(complete,sizeof(complete),sink) == Result::Fault && decoder.poisoned());
    assert(!validPacket(complete,sizeof(complete),true));
    assert(validPacket(ResetCommand,sizeof(ResetCommand),true));
    assert(!validPacket(ResetCommand,sizeof(ResetCommand),false));
}

static void transactions() {
    ResetTransaction command; command.reset(1);
    assert(command.begin(2,0,2000) == Result::NotReady);
    assert(command.begin(1,UINT64_MAX,1) == Result::Invalid);
    assert(command.begin(1,100,2000) == Result::Success);
    assert(command.begin(1,100,2000) == Result::Busy);
    uint8_t event[sizeof(complete)]; std::copy(std::begin(complete),std::end(complete),event);
    event[4] = 4;
    assert(command.receive(1,101,event,sizeof(event)) == Result::Ignored);
    const uint8_t status[] = {Event,0x0f,4,0,1,3,0x0c};
    assert(command.receive(1,102,status,sizeof(status)) == Result::Pending && command.pending());
    assert(command.receive(2,102,complete,sizeof(complete)) == Result::Ignored);
    assert(command.receive(1,2099,complete,sizeof(complete)) == Result::Success);
    assert(command.begin(1,2100,2000) == Result::Fault);
    command.reset(2); assert(command.begin(2,0,100) == Result::Success);
    assert(command.receive(2,100,complete,sizeof(complete)) == Result::Timeout);
    assert(command.receive(2,101,complete,sizeof(complete)) == Result::Ignored);
    assert(command.begin(2,101,100) == Result::Fault);
    command.reset(3); assert(command.begin(3,0,100) == Result::Success);
    event[4] = 3; event[6] = 1;
    assert(command.receive(3,1,event,sizeof(event)) == Result::Fault);
}

static void lifecycle() {
    const std::array<unsigned,6> order = {Rails,ReferenceClock,Pins,UART,BootROM,Firmware};
    for (size_t failure = 0; failure < order.size(); ++failure) {
        Backend backend; backend.failAcquire = order[failure]; Controller<Backend> c(backend);
        c.attach(); assert(c.start(0) == Result::NotReady && c.currentState() == State::Off);
        assert(!c.resources() && !backend.owned && c.epoch() == 1);
        assert(backend.acquired.size() == failure + 1 && backend.released.size() == failure);
        for (size_t i = 0; i < failure; ++i) assert(backend.released[i] == order[failure - i - 1]);
    }
    for (size_t failure = 0; failure < order.size(); ++failure) {
        Backend backend; Controller<Backend> c(backend); c.attach();
        assert(c.start(0) == Result::Pending && c.currentState() == State::WaitingForReset);
        backend.failRelease = order[failure];
        assert(c.stop() == Result::Timeout && c.currentState() == State::Fault);
        uint32_t retained = 0; for (size_t i = 0; i <= failure; ++i) retained |= order[i];
        assert(c.resources() == retained && backend.owned == retained);
        assert(c.start(1) == Result::Busy);
        backend.failRelease = 0; assert(c.stop() == Result::Success && !backend.owned);
    }
    Backend backend; Controller<Backend> c(backend); c.attach();
    assert(c.start(UINT64_MAX) == Result::Invalid && !c.epoch());
    assert(c.start(0) == Result::Pending && c.resources() == AllResources);
    assert(backend.writes.size() == 1 && backend.writes[0] == std::vector<uint8_t>(ResetCommand,ResetCommand + 4));
    assert(c.receive(2,1,complete,sizeof(complete)) == Result::Ignored);
    assert(c.receive(1,1,complete,3) == Result::Success && c.currentState() == State::WaitingForReset);
    assert(c.receive(1,2,complete + 3,4) == Result::Success && c.currentState() == State::Ready);
    const uint8_t payload[] = {ACL,1,0,2,0,0xfc,0xfe};
    assert(c.receive(1,3,payload,sizeof(payload)) == Result::Success && backend.packets == 1);
    const uint8_t wake = WakeIndication;
    assert(c.receive(1,4,&wake,1) == Result::Success && backend.clock);
    assert(backend.controls.back() == WakeAcknowledgement);
    assert(c.stop() == Result::Success && !backend.clock && !backend.owned);
    assert(c.start(10) == Result::Pending && c.epoch() == 2);
    assert(c.receive(1,11,complete,sizeof(complete)) == Result::Ignored);
    assert(c.tick(2,2010) == Result::Timeout && c.resources() == AllResources);
    assert(c.receive(2,2011,complete,sizeof(complete)) == Result::Ignored);
    assert(c.stop() == Result::Success);
    backend.writeResult = Result::Fault;
    assert(c.start(3000) == Result::Fault && c.currentState() == State::Off && !backend.owned);
    assert(c.detach() == Result::Success && c.currentState() == State::Detached);
    Backend rollback; rollback.failAcquire = Firmware; rollback.failRelease = UART;
    Controller<Backend> r(rollback); r.attach();
    assert(r.start(0) == Result::Timeout && r.currentState() == State::Fault);
    assert(r.resources() == (Rails | ReferenceClock | Pins | UART));
    rollback.failRelease = 0; assert(r.detach() == Result::Success);
    Backend malformed; Controller<Backend> m(malformed); m.attach(); assert(m.start(0) == Result::Pending);
    const uint8_t unknown = 5;
    assert(m.receive(1,1,&unknown,1) == Result::Invalid && m.currentState() == State::Fault);
    assert(m.stop() == Result::Success);
    assert(m.start(10) == Result::Pending);
    assert(m.receive(2,11,complete,sizeof(complete)) == Result::Success);
    const uint8_t hardwareError[] = {Event,0x10,1,1};
    assert(m.receive(2,12,hardwareError,sizeof(hardwareError)) == Result::Fault);
    assert(m.currentState() == State::Fault && m.resources() == AllResources);
    assert(m.stop() == Result::Success);
}

static void sleepProtocol() {
    Backend backend; InBandSleep<Backend> ibs(backend);
    assert(ibs.start(1) == Result::Success);
    assert(ibs.requestTransmit(1,0) == Result::Pending && backend.clock && backend.controls.back() == WakeIndication);
    assert(ibs.requestTransmit(1,10) == Result::Pending && backend.controls.size() == 1);
    assert(ibs.signal(2,WakeAcknowledgement) == Result::NotReady);
    assert(ibs.signal(1,WakeAcknowledgement) == Result::Success);
    assert(ibs.requestTransmit(1,20) == Result::Success);
    assert(ibs.tick(1,10000) == Result::Success && backend.controls.size() == 1); // no drain yet
    assert(ibs.transmitDrained(1,10000) == Result::Success);
    assert(ibs.signal(1,WakeIndication) == Result::Success && ibs.receiveAwake());
    assert(ibs.tick(1,12000) == Result::Success && ibs.transmitState() == InBandSleep<Backend>::TX::Asleep);
    assert(backend.clock); // peer still awake
    assert(ibs.signal(1,SleepIndication) == Result::Success && !backend.clock);
    assert(ibs.quiesce(false) == Result::Busy);
    assert(ibs.quiesce(true) == Result::Success);
    assert(ibs.start(2) == Result::Success && ibs.requestTransmit(2,0) == Result::Pending);
    assert(ibs.tick(2,100) == Result::Success && ibs.tick(2,200) == Result::Success);
    assert(ibs.tick(2,300) == Result::Timeout && ibs.faulted() && ibs.ownsClock());
    assert(ibs.signal(2,WakeAcknowledgement) == Result::Fault);
    backend.voteResult = Result::Fault;
    assert(ibs.quiesce(true) == Result::Fault && ibs.ownsClock());
    backend.voteResult = Result::Success; assert(ibs.quiesce(true) == Result::Success);
    assert(ibs.start(3) == Result::Success && ibs.requestTransmit(3,UINT64_MAX) == Result::Invalid && !backend.clock);
    backend.controlResult = Result::Fault;
    assert(ibs.requestTransmit(3,0) == Result::Fault && backend.clock);
    assert(ibs.quiesce(true) == Result::Success && !backend.clock);
}

struct AsyncBackend : Backend {
    Resource blockedRelease=Rails;
    unsigned waitAt=0,boots=0;
    bool block=false,ambiguous=false;
    Result acquire(Resource r,uint64_t epoch) {
        acquired.push_back(r); assert(epoch);
        if(!(owned&r)) {
            owned|=r;waitAt=r;
            return ambiguous?Result::Timeout:Result::Pending;
        }
        assert(waitAt==r);return Result::Success;
    }
    Result release(Resource r,uint64_t epoch) {
        if(block && r==blockedRelease){assert(owned&r);return Result::Pending;}
        return Backend::release(r,epoch);
    }
    Result bootstrapPacket(const uint8_t *,size_t,uint64_t,uint64_t){++boots;return Result::Success;}
};
static void asynchronousLifecycle() {
    const Resource stages[]={Rails,ReferenceClock,Pins,UART,BootROM,Firmware};
    for(unsigned cancel=0;cancel<ResourceCount;++cancel) {
        AsyncBackend backend;Controller<AsyncBackend> c(backend);c.attach();
        assert(c.start(0)==Result::Pending);
        uint32_t prefix=stages[0];
        for(unsigned i=1;i<=cancel;++i) {
            assert(c.tick(c.epoch(),i)==Result::Pending);prefix|=stages[i];
        }
        assert(c.currentState()==State::Starting && c.resources()==prefix && backend.owned==prefix);
        if(cancel>=3) {
            backend.block=true;backend.blockedRelease=UART;
            assert(c.stop()==Result::Pending && c.currentState()==State::Stopping);
            assert(c.resources()==(Rails|ReferenceClock|Pins|UART));
            assert(c.receive(c.epoch(),100,complete,sizeof(complete))==Result::Ignored);
            assert(c.start(100)==Result::Busy);
            assert(c.tick(c.epoch(),101)==Result::Pending);
            backend.block=false;
            assert(c.tick(c.epoch(),102)==Result::Success);
        } else assert(c.stop()==Result::Success);
        assert(!c.resources() && !backend.owned && c.currentState()==State::Off);
        assert(backend.writes.empty()); // no Reset until firmware completed
    }
    AsyncBackend backend;Controller<AsyncBackend> c(backend);c.attach();
    assert(c.start(0)==Result::Pending);
    for(unsigned i=1;i<ResourceCount;++i)assert(c.tick(c.epoch(),i)==Result::Pending);
    assert(c.currentState()==State::Starting && c.resources()==AllResources);
    assert(c.receive(c.epoch(),10,complete,sizeof(complete))==Result::Success && backend.boots==1);
    assert(c.tick(c.epoch(),11)==Result::Pending && c.currentState()==State::WaitingForReset);
    assert(c.receive(c.epoch(),12,complete,sizeof(complete))==Result::Success && c.currentState()==State::Ready);
    backend.block=true;backend.blockedRelease=UART;
    assert(c.detach()==Result::Pending);
    backend.block=false;
    assert(c.tick(c.epoch(),13)==Result::Success && c.currentState()==State::Detached);
    // An acquisition timeout may own hardware, even before its first success.
    AsyncBackend timed;timed.ambiguous=true;timed.block=true;timed.blockedRelease=Rails;
    Controller<AsyncBackend> t(timed);t.attach();
    assert(t.start(0)==Result::Timeout && t.currentState()==State::Stopping && t.resources()==Rails);
    timed.block=false;assert(t.tick(t.epoch(),1)==Result::Success && !timed.owned);
}

int main() {
    framing(); transactions(); lifecycle(); asynchronousLifecycle(); sleepProtocol();
    for (unsigned selector = 0; selector < 6; ++selector) for (unsigned inputs = 0; inputs < 10; ++inputs)
        for (unsigned outputs = 0; outputs < 14; ++outputs) {
            const bool expected = ((selector == GetInfo || selector == GetStatus) && !inputs && outputs == 8) ||
                (selector == SetPower && inputs == 1 && !outputs) ||
                (selector == GetBootstrap && !inputs && outputs == BootstrapScalars) ||
                (selector == BootstrapSetPower && inputs == 2 && outputs == 1);
            assert(validCall(selector,inputs,outputs) == expected);
        }
    assert(!validCall(SetPower,1,0,2));
    puts("Bluetooth H4, reset, resource rollback, generations and IBS passed");
}
