// SPDX-License-Identifier: BSD-2-Clause
#include "XHCIRequests.h"
#include "USBHostCurrent.h"
#include "usb_memory_model.h"
#include <algorithm>
#include <cstdio>
#include <array>
#include <map>
#include <string>

namespace {
struct Order { void dmaReadBarrier() {} void dmaWriteBarrier() {} };
struct CommandBackend {
    Order order;alignas(64) XHCI::TRB data[64]{};
    XHCI::ProducerRing<Order> producer{order};
    unsigned refs=0;
    CommandBackend() { assert(producer.initialize(data,0x100000,64,36)); }
    auto &ring() { return producer; }
    bool admitted(uint64_t epoch) { return epoch==7; }
    void retain(uintptr_t) { ++refs; }
    Result doorbell() { return Result::Success; }
    Result isolation(uint64_t) { return Result::Success; }
};
struct TransferBackend {
    Order order;alignas(64) XHCI::TRB data[256]{};
    XHCI::ProducerRing<Order> producer{order};
    std::map<uintptr_t,unsigned> refs;
    uint64_t epoch=7;
    unsigned bells=0;
    bool isolated=false;
    Result bell=Result::Success;
    TransferBackend() { assert(producer.initialize(data,0x200000,256,36)); }
    auto &ring() { return producer; }
    bool admitted(uint64_t generation) { return generation==epoch; }
    void retain(uintptr_t owner) { ++refs[owner]; }
    Result doorbell(unsigned slot,unsigned endpoint) { assert(slot==3 && endpoint);++bells;return bell; }
    Result isolation(uint64_t generation) { return isolated && generation==7 ? Result::Success : Result::Busy; }
    void release(uintptr_t owner) { assert(refs[owner]);if (!--refs[owner]) refs.erase(owner); }
};
using Commands=XHCI::CommandQueue<CommandBackend>;
using Queue=XHCI::TransferQueue<TransferBackend,Commands>;
using Request=XHCI::RequestMemory<MemoryAPI>;
struct Machine {
    MemoryAPI api;MemoryAPI::Memory *client;
    CommandBackend cb;Commands commands{cb};
    TransferBackend tb;Queue queue{tb,commands};
    XHCI::EndpointConfiguration endpoint;
    uint64_t commandTime=1;
    explicit Machine(XHCI::EndpointType type=XHCI::EndpointType::Bulk) {
        api.bus=0x800000;client=api.allocate(1024,64);
        assert(api.prepare(client,true,false)==Result::Success);
        std::memset(client->data,0x25,client->size);
        endpoint.address=type==XHCI::EndpointType::Control ? 0 : 0x81;
        endpoint.type=type;endpoint.speed=USB::Speed::High;
        endpoint.maxPacket=type==XHCI::EndpointType::Control ? 64 : type==XHCI::EndpointType::Isochronous ? 1024 : 512;
        endpoint.interval=type==XHCI::EndpointType::Isochronous ? 1 : 0;endpoint.dequeue=0x200000;
        assert(commands.initialize(7,32,32,36)==Result::Success);
        assert(queue.initialize(7,3,endpoint,36,0xee)==Result::Success);
    }
    unsigned id() const { return XHCI::endpointID(endpoint.address,endpoint.type); }
    XHCI::TRB event(uint64_t ticket,unsigned bytes,bool final=true,unsigned code=1) const {
        return XHCI::trb(ticket<<1|uint64_t(final),code<<24|bytes,
            XHCI::TransferEvent<<10|XHCI::EventDataFlag|3u<<24|id()<<16);
    }
    XHCI::TRB physical(uint64_t address,unsigned code=26,unsigned residue=0) const {
        return XHCI::trb(address,code<<24|residue,XHCI::TransferEvent<<10|3u<<24|id()<<16);
    }
    uint64_t command(XHCI::Type type,bool sp=false,uint64_t parameter=0,uintptr_t owner=0xee,unsigned endpointID=0) {
        XHCI::Command command;command.type=type;command.slot=3;
        command.endpoint=endpointID ? endpointID : id();command.parameter=parameter;command.option=sp;
        uint64_t ticket=0;assert(commands.submit(command,owner,commandTime++,1000000,ticket)==Result::Success);
        return ticket;
    }
    void completeCommand(uint64_t ticket,unsigned code=1) {
        XHCI::CommandRecord record{},done{};assert(commands.copy(ticket,record));
        assert(commands.complete(XHCI::trb(record.address,code<<24,XHCI::CommandEvent<<10|3u<<24),7,done)==Result::Success);
    }
    void ack(uint64_t ticket,uintptr_t expected=0xee) {
        uintptr_t owner=0;assert(commands.acknowledge(ticket,expected,owner)==Result::Success && owner==expected);
        assert(cb.refs);--cb.refs;
    }
    uint64_t stop(uint64_t now=200) {
        assert(queue.beginPause()==Result::Success);
        const auto ticket=command(XHCI::StopEndpoint,true);completeCommand(ticket);
        assert(queue.confirmPause(ticket,now)==Result::Success);ack(ticket);
        XHCI::CommandRecord old;assert(!commands.copy(ticket,old));
        return ticket;
    }
    uint64_t dequeue() {
        const auto ticket=command(XHCI::SetDequeue,false,tb.ring().nextAddress()|tb.ring().producerCycle());
        completeCommand(ticket);return ticket;
    }
    void finish(Request &request,bool copy) {
        uintptr_t owner=0;assert(request.retire(queue,owner)==Result::Success && owner==0xabc);
        assert(request.finish(copy)==Result::Success);tb.release(owner);
    }
    void finishNative() {
        assert(api.memories.size()==1 && api.commands.empty() && client->references==1 && client->prepares==1);
        assert(tb.refs.empty() && !queue.retained() && !commands.retained() && !cb.refs);
        assert(api.complete(client)==Result::Success);api.releaseMemory(client);
    }
};

void partialBulkAndDeadline() {
    Machine m;Request request(m.api),ready(m.api);
    assert(request.prepare(m.client,m.endpoint,1024,36)==Result::Success);
    assert(request.submit(m.queue,m.tb.ring(),0xabc,7,100,5000)==Result::Success);
    XHCI::TransferRecord record;assert(m.queue.copy(request.ticket(),record));
    std::array<XHCI::TRB,256> ring;std::memcpy(ring.data(),m.tb.data,sizeof(m.tb.data));
    assert(m.queue.beginPause()==Result::Success);
    const auto stop=m.command(XHCI::StopEndpoint,true);
    const auto partial=m.physical(record.first,26,512);
    assert(m.queue.pauseEvent(stop,partial,7));
    assert(!m.queue.pauseEvent(stop,partial,8));
    assert(request.finish(true)==Result::Busy && request.currentState()==DMAState::Owned);
    assert(m.queue.confirmPause(stop,200)==Result::Invalid); // No actual command completion yet.
    m.completeCommand(stop);
    assert(m.queue.confirmPause(stop,99)==Result::Invalid);
    assert(m.queue.confirmPause(stop,200)==Result::Success);m.ack(stop);
    assert(m.queue.pausedStopProof() && m.queue.outstanding()==1 && m.tb.bells==1);
    assert(ready.prepare(m.client,m.endpoint,64,36)==Result::Success);
    assert(ready.submit(m.queue,m.tb.ring(),0xabc,7,201,0)==Result::Busy && !ready.ticket());
    assert(ready.finish(false)==Result::Success);
    assert(m.queue.resume(10199,10000)==Result::Pending && m.tb.bells==1);
    assert(m.queue.expire(10200)==Result::Success); // Original deadline 5100 is frozen.
    assert(m.queue.resume(10199,10000)==Result::Invalid);
    assert(m.queue.resume(10200,10000)==Result::Success && m.tb.bells==2);
    assert(!m.queue.pausedStopProof() && !m.queue.paused() && m.commands.lastTicket()==stop);
    assert(!std::memcmp(ring.data(),m.tb.data,sizeof(m.tb.data))); // No replay, dequeue or cycle rewrite.
    assert(m.queue.copy(request.ticket(),record) && record.deadline==15100 && !record.transferred);
    assert(m.queue.expire(15099)==Result::Success);
    std::memset(m.api.memories.back()->data,0x72,1024);
    assert(m.queue.complete(m.event(request.ticket(),1024),7,record)==Result::Success);
    m.finish(request,true);assert(request.transferred()==1024 && m.api.copies==1);
    assert(static_cast<uint8_t *>(m.client->data)[1023]==0x72);m.finishNative();
}

void controlShortAcrossPause() {
    for (bool dataAlreadyReported:{false,true}) {
        Machine m(XHCI::EndpointType::Control);Request request(m.api);
        USB::SetupPacket setup{0x80,6,0x100,0,64};
        assert(request.prepare(m.client,m.endpoint,64,36,&setup)==Result::Success);
        assert(request.submit(m.queue,m.tb.ring(),0xabc,7,100,5000)==Result::Success);
        XHCI::TransferRecord record;assert(m.queue.copy(request.ticket(),record));
        const auto first=record.first;
        std::memset(m.api.memories.back()->data,0x91,9);
        if (dataAlreadyReported) assert(m.queue.complete(m.event(request.ticket(),9,false,13),7,record)==Result::Pending);
        assert(m.queue.beginPause()==Result::Success);
        const auto stop=m.command(XHCI::StopEndpoint,true);
        const auto stopped=m.physical(first+(dataAlreadyReported ? 48 : 16),dataAlreadyReported ? 26 : 28,9);
        assert(m.queue.pauseEvent(stop,stopped,7));
        assert(!m.queue.pauseEvent(stop,m.event(request.ticket(),9,false,28),7));
        m.completeCommand(stop);assert(m.queue.confirmPause(stop,200)==Result::Success);m.ack(stop);
        assert(m.queue.resume(10200,10000)==Result::Success);
        if (!dataAlreadyReported) assert(m.queue.complete(m.event(request.ticket(),9,false,13),7,record)==Result::Pending);
        uintptr_t owner=0;assert(request.retire(m.queue,owner)==Result::Busy && !owner);
        assert(request.finish(true)==Result::Busy && !m.api.copies); // STATUS still owns DMA.
        assert(m.queue.complete(m.event(request.ticket(),0),7,record)==Result::Success);
        m.finish(request,true);assert(request.transferred()==9 && m.api.copies==1);
        assert(static_cast<uint8_t *>(m.client->data)[8]==0x91 && static_cast<uint8_t *>(m.client->data)[9]==0x25);
        assert(m.commands.lastTicket()==stop && m.tb.bells==2);m.finishNative();
    }
}

void completionWinsStopRace() {
    Machine m;Request request(m.api);
    assert(request.prepare(m.client,m.endpoint,1024,36)==Result::Success);
    assert(request.submit(m.queue,m.tb.ring(),0xabc,7,100,5000)==Result::Success);
    assert(m.queue.beginPause()==Result::Success);const auto stop=m.command(XHCI::StopEndpoint,true);
    XHCI::TransferRecord record;
    assert(m.queue.complete(m.event(request.ticket(),1024),7,record)==Result::Success);m.finish(request,true);
    assert(m.queue.pauseEvent(stop,m.physical(m.tb.ring().nextAddress(),27),7));
    const auto link=m.tb.ring().startAddress()+m.tb.ring().dataEntries()*sizeof(XHCI::TRB);
    assert(m.queue.pauseEvent(stop,m.physical(link,27),7));
    assert(!m.queue.pauseEvent(stop,m.physical(link+16,27),7));
    m.completeCommand(stop);assert(m.queue.confirmPause(stop,200)==Result::Success);m.ack(stop);
    assert(m.queue.resume(10200,10000)==Result::Success && m.tb.bells==1); // Nothing left to ring.
    assert(m.api.copies==1);m.finishNative();
}

void pausedCancellationProof() {
    Machine m;Request request(m.api);
    assert(request.prepare(m.client,m.endpoint,1024,36)==Result::Success);
    assert(request.submit(m.queue,m.tb.ring(),0xabc,7,100,5000)==Result::Success);
    const auto stale=m.dequeue();m.ack(stale);
    m.stop();assert(m.queue.quiesce()==Result::Aborted);request.quarantine();
    uintptr_t owner=0;assert(request.retire(m.queue,owner)==Result::Busy);
    assert(request.finish(false)==Result::Busy && m.queue.recoverPaused(stale)==Result::NotReady);
    const auto wrong=m.command(XHCI::SetDequeue,false,m.tb.ring().nextAddress()|1,0xab);
    m.completeCommand(wrong);assert(m.queue.recoverPaused(wrong)==Result::Invalid);m.ack(wrong,0xab);
    const auto bad=m.command(XHCI::SetDequeue,false,m.tb.ring().startAddress()|1);
    m.completeCommand(bad);assert(m.queue.recoverPaused(bad)==Result::Invalid);m.ack(bad);
    const auto dequeue=m.command(XHCI::SetDequeue,false,m.tb.ring().nextAddress()|m.tb.ring().producerCycle());
    assert(m.queue.recoverPaused(dequeue)==Result::Invalid);m.completeCommand(dequeue);
    assert(m.queue.recoverPaused(dequeue)==Result::Success);m.ack(dequeue);
    assert(m.queue.pausedStopProof() && m.queue.paused() && !m.queue.outstanding());
    m.finish(request,false);assert(!request.validData() && !m.api.copies);
    assert(m.queue.resume(10199,10000)==Result::Pending);
    assert(m.queue.resume(10200,10000)==Result::Success && m.tb.bells==1);
    assert(m.queue.recoverPaused(dequeue)==Result::NotReady);m.finishNative();
}

void isoAbortDuringSuspend() {
    Machine m(XHCI::EndpointType::Isochronous);Request a(m.api),b(m.api),ready(m.api);
    IOUSBHostIsochronousTransaction transaction{};transaction.requestCount=64;
    for (auto *request:{&a,&b,&ready}) {
        assert(request->prepareIsochronous(m.client,m.endpoint,transaction,36,{10,false,false})==Result::Success);
        m.api.bus+=65536;
    }
    for (auto *request:{&a,&b}) assert(request->submit(m.queue,m.tb.ring(),0xabc,7,100,0)==Result::Success);
    assert(m.queue.quiesce()==Result::Aborted);a.quarantine();b.quarantine();
    assert(m.queue.beginPause()==Result::Success);const auto stop=m.command(XHCI::StopEndpoint,true);
    XHCI::TransferRecord record;assert(m.queue.copy(a.ticket(),record));
    assert(m.queue.pauseEvent(stop,m.physical(record.first,26),7));
    assert(ready.finish(false)==Result::Success); // Software-only prepared payload needs no Stop proof.
    assert(a.finish(false)==Result::Busy && b.finish(false)==Result::Busy);
    m.completeCommand(stop);assert(m.queue.confirmPause(stop,200)==Result::Success);m.ack(stop);
    assert(a.finish(false)==Result::Busy); // Stop alone cannot release the published ring slots.
    const auto dq=m.dequeue();assert(m.queue.recoverPaused(dq)==Result::Success);m.ack(dq);
    m.finish(a,false);m.finish(b,false);
    assert(m.queue.resume(10200,10000)==Result::Success && m.tb.bells==2 && !m.api.copies);m.finishNative();
}

void rejectedStopAndEmptyEndpoint() {
    Machine m;Request request(m.api);
    assert(request.prepare(m.client,m.endpoint,1024,36)==Result::Success);
    assert(request.submit(m.queue,m.tb.ring(),0xabc,7,100,5000)==Result::Success);
    const auto old=m.command(XHCI::StopEndpoint,true);m.completeCommand(old);
    assert(m.queue.beginPause()==Result::Success && m.queue.confirmPause(old,200)==Result::NotReady);m.ack(old);
    for (unsigned variant=0;variant<4;++variant) {
        const auto stop=m.command(XHCI::StopEndpoint,variant!=0,0,variant==1 ? 0xab : 0xee,variant==2 ? m.id()+1 : 0);
        XHCI::TransferRecord record;assert(m.queue.copy(request.ticket(),record));
        assert(!m.queue.pauseEvent(stop,m.physical(record.first),8));
        if (variant<3) assert(!m.queue.pauseEvent(stop,m.physical(record.first),7));
        m.completeCommand(stop,variant==3 ? 19 : 1);
        assert(m.queue.confirmPause(stop,200)==Result::Invalid);m.ack(stop,variant==1 ? 0xab : 0xee);
    }
    assert(m.queue.pauseEmpty(200)==Result::Busy);
    m.queue.abandonPause();assert(!m.queue.pausing() && !m.queue.paused() && !m.queue.faulted());
    assert(request.finish(true)==Result::Busy && m.tb.bells==1);
    XHCI::TransferRecord record;assert(m.queue.complete(m.event(request.ticket(),1024),7,record)==Result::Success);
    assert(m.queue.pauseEmpty(200)==Result::Busy); // Retained terminal record still has an owner.
    m.finish(request,true);
    assert(m.queue.pauseEmpty(99)==Result::Invalid && m.queue.pauseEmpty(200)==Result::Success);
    assert(!m.queue.pausedStopProof() && m.queue.resume(10200,10000)==Result::Success && m.tb.bells==1);
    m.finishNative();
}

void epochAndDoorbellFailurePinDMA() {
    for (bool revoke:{false,true}) {
        Machine m;Request request(m.api);
        assert(request.prepare(m.client,m.endpoint,1024,36)==Result::Success);
        assert(request.submit(m.queue,m.tb.ring(),0xabc,7,100,5000)==Result::Success);m.stop();
        if (revoke) {
            m.tb.epoch=8;assert(m.queue.resume(10200,10000)==Result::NotReady);
            assert(m.queue.quiesce()==Result::Aborted);
            const auto dq=m.dequeue();assert(m.queue.recoverPaused(dq)==Result::NotReady);m.ack(dq);
        } else {
            m.tb.bell=Result::HardwareError;assert(m.queue.resume(10200,10000)==Result::HardwareError);
            assert(!m.queue.pausedStopProof() && m.queue.faulted());
        }
        assert(request.finish(false)==Result::Busy && request.currentState()==DMAState::Owned);
        assert(m.queue.isolate()==Result::Busy);
        m.tb.isolated=true;assert(m.queue.isolate()==Result::Success && !m.queue.pausedStopProof());
        m.finish(request,false);assert(m.queue.resume(20200,20000)==Result::NotReady);m.finishNative();
    }
}

void overflowAndResumedTimeout() {
    for (bool overflow:{false,true}) {
        Machine m;Request request(m.api);
        assert(request.prepare(m.client,m.endpoint,1024,36)==Result::Success);
        assert(request.submit(m.queue,m.tb.ring(),0xabc,7,100,overflow ? UINT64_MAX-100 : 5000)==Result::Success);m.stop();
        assert(m.queue.resume(10200,10000)==(overflow ? Result::Invalid : Result::Success));
        assert(m.tb.bells==(overflow ? 1u : 2u));
        if (!overflow) assert(m.queue.expire(15100)==Result::Timeout);
        assert(m.queue.isolate()==Result::Busy);m.tb.isolated=true;assert(m.queue.isolate()==Result::Success);
        m.finish(request,false);m.finishNative();
    }
}

void deviceFenceAndRollback() {
    XHCI::DevicePause device;
    uint64_t duration=0;
    assert(device.running() && !device.elapsed(1,duration) && !device.beginResume());
    assert(device.beginSuspend(UINT64_MAX)==Result::Invalid && device.running());
    assert(device.beginSuspend(100)==Result::Success && device.transitioning() && !device.running());
    assert(device.beginSuspend(101)==Result::Busy);
    assert(device.stopped(99)==Result::Invalid && device.stopped(200)==Result::Success);
    assert(device.stopped(199)==Result::Invalid && device.stopped(300)==Result::Success);
    assert(device.remaining(10299)==1 && device.finishSuspend() && device.suspended());
    assert(device.beginResume() && !device.finishResume(10299));
    assert(device.elapsed(10300,duration) && duration==10200 && device.finishResume(10300));
    assert(device.running() && !device.finishSuspend());
    assert(device.beginSuspend(20000)==Result::Success && device.stopped(21000)==Result::Success);
    assert(device.beginResume()); // Rejected suspend rolls back from Quiescing too.
    assert(!device.finishResume(30999) && device.finishResume(31000));
    assert(device.beginSuspend(UINT64_MAX-XHCI::SuspendMinimumUS)==Result::Success);
    assert(device.stopped(UINT64_MAX-XHCI::SuspendMinimumUS+1)==Result::Invalid);
    assert(device.beginResume() && device.finishResume(UINT64_MAX));
}

void commandPoolRemainsReusable() {
    Machine m;Request request(m.api);
    assert(request.prepare(m.client,m.endpoint,1024,36)==Result::Success);
    assert(request.submit(m.queue,m.tb.ring(),0xabc,7,100,0)==Result::Success);
    const auto allocations=m.api.allocationCalls;
    for (unsigned n=0;n<160;++n) {
        const auto now=200+uint64_t(n)*10200;
        m.stop(now);
        assert(!m.commands.retained() && !m.cb.refs && m.queue.pausedStopProof());
        assert(request.currentState()==DMAState::Owned && request.finish(true)==Result::Busy);
        assert(m.queue.resume(now+10000,10000)==Result::Success);
        assert(m.api.allocationCalls==allocations && m.client->prepares==1 && !m.api.copies);
    }
    XHCI::TransferRecord record;
    assert(m.commands.lastTicket()==160 && m.tb.bells==161);
    assert(m.queue.complete(m.event(request.ticket(),1024),7,record)==Result::Success);
    m.finish(request,true);m.finishNative();
}

struct CurrentProperties {
    std::map<std::string,uint32_t> values;
    unsigned calls=0,failAt=0;
    bool set(const char *key,uint32_t value) {
        if (++calls==failAt) return false;
        values[key]=value;return true;
    }
    uint32_t get(const char *key) const { return values.at(key); }
};
// External Native policy model, derived from the pinned Family pool and
// allocator. Production only publishes properties and calls Native policy.
struct NativeCurrentPool {
    struct Credit { uint32_t wake=0,sleep=0; };
    Credit capacity;
    std::map<unsigned,Credit> clients;
    explicit NativeCurrentPool(const CurrentProperties &p) :
        capacity{p.get("kUSBWakePowerSupply"),p.get("kUSBSleepPowerSupply")} {}
    Credit allocated() const {
        Credit used;for (auto &entry:clients) { used.wake+=entry.second.wake;used.sleep+=entry.second.sleep; }
        return used;
    }
    Credit request(unsigned client,Credit wanted) {
        clients.erase(client);const auto used=allocated();
        wanted.wake=std::min(wanted.wake,capacity.wake-used.wake);
        wanted.sleep=std::min(wanted.sleep,capacity.sleep-used.sleep);
        if (wanted.wake || wanted.sleep) clients[client]=wanted;
        const auto total=allocated();assert(total.wake<=capacity.wake && total.sleep<=capacity.sleep);
        return wanted;
    }
};
struct NativeCurrentPort {
    NativeCurrentPool &pool;unsigned id;uint32_t limit,minimum;
    std::map<unsigned,uint32_t> requests;
    NativeCurrentPort(NativeCurrentPool &p,unsigned index,const CurrentProperties &properties,uint32_t enumeration) :
        pool(p),id(index),limit(properties.get("kUSBWakePortCurrentLimit")),minimum(enumeration) {
        assert(!properties.get("kUSBSleepPortCurrentLimit"));
        assert(allocate(0,minimum)==minimum);
    }
    uint32_t allocate(unsigned client,uint32_t wanted) {
        wanted=std::min(limit,client ? wanted:std::max(minimum,wanted));
        uint32_t aggregate=wanted;
        for (auto &entry:requests) if (entry.first!=client) aggregate=std::max(aggregate,entry.second);
        const auto granted=pool.request(id,{aggregate,0});
        wanted=std::min(wanted,granted.wake);
        if (wanted) requests[client]=wanted;else requests.erase(client);
        return wanted;
    }
    ~NativeCurrentPort() { pool.request(id,{}); }
};


// External IOKit client model. Native registration explicitly resets the
// driver request; Native's post-createPorts selector lowers its device request.
// IOServicePM computes the maximum client demand, bounded by the parent.
struct NativeControllerPowerClients {
    unsigned driver=0,device=0,parent=3;
    bool initialized=false,live=true,failNext=false;
    Result request(bool privateClient,unsigned value) {
        if (!initialized || !live) return Result::NotReady;
        if (value>3) return Result::Invalid;
        if (failNext) { failNext=false;return Result::HardwareError; }
        (privateClient ? device:driver)=value;return Result::Success;
    }
    void registered() {
        initialized=true;
        assert(request(false,0)==Result::Success && request(true,3)==Result::Success);
    }
    unsigned desired() const { return live ? std::min(parent,std::max(driver,device)):0; }
};
void nativeRegistrationPowerPolicy() {
    unsigned cases=0;
    for (unsigned epoch=0;epoch<4;++epoch) for (bool earlyInitialized:{false,true}) {
        NativeControllerPowerClients old;old.initialized=earlyInitialized;
        assert(old.request(false,3)==(earlyInitialized ? Result::Success:Result::NotReady));
        old.registered();assert(old.driver==0 && old.desired()==3);
        assert(old.request(true,0)==Result::Success && old.desired()==0); // old pre-registration vote was lost

        NativeControllerPowerClients current;current.initialized=earlyInitialized;
        current.registered();
        assert(current.request(false,3)==Result::Success); // callback after PMinit/registration
        assert(current.request(true,0)==Result::Success && current.desired()==3);
        for (unsigned idle=0;idle<4;++idle) {
            assert(current.request(true,idle)==Result::Success && current.desired()==3);
        }
        // Ordinary driver demand cannot override an actual parent power cap.
        // The platform cold path halts/drains and terminates this controller
        // before returning the underlying hardware resource holds.
        for (unsigned cap=0;cap<3;++cap) { current.parent=cap;assert(current.desired()==cap); }
        current.live=false;assert(!current.desired() && current.request(false,3)==Result::NotReady);
        ++cases;

        NativeControllerPowerClients failed;failed.registered();failed.failNext=true;
        const auto vote=failed.request(false,3);
        const bool publish=vote==Result::Success;
        assert(vote==Result::HardwareError && !publish && failed.driver==0);
        failed.live=false;assert(!failed.desired());++cases;
    }
    assert(cases==16);
    std::printf("Native Host PM: %u registration/reset-vote and cold-teardown scenarios passed\n",cases);
}

void leasedCurrentAndNativeAllocation() {
    USB::HostCurrent current;
    USB::PlatformContract resources{2,31,120000000,19200000,40,4,1000000,0};
    unsigned cases=0;
    for (unsigned budget : {500000,750000,1000000,1250000,1500000}) {
        for (unsigned speed : {3,4}) {
            resources.maximumSpeed=speed;resources.sourceMicroamps=budget;
            const auto configured=current.configure(resources);
            if (speed==4 && budget<900000) {
                assert(configured==Result::Unsupported && !current.wake);++cases;continue;
            }
            assert(configured==Result::Success && current.wake==budget/1000);
            CurrentProperties controller,usb2,usb3;
            assert(current.controller([&](const char *key,uint32_t value) { return controller.set(key,value); }));
            assert(current.port(false,[&](const char *key,uint32_t value) { return usb2.set(key,value); }));
            assert(current.port(true,[&](const char *key,uint32_t value) { return usb3.set(key,value); })==(speed==4));
            assert(controller.calls==2 && controller.values.size()==2 && usb2.calls==3 && usb3.calls==(speed==4 ? 3u:0u));
            assert(usb2.get("kUSBWakePortCurrentLimit")==500 && usb2.get("UsbConnector")==unsigned(speed==4 ? 9:8));
            if (speed==4) assert(usb3.get("kUSBWakePortCurrentLimit")==900 && usb3.get("UsbConnector")==9);
            else assert(usb3.values.empty());
            assert(USB::HostCurrent::enumeration(false)==100 && USB::HostCurrent::enumeration(true)==150);
            // Replug/cold-PM gives each new Native controller its own pool;
            // old client credits cannot alter the new connection's ceiling.
            for (unsigned epoch=0;epoch<4;++epoch) {
                NativeCurrentPool pool(controller);
                assert(pool.capacity.wake==budget/1000 && !pool.capacity.sleep);
                {
                    NativeCurrentPort hs(pool,1,usb2,USB::HostCurrent::enumeration(false));
                    if (speed==4) {
                        NativeCurrentPort ss(pool,2,usb3,USB::HostCurrent::enumeration(true));
                        assert(pool.allocated().wake==250);
                        assert(hs.allocate(10,500)==std::min(500u,budget/1000-150));
                        assert(ss.allocate(11,900)==std::min(900u,budget/1000-pool.clients.at(1).wake));
                        assert(pool.allocated().wake<=budget/1000);
                        const auto before=pool.allocated().wake;
                        ss.allocate(11,900);assert(pool.allocated().wake==before); // replace, never double debit
                        assert(hs.allocate(10,0)==0);assert(pool.clients.at(1).wake==100);
                        assert(ss.allocate(11,UINT32_MAX)==std::min(900u,budget/1000-100));
                        assert(ss.allocate(11,0)==0 && pool.allocated().wake==250);
                    } else {
                        // An unrouted USB3 port creates no Native allocator
                        // or reserve, so the full500mA remains available.
                        assert(pool.allocated().wake==100 && !pool.clients.count(2));
                        assert(hs.allocate(10,500)==500 && pool.allocated().wake==500);
                        assert(hs.allocate(10,0)==0 && pool.allocated().wake==100);
                    }
                    // A cold system sleep has no USB Source supply, including
                    // a device that asks for a retained sleep allocation.
                    assert(!pool.request(3,{0,100}).sleep && !pool.allocated().sleep);
                    ++cases;
                }
                assert(pool.clients.empty() && !pool.allocated().wake);
            }
        }
    }
    resources.maximumSpeed=4;resources.sourceMicroamps=1000000;
    assert(current.configure(resources)==Result::Success);
    for (unsigned failed=1;failed<=2;++failed) {
        CurrentProperties controller;controller.failAt=failed;
        assert(!current.controller([&](const char *key,uint32_t value) { return controller.set(key,value); }));
        assert(controller.calls==failed);++cases;
    }
    for (unsigned failed=1;failed<=3;++failed) {
        for (bool super : {false,true}) {
            CurrentProperties port;port.failAt=failed;
            assert(!current.port(super,[&](const char *key,uint32_t value) { return port.set(key,value); }));
            assert(port.calls==failed);++cases;
        }
    }
    for (unsigned budget : {0u,1u,499999u,900000u,1500001u,UINT32_MAX}) {
        resources.sourceMicroamps=budget;
        assert(current.configure(resources)==(!budget ? Result::Unsupported:Result::Invalid));
        assert(!current.wake && !current.maximumSpeed);
        CurrentProperties p;auto set=[&](const char *key,uint32_t value) { return p.set(key,value); };
        assert(!current.controller(set) && !current.port(false,set) && !current.port(true,set) && !p.calls);++cases;
    }
    resources.sourceMicroamps=1000000;resources.version=1;
    assert(current.configure(resources)==Result::Invalid && !current.wake);++cases;
    resources.version=2;resources.reserved=1;
    assert(current.configure(resources)==Result::Invalid && !current.wake);++cases;
    resources.reserved=0;resources.maximumSpeed=4;resources.sourceMicroamps=1500000;
    CurrentProperties oldProperties,newProperties;
    assert(current.configure(resources)==Result::Success && current.controller([&](const char *key,uint32_t value) {
        return oldProperties.set(key,value);
    }));
    NativeCurrentPool oldPool(oldProperties);assert(oldPool.request(1,{1400,0}).wake==1400);
    resources.maximumSpeed=3;resources.sourceMicroamps=500000;
    assert(current.configure(resources)==Result::Success && current.controller([&](const char *key,uint32_t value) {
        return newProperties.set(key,value);
    }));
    NativeCurrentPool newPool(newProperties);
    assert(newPool.request(1,{1400,0}).wake==500 && oldPool.allocated().wake==1400);++cases;
    assert(cases==51);
    std::printf("Native Host current: %u budget, shared allocation, release, cold-PM and publication failure cases passed\n",cases);
}
}
void xhciPowerTests() {
    partialBulkAndDeadline();controlShortAcrossPause();completionWinsStopRace();pausedCancellationProof();
    isoAbortDuringSuspend();rejectedStopAndEmptyEndpoint();epochAndDoorbellFailurePinDMA();
    overflowAndResumedTimeout();deviceFenceAndRollback();commandPoolRemainsReusable();
    leasedCurrentAndNativeAllocation();nativeRegistrationPowerPolicy();
}
