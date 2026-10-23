// SPDX-License-Identifier: BSD-2-Clause
#include "XHCIRequests.h"
#include "XHCITTClear.h"
#include "usb_memory_model.h"
#include <cstdio>
#include <map>
#include <memory>

namespace {
struct Order { void dmaReadBarrier() {} void dmaWriteBarrier() {} };
struct CommandBackend {
    Order order;alignas(64) XHCI::TRB data[64]{};
    XHCI::ProducerRing<Order> producer{order};
    CommandBackend() { assert(producer.initialize(data,0x100000,64,36)); }
    auto &ring() { return producer; }
    bool admitted(uint64_t epoch) { return epoch==7; }
    void retain(uintptr_t) {}
    Result doorbell() { return Result::Success; }
    Result isolation(uint64_t) { return Result::Success; }
};
struct TransferBackend {
    Order order;alignas(64) XHCI::TRB data[256]{};
    XHCI::ProducerRing<Order> producer{order};
    bool isolated=false;
    Result bell=Result::Success;
    std::map<uintptr_t,unsigned> refs;
    explicit TransferBackend(uint64_t base=0x200000) { assert(producer.initialize(data,base,256,36)); }
    auto &ring() { return producer; }
    bool admitted(uint64_t epoch) { return epoch==7; }
    void retain(uintptr_t owner) { ++refs[owner]; }
    Result doorbell(unsigned slot,unsigned endpoint) { assert(slot && slot<=32 && endpoint);return bell; }
    Result isolation(uint64_t epoch) { return isolated && epoch==7 ? Result::Success : Result::Busy; }
    void release(uintptr_t owner) { assert(refs[owner]);if (!--refs[owner]) refs.erase(owner); }
};
using Commands=XHCI::CommandQueue<CommandBackend>;
using Queue=XHCI::TransferQueue<TransferBackend,Commands>;
using Request=XHCI::RequestMemory<MemoryAPI>;
struct Machine {
    MemoryAPI api;
    MemoryAPI::Memory *client;
    CommandBackend cb;Commands commands{cb};
    TransferBackend tb;Queue queue{tb,commands};
    XHCI::EndpointConfiguration configuration;
    unsigned length;
    explicit Machine(bool input=true,bool control=false,unsigned bytes=1024,USB::Speed speed=USB::Speed::High,
        bool interrupt=false) : length(bytes) {
        api.bus=0x800000;client=api.allocate(bytes ? bytes : 64,64);
        assert(api.prepare(client,input,!input)==Result::Success);
        std::memset(client->data,0x25,client->size);
        configuration.address=control ? 0 : input ? 0x81 : 1;
        configuration.type=control ? XHCI::EndpointType::Control : interrupt ? XHCI::EndpointType::Interrupt:XHCI::EndpointType::Bulk;
        configuration.interval=interrupt ? 1:0;
        configuration.speed=speed;configuration.maxPacket=control || speed==USB::Speed::Full ? 64 : 512;
        configuration.dequeue=0x200000;
        assert(commands.initialize(7,32,32,36)==Result::Success);
        assert(queue.initialize(7,3,configuration,36,0xee)==Result::Success);
    }
    XHCI::TRB event(uint64_t ticket,unsigned bytes,bool final=true,unsigned code=1) {
        return XHCI::trb(ticket<<1|uint64_t(final),code<<24|bytes,XHCI::TransferEvent<<10|XHCI::EventDataFlag|
            3u<<24|XHCI::endpointID(configuration.address,configuration.type)<<16);
    }
    uint64_t issue(XHCI::Type type,uint64_t parameter=0) {
        XHCI::Command command;command.type=type;command.slot=3;
        command.endpoint=XHCI::endpointID(configuration.address,configuration.type);command.parameter=parameter;
        uint64_t ticket=0;assert(commands.submit(command,0xee,1,1000,ticket)==Result::Success);
        XHCI::CommandRecord record{},done{};assert(commands.copy(ticket,record));
        assert(commands.complete(XHCI::trb(record.address,1u<<24,XHCI::CommandEvent<<10|3u<<24),7,done)==Result::Success);
        return ticket;
    }
    void finishNative() {
        assert(api.memories.size()==1 && api.commands.empty() && client->references==1 && client->prepares==1);
        assert(tb.refs.empty());assert(api.complete(client)==Result::Success);api.releaseMemory(client);
    }
};
// External Family peer: kind3 changes completion grouping, not the submitted
// MD/TD. Only this peer completes Family preparation and invokes callbacks.
struct NativeRequest {
    Machine &machine;
    Request &dma;
    unsigned identifier,group[3];
    bool bundled=true,handed=false,completed=false,detached=false;
    Result status=Result::Pending;
    uint32_t bytes=UINT32_MAX;
    uint64_t stamp=0;
    NativeRequest(Machine &m,Request &memory,unsigned id) : machine(m),dma(memory),identifier(id),
        group{1,1,1} {machine.api.retainMemory(machine.client);}
    void setStatus(Result value) {assert(!handed);status=value;}
    void setBytesTransferred(uint32_t value) {assert(!handed);bytes=value;}
    void setTimeStamp(uint64_t value) {assert(!handed);stamp=value;}
};
struct NativeCompleter {
    struct Record {unsigned identifier;Result status;uint32_t bytes;uint64_t stamp;};
    struct Callback {unsigned group[3];std::vector<Record> records;};
    std::vector<NativeRequest *> pending;
    std::vector<Callback> callbacks;
    bool immediate=false,uncertainEnqueue=false;
    unsigned enqueues=0;
    void drain() {
        while(!pending.empty()) {
            const auto *first=pending.front();Callback callback{{first->group[0],first->group[1],first->group[2]}, {}};
            const bool bundled=first->bundled;
            while(!pending.empty() && callback.records.size()<16) {
                auto &request=*pending.front();
                if(!callback.records.empty() && (!bundled || !request.bundled ||
                    request.group[0]!=callback.group[0] || request.group[1]!=callback.group[1] ||
                    request.group[2]!=callback.group[2]))break;
                auto &m=request.machine;
                assert(request.handed && !request.completed && request.detached);
                assert(request.dma.currentState()==DMAState::Empty && m.tb.refs.empty() && m.api.commands.empty());
                assert(m.client->prepares==1 && m.client->references==2);
                if(m.length && (m.configuration.address&128)) {
                    const auto *data=static_cast<const uint8_t *>(m.client->data);
                    if(request.status==Result::Success && request.bytes) {
                        assert(data[request.bytes-1]==0x72);
                        if(request.bytes<m.length)assert(data[request.bytes]==0x25);
                    } else assert(data[0]==0x25);
                }
                assert(m.api.complete(m.client)==Result::Success);m.api.releaseMemory(m.client);
                request.completed=true;
                callback.records.push_back({request.identifier,request.status,request.bytes,request.stamp});
                pending.erase(pending.begin());
            }
            callbacks.push_back(callback);
        }
    }
    Result enqueueRequest(NativeRequest *request) {
        assert(request && !request->handed && request->detached && request->stamp && request->bytes!=UINT32_MAX);
        assert(request->dma.currentState()==DMAState::Empty && request->machine.tb.refs.empty());
        ++enqueues;request->handed=true;pending.push_back(request);
        if(immediate)drain(); // Reuse/cleanup can precede enqueue's return.
        return uncertainEnqueue ? Result::HardwareError:Result::Success;
    }
};
void nativeBundledCompletions() {
    for(bool input:{false,true})for(bool immediate:{false,true})for(bool mixed:{false,true})
        for(unsigned count:{1u,16u,17u,33u}) {
        NativeCompleter completer;completer.immediate=immediate;
        std::vector<std::unique_ptr<Machine>> machines;
        std::vector<std::unique_ptr<Request>> memory;
        std::vector<std::unique_ptr<NativeRequest>> native;
        std::map<unsigned,NativeCompleter::Record> expected;
        for(unsigned i=0;i<count;++i) {
            // Include ordinary callbacks, independent target/action/context
            // groups, short data, zero data, abort, timeout and isolation.
            const unsigned mode=i%6,bytes=i%7 ? 64u:0u;
            machines.emplace_back(new Machine(input,false,bytes,USB::Speed::High,i%9==8));auto &m=*machines.back();
            memory.emplace_back(new Request(m.api));auto &request=*memory.back();
            native.emplace_back(new NativeRequest(m,request,i));auto &peer=*native.back();
            if(mixed && i>=16)peer.group[(i/16-1)%3]=2;
            peer.bundled=!mixed || i%11!=10;
            assert(request.prepare(m.client,m.configuration,bytes,36)==Result::Success);
            assert(request.submit(m.queue,m.tb.ring(),0xabc,7,100,5000)==Result::Success);
            if(bytes && input)std::memset(m.api.memories.back()->data,0x72,bytes);
            uint32_t actual=mode==1 && input && bytes ? 7u:bytes;Result status=Result::Success;
            if(mode<2) {
                XHCI::TransferRecord record{};
                assert(m.queue.complete(m.event(request.ticket(),actual,true,mode==1 && input ? 13:1),7,record)==Result::Success);
            } else {
                assert((mode==3 ? m.queue.expire(5100):m.queue.quiesce())==
                    (mode==3 ? Result::Timeout:Result::Aborted));
                request.quarantine();
                assert(request.finish(false)==Result::Busy && !peer.handed && m.client->prepares==1);
                if(mode==5) {m.tb.isolated=true;assert(m.queue.isolate()==Result::Success);}
                else {
                    const auto stop=m.issue(XHCI::StopEndpoint);
                    const auto dequeue=m.issue(XHCI::SetDequeue,m.tb.ring().nextAddress()|m.tb.ring().producerCycle());
                    assert(m.queue.recover(stop,dequeue)==Result::Success);
                }
                actual=0;status=mode==3 ? Result::Timeout:Result::Aborted;
            }
            uintptr_t owner=0;assert(request.retire(m.queue,owner)==Result::Success && owner==0xabc);
            assert(request.finish(status==Result::Success)==Result::Success);
            m.tb.release(owner);peer.detached=true;
            const uint64_t stamp=1000+i;
            assert(XHCI::handoffRequest(peer,completer,status,actual,stamp)==Result::Success);
            expected.emplace(i,NativeCompleter::Record{i,status,actual,stamp});
        }
        completer.drain();assert(completer.enqueues==count && completer.pending.empty());
        if(!immediate && !mixed)assert(completer.callbacks.size()==(count+15)/16);
        unsigned delivered=0;
        for(const auto &callback:completer.callbacks) {
            assert(!callback.records.empty() && callback.records.size()<=16);
            for(const auto &record:callback.records) {
                const auto wanted=expected.at(record.identifier);auto &peer=*native[record.identifier];
                assert(record.status==wanted.status && record.bytes==wanted.bytes && record.stamp==wanted.stamp);
                for(unsigned n=0;n<3;++n)assert(peer.group[n]==callback.group[n]);
                assert(peer.completed && peer.machine.client->prepares==0 && peer.machine.client->references==1);
                expected.erase(record.identifier);++delivered;
            }
        }
        assert(delivered==count && expected.empty());
        for(auto &m:machines){m->api.releaseMemory(m->client);assert(m->api.memories.empty());}
    }
    // A failed enqueue may already have consumed/reused the Native object.
    // The caller must pin it; no second handoff or post-enqueue setters.
    for(bool immediate:{false,true}) {
        Machine m;Request request(m.api);NativeRequest peer(m,request,0);NativeCompleter completer;
        assert(request.prepare(m.client,m.configuration,1024,36)==Result::Success && request.finish(false)==Result::Success);
        peer.detached=true;completer.immediate=immediate;completer.uncertainEnqueue=true;
        assert(XHCI::handoffRequest(peer,completer,Result::Aborted,0,1000)==Result::HardwareError);
        assert(completer.enqueues==1 && peer.handed);completer.drain();
        assert(peer.completed && completer.enqueues==1);m.api.releaseMemory(m.client);assert(m.api.memories.empty());
    }
    puts("Native bundled completion:32 ordinary/interrupt IN/OUT, immediate/deferred and grouping cases,536 real TD retirements,16-request callback boundary and2 uncertain enqueues passed");
}
void transferAndFamilyOwnership() {
    for (bool input:{false,true}) for (unsigned bytes:{0u,64u,1024u}) {
        Machine m(input,false,bytes);Request request(m.api);
        assert(request.prepare(m.client,m.configuration,bytes,36)==Result::Success);
        assert(m.client->references==2 && m.client->prepares==1);
        if (bytes && !input) assert(static_cast<uint8_t *>(m.api.memories.back()->data)[0]==0x25);
        assert(request.submit(m.queue,m.tb.ring(),0xabc,7,100,5000)==Result::Success);
        assert(request.finish(true)==Result::Busy && request.ticket());
        if (bytes && input) std::memset(m.api.memories.back()->data,0x72,bytes);
        XHCI::TransferRecord record{};
        assert(m.queue.complete(m.event(request.ticket(),bytes),7,record)==Result::Success);
        uintptr_t owner=0;
        assert(request.retire(m.queue,owner)==Result::Success && owner==0xabc);
        assert(request.retire(m.queue,owner)==Result::Invalid && !owner);
        assert(request.finish(true)==Result::Success && request.validData() && request.transferred()==bytes);
        assert(m.client->prepares==1 && m.client->references==1);
        if (bytes && input) assert(static_cast<uint8_t *>(m.client->data)[bytes-1]==0x72);
        m.tb.release(0xabc);m.finishNative();
    }
}
void controlShortAndStatus() {
    Machine m(true,true,64);Request request(m.api);USB::SetupPacket setup{0x80,6,0x100,0,64};
    assert(request.prepare(m.client,m.configuration,64,36,&setup)==Result::Success);
    assert(request.submit(m.queue,m.tb.ring(),0xabc,7,100,5000)==Result::Success);
    std::memset(m.api.memories.back()->data,0x91,9);
    XHCI::TransferRecord record{};
    assert(m.queue.complete(m.event(request.ticket(),9,false,13),7,record)==Result::Pending);
    uintptr_t owner=0;assert(request.retire(m.queue,owner)==Result::Busy && !owner);
    assert(request.finish(true)==Result::Busy && m.client->prepares==1);
    assert(m.queue.complete(m.event(request.ticket(),0),7,record)==Result::Success);
    assert(request.retire(m.queue,owner)==Result::Success && owner==0xabc);
    assert(request.finish(true)==Result::Success && request.transferred()==9);
    assert(static_cast<uint8_t *>(m.client->data)[8]==0x91 && static_cast<uint8_t *>(m.client->data)[9]==0x25);
    m.tb.release(owner);m.finishNative();
}
void nativeErrorRetainsPreparedFIFO() {
    for (bool control:{false,true}) {
        Machine m(true,control,64);Request first(m.api),second(m.api);
        USB::SetupPacket setup{0x80,6,0x100,0,64};
        assert(first.prepare(m.client,m.configuration,64,36,control ? &setup : nullptr)==Result::Success);
        assert(second.prepare(m.client,m.configuration,64,36,control ? &setup : nullptr)==Result::Success);
        assert(m.client->prepares==1 && m.client->references==3);
        assert(first.submit(m.queue,m.tb.ring(),0xabc,7,100,5000)==Result::Success);
        XHCI::TransferRecord record;
        if (control) assert(m.queue.complete(m.event(first.ticket(),9,false,13),7,record)==Result::Pending);
        assert(m.queue.complete(m.event(first.ticket(),0,true,36),7,record)==Result::HardwareError);
        first.quarantine();uintptr_t owner=0;
        assert(first.retire(m.queue,owner)==Result::Busy && first.finish(false)==Result::Busy);
        assert(m.client->prepares==1 && m.client->references==3 && m.api.memories.size()==3);
        const auto stop=m.issue(XHCI::ResetEndpoint);
        const auto dq=m.issue(XHCI::SetDequeue,m.tb.ring().nextAddress()|m.tb.ring().producerCycle());
        assert(m.queue.recover(stop,dq)==Result::Success);
        assert(first.retire(m.queue,owner)==Result::Success && owner==0xabc);
        assert(first.finish(false)==Result::Success && !first.validData());m.tb.release(owner);
        assert(m.client->prepares==1 && m.client->references==2 && m.api.memories.size()==2);
        assert(second.submit(m.queue,m.tb.ring(),0xdef,7,101,5000)==Result::Busy && !second.ticket());
        assert(second.currentState()==DMAState::Ready && m.client->references==2);
        assert(m.queue.beginNativeClear()==Result::Success && m.queue.finishNativeClear()==Result::Success);
        assert(second.submit(m.queue,m.tb.ring(),0xdef,7,102,5000)==Result::Success);
        if (control) assert(m.queue.complete(m.event(second.ticket(),64,false),7,record)==Result::Pending);
        assert(m.queue.complete(m.event(second.ticket(),control ? 0 : 64),7,record)==Result::Success);
        assert(second.retire(m.queue,owner)==Result::Success && owner==0xdef);
        assert(second.finish(true)==Result::Success);m.tb.release(owner);m.finishNative();
    }
}
void ttBetweenRealStopAndDequeue() {
    // Native error, ordinary abort, completion timeout and close. Only the
    // first has a Native error latch; every published FS control/bulk TD can
    // require the same real TT maintenance before its dequeue is advanced.
    for (unsigned reason=0;reason<4;++reason) for (bool control:{false,true}) {
        Machine child(true,control,64,USB::Speed::Full);Request failed(child.api);
        USB::SetupPacket dataSetup{0x80,6,0x100,0,64};
        assert(failed.prepare(child.client,child.configuration,64,36,control ? &dataSetup : nullptr)==Result::Success);
        assert(failed.submit(child.queue,child.tb.ring(),0xabc,7,100,5000)==Result::Success);
        XHCI::TransferRecord record;
        if (!reason) assert(child.queue.complete(child.event(failed.ticket(),0,true,36),7,record)==Result::HardwareError);
        else if (reason==2) assert(child.queue.expire(5100)==Result::Timeout);
        else assert(child.queue.quiesce()==Result::Aborted);
        assert(child.queue.requiresNativeClear()==!reason);
        failed.quarantine();const auto stop=child.issue(!reason ? XHCI::ResetEndpoint : XHCI::StopEndpoint);
        XHCI::SlotConfiguration slot;slot.speed=USB::Speed::Full;slot.ttHub=7;slot.ttPort=4;
        assert(XHCI::ttCleanupRequired(slot,child.configuration));
        XHCI::TTBufferCleanup cleanup;cleanup.require();
        assert(cleanup.dirty() && !cleanup.canRelease(true,true,false));
        assert(cleanup.schedule(100) && !cleanup.schedule(100) && cleanup.start() && !cleanup.start());
        XHCI::TTClearPlan plan;
        assert(XHCI::ttClearPlan(slot,child.configuration,38,true,plan)==Result::Success);
        TransferBackend hubBackend(0x400000);Queue hub(hubBackend,child.commands);
        XHCI::EndpointConfiguration ep0;ep0.type=XHCI::EndpointType::Control;ep0.speed=USB::Speed::High;
        ep0.maxPacket=64;ep0.dequeue=0x400000;
        assert(hub.initialize(7,7,ep0,36,0xee)==Result::Success);
        for (unsigned i=0;i<plan.count;++i) {
            uintptr_t owner=0;
            assert(failed.retire(child.queue,owner)==Result::Busy && failed.finish(false)==Result::Busy);
            assert(child.client->prepares==1 && child.client->references==2);
            const auto &wire=plan.requests[i];
            USB::SetupPacket setup{wire.bmRequestType,wire.bRequest,wire.wValue,wire.wIndex,wire.wLength};
            Request maintenance(child.api);
            assert(maintenance.prepare(nullptr,ep0,0,36,&setup)==Result::Success);
            assert(maintenance.submit(hub,hubBackend.ring(),0xdef,7,101+i,500)==Result::Success);
            assert(hub.complete(XHCI::trb(maintenance.ticket()<<1|1,1u<<24,
                XHCI::TransferEvent<<10|XHCI::EventDataFlag|7u<<24|1u<<16),7,record)==Result::Success);
            assert(maintenance.retire(hub,owner)==Result::Success && owner==0xdef);
            assert(maintenance.finish(true)==Result::Success);hubBackend.release(owner);
            XHCI::CommandRecord retained;
            assert(child.commands.copy(stop,retained) && retained.phase==XHCI::CommandPhase::Completed);
            // Successful TT traffic on another actual endpoint does not
            // prove retirement of the failed child's quarantined buffer.
            assert(failed.finish(false)==Result::Busy && child.queue.requiresNativeClear()==!reason);
            assert(!cleanup.canRelease(false,true,false) && !cleanup.canRelease(false,false,true));
        }
        const auto dq=child.issue(XHCI::SetDequeue,child.tb.ring().nextAddress()|child.tb.ring().producerCycle());
        assert(child.queue.recover(stop,dq)==Result::Success);uintptr_t owner=0;
        assert(failed.retire(child.queue,owner)==Result::Success && owner==0xabc);
        assert(failed.finish(false)==Result::Success);child.tb.release(owner);
        assert(child.queue.requiresNativeClear()==!reason);
        // Wire failure can retire DMA only with the real dequeue proof; the
        // pipe stays fenced and retained through its bounded retry window.
        if (reason==3) {
            assert(cleanup.finish(false,1000) && cleanup.dirty() && !cleanup.running());
            assert(!cleanup.canRelease(false,true,false) && !cleanup.schedule(500999));
            assert(cleanup.canRelease(false,false,false) && cleanup.canRelease(false,true,true));
            assert(cleanup.schedule(501000) && cleanup.start());
        }
        assert(cleanup.finish(true,501001) && !cleanup.dirty() && cleanup.canRelease(false,true,false));
        assert(!cleanup.finish(true,501002) && !cleanup.schedule(501002));
        if (!reason) {
            assert(child.queue.beginNativeClear()==Result::Success && child.queue.finishNativeClear()==Result::Success);
        }
        child.finishNative();
    }
    XHCI::SlotConfiguration slot;slot.speed=USB::Speed::Full;slot.ttHub=7;slot.ttPort=4;
    XHCI::EndpointConfiguration endpoint;endpoint.speed=USB::Speed::Full;endpoint.type=XHCI::EndpointType::Interrupt;
    assert(!XHCI::ttCleanupRequired(slot,endpoint));endpoint.type=XHCI::EndpointType::Control;
    slot.ttHub=0;assert(!XHCI::ttCleanupRequired(slot,endpoint));
    slot.ttHub=7;slot.speed=USB::Speed::High;endpoint.speed=slot.speed;
    assert(!XHCI::ttCleanupRequired(slot,endpoint));
    XHCI::TTBufferCleanup saturated;saturated.require();assert(saturated.schedule(UINT64_MAX-1) && saturated.start());
    assert(saturated.finish(false,UINT64_MAX-1) && !saturated.schedule(UINT64_MAX-1) && saturated.schedule(UINT64_MAX));
}
void zeroLengthControlWithoutMD() {
    Machine m(false,true,0);Request request(m.api);USB::SetupPacket setup{0,9,1,0,0};
    assert(request.prepare(nullptr,m.configuration,0,36,&setup)==Result::Success);
    assert(m.api.memories.size()==1 && m.client->references==1);
    assert(request.submit(m.queue,m.tb.ring(),0xabc,7,100,5000)==Result::Success);
    XHCI::TransferRecord record{};
    assert(m.queue.complete(m.event(request.ticket(),0),7,record)==Result::Success);
    uintptr_t owner=0;assert(request.retire(m.queue,owner)==Result::Success);
    assert(request.finish(true)==Result::Success && request.transferred()==0);
    m.tb.release(owner);m.finishNative();
}
void cancellationAndIsolation() {
    for (bool reset:{false,true}) {
        Machine m;Request request(m.api);
        assert(request.prepare(m.client,m.configuration,1024,36)==Result::Success);
        assert(request.submit(m.queue,m.tb.ring(),0xabc,7,100,5000)==Result::Success);
        assert(m.queue.quiesce()==Result::Aborted);request.quarantine();
        assert(m.queue.ownsEvent(request.ticket(),m.event(request.ticket(),1024),7));
        assert(!m.queue.ownsEvent(request.ticket()+1,m.event(request.ticket(),1024),7));
        assert(!m.queue.ownsEvent(request.ticket(),m.event(request.ticket(),1024),8));
        uintptr_t owner=0;assert(request.retire(m.queue,owner)==Result::Busy && !owner);
        assert(request.finish(false)==Result::Busy);
        if (reset) {
            assert(m.queue.isolate()==Result::Busy);m.tb.isolated=true;assert(m.queue.isolate()==Result::Success);
        } else {
            const auto stopped=m.issue(XHCI::StopEndpoint);
            const auto dequeue=m.issue(XHCI::SetDequeue,m.tb.ring().nextAddress()|m.tb.ring().producerCycle());
            assert(m.queue.recover(stopped,dequeue)==Result::Success);
        }
        assert(request.retire(m.queue,owner)==Result::Success && owner==0xabc);
        assert(request.finish(false)==Result::Success && !request.validData());
        assert(static_cast<uint8_t *>(m.client->data)[0]==0x25);
        m.tb.release(owner);m.finishNative();
    }
}
void failureOwnership() {
    for (int fail:{1,2,3,4,5,6,7}) {
        Machine m(false);Request request(m.api);m.api.fail=fail;
        assert(request.prepare(m.client,m.configuration,1024,36)!=Result::Success);
        m.api.fail=0;assert(request.finish(false)==Result::Success);m.finishNative();
    }
    Machine m;Request request(m.api);
    assert(request.prepare(m.client,m.configuration,1024,36)==Result::Success);
    m.tb.bell=Result::HardwareError;
    assert(request.submit(m.queue,m.tb.ring(),0xabc,7,100,5000)==Result::HardwareError && request.ticket());
    uintptr_t owner=0;assert(request.retire(m.queue,owner)==Result::Busy);
    m.tb.isolated=true;assert(m.queue.isolate()==Result::Success);
    assert(request.retire(m.queue,owner)==Result::Success);
    m.api.clearFailures=1;assert(request.finish(false)==Result::HardwareError && m.client->references==2);
    assert(m.client->prepares==1 && m.api.memories.size()==2);
    assert(request.finish(false)==Result::Success);m.tb.release(owner);m.finishNative();
}
void ringWrapAndQueueIdentity() {
    Machine m;Request request(m.api);
    const XHCI::TRB nop=XHCI::trb(0,0,XHCI::TransferNoop<<10);uint64_t last=0;
    for (unsigned n=0;n<254;++n) {
        assert(m.tb.ring().submit(&nop,1,last)==Result::Success);assert(m.tb.ring().retire(1,last));
    }
    assert(request.prepare(m.client,m.configuration,1024,36)==Result::Success);
    assert(request.submit(m.queue,m.tb.ring(),0xabc,7,100,5000)==Result::Success);
    assert(request.transferDescriptor().data()[0].type()==XHCI::TransferNoop);
    XHCI::TransferRecord record{};assert(m.queue.complete(m.event(request.ticket(),1024),7,record)==Result::Success);
    Machine other;Request foreign(other.api);
    assert(foreign.prepare(other.client,other.configuration,1024,36)==Result::Success);
    assert(foreign.submit(other.queue,other.tb.ring(),0xabc,7,100,5000)==Result::Success);
    assert(other.queue.complete(other.event(foreign.ticket(),1024),7,record)==Result::Success);
    uintptr_t owner=0;assert(request.retire(other.queue,owner)==Result::Invalid);
    assert(request.retire(m.queue,owner)==Result::Success && request.finish(true)==Result::Success);
    m.tb.release(owner);m.finishNative();
    assert(foreign.retire(other.queue,owner)==Result::Success && foreign.finish(true)==Result::Success);
    other.tb.release(owner);other.finishNative();
}
}
void xhciRequestTests() {
    nativeBundledCompletions();
    transferAndFamilyOwnership();controlShortAndStatus();nativeErrorRetainsPreparedFIFO();ttBetweenRealStopAndDequeue();
    zeroLengthControlWithoutMD();
    cancellationAndIsolation();failureOwnership();ringWrapAndQueueIdentity();
}
