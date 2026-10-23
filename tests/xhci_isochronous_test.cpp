// SPDX-License-Identifier: BSD-2-Clause
#include "XHCIRequests.h"
#include "usb_memory_model.h"
#include <map>
#include <memory>
#include <vector>

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
    std::map<uintptr_t,unsigned> refs;
    bool isolated=false;
    TransferBackend() { assert(producer.initialize(data,0x200000,256,36)); }
    auto &ring() { return producer; }
    bool admitted(uint64_t epoch) { return epoch==7; }
    void retain(uintptr_t owner) { ++refs[owner]; }
    Result doorbell(unsigned slot,unsigned endpoint) { assert(slot==3 && endpoint);return Result::Success; }
    Result isolation(uint64_t epoch) { return isolated && epoch==7 ? Result::Success : Result::NotReady; }
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
    Machine(bool input,unsigned bytes=64,bool nearLink=false) {
        api.bus=0x800000;client=api.allocate(bytes,64);
        assert(api.prepare(client,input,!input)==Result::Success);
        for (unsigned i=0;i<bytes;++i) static_cast<uint8_t *>(client->data)[i]=uint8_t(i);
        if (nearLink) {
            XHCI::TRB noops[252];for (auto &item:noops) item=XHCI::trb(0,0,XHCI::TransferNoop<<10);
            uint64_t last=0;assert(tb.ring().submit(noops,252,last)==Result::Success);
            assert(tb.ring().retire(252,last));
        }
        endpoint.address=input ? 0x81 : 1;endpoint.type=XHCI::EndpointType::Isochronous;
        endpoint.speed=USB::Speed::High;endpoint.maxPacket=1024;endpoint.interval=1;
        endpoint.dequeue=tb.ring().nextAddress();endpoint.cycle=bool(tb.ring().producerCycle());
        assert(commands.initialize(7,32,32,36)==Result::Success);
        assert(queue.initialize(7,3,endpoint,36,0xee)==Result::Success);
    }
    XHCI::TRB event(uint64_t ticket,unsigned bytes,unsigned code=1) const {
        return XHCI::trb(ticket<<1|1,code<<24|bytes,XHCI::TransferEvent<<10|XHCI::EventDataFlag|
            3u<<24|XHCI::endpointID(endpoint.address,endpoint.type)<<16);
    }
    void retire(Request &request,unsigned bytes,unsigned code=1) {
        XHCI::TransferRecord record;
        assert(queue.complete(event(request.ticket(),bytes,code),7,record)==Result::Success);
        uintptr_t released=0;assert(request.retire(queue,released)==Result::Success && released==0xabc);
        tb.release(released);
    }
    uint64_t issue(XHCI::Type type,uint64_t parameter=0) {
        XHCI::Command command;command.type=type;command.slot=3;
        command.endpoint=XHCI::endpointID(endpoint.address,endpoint.type);command.parameter=parameter;
        uint64_t ticket=0;assert(commands.submit(command,0xee,1,1000,ticket)==Result::Success);
        XHCI::CommandRecord record,done;assert(commands.copy(ticket,record));
        assert(commands.complete(XHCI::trb(record.address,1u<<24,XHCI::CommandEvent<<10|3u<<24),7,done)==Result::Success);
        return ticket;
    }
    void finishNative() {
        assert(api.memories.size()==1 && api.commands.empty() && client->references==1 && client->prepares==1);
        assert(tb.refs.empty() && !queue.retained());
        assert(api.complete(client)==Result::Success);api.releaseMemory(client);
    }
};

void schedulingWindow() {
    Machine m(true);XHCI::IsoSchedule schedule;
    assert(XHCI::isochronousSchedule(m.endpoint,0,0,0,0,0,schedule)==Result::Success && schedule.asap);
    assert(XHCI::isochronousSchedule(m.endpoint,10,0,8,67,4,schedule)==Result::Success);
    assert(!schedule.asap && schedule.frame==10);
    assert(XHCI::isochronousSchedule(m.endpoint,10,7,8,67,4,schedule)==Result::Success && schedule.frame==10);
    assert(XHCI::isochronousSchedule(m.endpoint,10,8,8,67,4,schedule)==Result::Success && schedule.frame==11);
    assert(XHCI::isochronousSchedule(m.endpoint,9,0,8,67,4,schedule)==Result::Timeout);
    assert(XHCI::isochronousSchedule(m.endpoint,9,0,8,64,7,schedule)==Result::Timeout);
    assert(XHCI::isochronousSchedule(m.endpoint,8,7,8,67,4,schedule)==Result::Timeout);
    assert(XHCI::isochronousSchedule(m.endpoint,9,0,8,67,56,schedule)==Result::Timeout);
    assert(XHCI::isochronousSchedule(m.endpoint,16,0,8,67,56,schedule)==Result::Success);
    assert(XHCI::isochronousSchedule(m.endpoint,903,7,8,67,0,schedule)==Result::Success && schedule.frame==903);
    assert(XHCI::isochronousSchedule(m.endpoint,904,0,8,67,0,schedule)==Result::Pending);
    assert(XHCI::isochronousSchedule(m.endpoint,2048,0,2046,16373,1,schedule)==Result::Success && !schedule.frame);
    const uint64_t far=uint64_t(1)<<35;
    assert(XHCI::isochronousSchedule(m.endpoint,far+2,0,far,3,0,schedule)==Result::Success && schedule.frame==2);
    assert(XHCI::isochronousSchedule(m.endpoint,10,0,8,67,0,schedule)==Result::Success);
    assert(XHCI::isochronousSchedule(m.endpoint,10,0,8,75,0,schedule)==Result::Invalid);
    assert(XHCI::isochronousSchedule(m.endpoint,UINT64_MAX,0,8,67,0,schedule)==Result::Invalid);
    assert(XHCI::isochronousSchedule(m.endpoint,10,UINT64_MAX,8,67,0,schedule)==Result::Invalid);
    assert(XHCI::isochronousSchedule(m.endpoint,10,0,8,67,57,schedule)==Result::Invalid);
    uint32_t interval=0;
    assert(XHCI::isochronousInterval(m.endpoint,interval)==Result::Success && interval==1);
    m.endpoint.speed=USB::Speed::Full;m.endpoint.interval=3;
    assert(XHCI::isochronousInterval(m.endpoint,interval)==Result::Success && interval==32);
    assert(XHCI::isochronousSchedule(m.endpoint,10,2,8,67,0,schedule)==Result::Invalid);
    assert(XHCI::isochronousSchedule(m.endpoint,12,2,8,67,0,schedule)==Result::Success && schedule.frame==20);
    m.endpoint.interval=17;assert(XHCI::isochronousInterval(m.endpoint,interval)==Result::Invalid);
    m.finishNative();
}
void outputRangesAndWrap() {
    for (bool wrap:{false,true}) {
        Machine m(false,16);Request request(m.api);
        IOUSBHostIsochronousTransaction transaction{};
        transaction.offset=12;transaction.requestCount=wrap ? 20 : 4;
        transaction.options=wrap ? kIsochronousTransactionOptionsWrap : kIsochronousTransactionOptionsNone;
        assert(request.prepareIsochronous(m.client,m.endpoint,transaction,36,{2047,false,false})==Result::Success);
        const auto *data=static_cast<uint8_t *>(m.api.memories.back()->data);
        for (unsigned i=0;i<transaction.requestCount;++i) assert(data[i]==(i+12)%16);
        assert(m.client->references==2 && m.client->prepares==1);
        assert(request.submit(m.queue,m.tb.ring(),0xabc,7,1,1000)==Result::Success);
        assert(((request.transferDescriptor().data()[0].control>>20)&2047)==2047);
        m.retire(request,transaction.requestCount);
        assert(request.finish(true)==Result::Success);
        for (unsigned i=0;i<16;++i) assert(static_cast<uint8_t *>(m.client->data)[i]==i);
        m.finishNative();
    }
}
void pipelinedInputAndLink() {
    Machine m(true,64,true);Request first(m.api),second(m.api);
    IOUSBHostIsochronousTransaction a{},b{};a.offset=8;a.requestCount=12;
    b.offset=52;b.requestCount=16;b.options=kIsochronousTransactionOptionsWrap;
    assert(first.prepareIsochronous(m.client,m.endpoint,a,36,{10,false,false})==Result::Success);
    auto *firstData=m.api.memories.back();m.api.bus=0x900000;
    assert(second.prepareIsochronous(m.client,m.endpoint,b,36,{10,false,false})==Result::Success);
    auto *secondData=m.api.memories.back();
    assert(first.submit(m.queue,m.tb.ring(),0xabc,7,1,1000)==Result::Success);
    assert(second.submit(m.queue,m.tb.ring(),0xabc,7,2,1000)==Result::Success);
    assert(first.transferDescriptor().count()==2 && second.transferDescriptor().count()==3);
    assert(second.transferDescriptor().data()[0].type()==XHCI::TransferNoop);
    assert(m.queue.outstanding()==2 && m.tb.ring().pending()==5 && m.client->references==3 && m.tb.refs[0xabc]==2);
    std::memset(firstData->data,0xa5,7);std::memset(secondData->data,0xb6,16);
    m.retire(first,7,13);assert(first.finish(true)==Result::Success && first.transferred()==7);
    assert(second.currentState()==USB::DMAState::Owned && second.finish(true)==Result::Busy);
    auto *native=static_cast<uint8_t *>(m.client->data);
    for (unsigned i=0;i<64;++i) assert(native[i]==(i>=8 && i<15 ? 0xa5 : i));
    m.retire(second,16);assert(second.finish(true)==Result::Success);
    for (unsigned i=0;i<64;++i) assert(native[i]==(i<4 || i>=52 ? 0xb6 : i>=8 && i<15 ? 0xa5 : i));
    m.finishNative();
}
void invalidAndZero() {
    Machine m(true,16);Request request(m.api);IOUSBHostIsochronousTransaction transaction{};
    transaction.offset=12;transaction.requestCount=8;
    assert(request.prepareIsochronous(m.client,m.endpoint,transaction,36,{})==Result::Invalid);
    assert(m.api.memories.size()==1 && m.client->references==1);
    transaction.options=2;assert(request.prepareIsochronous(m.client,m.endpoint,transaction,36,{})==Result::Unsupported);
    transaction.options=1;transaction.offset=UINT32_MAX;transaction.requestCount=1;
    assert(request.prepareIsochronous(m.client,m.endpoint,transaction,36,{})==Result::Invalid);
    transaction={};
    assert(request.prepareIsochronous(nullptr,m.endpoint,transaction,36,{})==Result::Success);
    assert(request.submit(m.queue,m.tb.ring(),0xabc,7,1,0)==Result::Success);
    m.retire(request,0);assert(request.finish(true)==Result::Success && request.transferred()==0);
    m.finishNative();
}
void capacityRetryRepacking() {
    Machine m(true,64,true);XHCI::TransferQueue<TransferBackend,Commands,1> limited(m.tb,m.commands);
    assert(limited.initialize(7,3,m.endpoint,36,0xee)==Result::Success);
    Request first(m.api),waiting(m.api),intervening(m.api);
    IOUSBHostIsochronousTransaction transaction{};transaction.requestCount=8;
    for (auto *request:{&first,&waiting,&intervening}) {
        assert(request->prepareIsochronous(m.client,m.endpoint,transaction,36,{})==Result::Success);m.api.bus+=0x10000;
    }
    auto complete=[&](Request &request) {
        XHCI::TransferRecord record;assert(limited.complete(m.event(request.ticket(),8),7,record)==Result::Success);
        uintptr_t owner=0;assert(request.retire(limited,owner)==Result::Success && owner==0xabc);m.tb.release(owner);
        assert(request.finish(false)==Result::Success);
    };
    assert(first.submit(limited,m.tb.ring(),0xabc,7,1,0)==Result::Success);
    assert(waiting.submit(limited,m.tb.ring(),0xabc,7,2,0)==Result::Busy && !waiting.ticket());
    assert(waiting.transferDescriptor().count()==3 && waiting.currentState()==USB::DMAState::Ready);
    const auto copies=m.api.copies;
    assert(waiting.scheduleIsochronous(m.endpoint,{100,false,false})==Result::Success && m.api.copies==copies);
    assert(waiting.transferDescriptor().data()[1].type()==XHCI::Iso &&
        ((waiting.transferDescriptor().data()[1].control>>20)&2047)==100);
    complete(first);
    assert(intervening.submit(limited,m.tb.ring(),0xabc,7,3,0)==Result::Success);complete(intervening);
    assert(waiting.submit(limited,m.tb.ring(),0xabc,7,4,0)==Result::Success);
    assert(waiting.transferDescriptor().count()==2 && waiting.transferDescriptor().data()[0].type()==XHCI::Iso);
    assert(((waiting.transferDescriptor().data()[0].control>>20)&2047)==100 && !(waiting.transferDescriptor().data()[0].control>>31));
    complete(waiting);assert(!limited.retained());m.finishNative();
}
void recoveryAndCleanup() {
    for (bool isolated:{false,true}) {
        Machine m(true);Request first(m.api),second(m.api);
        IOUSBHostIsochronousTransaction a{},b{};a.offset=3;a.requestCount=8;b.offset=20;b.requestCount=12;
        assert(first.prepareIsochronous(m.client,m.endpoint,a,36,{})==Result::Success);m.api.bus=0x900000;
        assert(second.prepareIsochronous(m.client,m.endpoint,b,36,{})==Result::Success);
        assert(first.submit(m.queue,m.tb.ring(),0xabc,7,1,0)==Result::Success);
        assert(second.submit(m.queue,m.tb.ring(),0xabc,7,2,0)==Result::Success);
        assert(m.queue.quiesce()==Result::Aborted);first.quarantine();second.quarantine();
        assert(first.finish(false)==Result::Busy && second.finish(false)==Result::Busy);
        if (isolated) { m.tb.isolated=true;assert(m.queue.isolate()==Result::Success); }
        else {
            const auto stop=m.issue(XHCI::StopEndpoint);
            const auto dequeue=m.issue(XHCI::SetDequeue,m.tb.ring().nextAddress()|m.tb.ring().producerCycle());
            assert(m.queue.recover(stop,dequeue)==Result::Success);
            for (auto ticket:{stop,dequeue}) { uintptr_t owner=0;assert(m.commands.acknowledge(ticket,0xee,owner)==Result::Success); }
        }
        for (auto *request:{&first,&second}) {
            uintptr_t owner=0;assert(request->retire(m.queue,owner)==Result::Success && owner==0xabc);m.tb.release(owner);
            assert(request->finish(false)==Result::Success && !request->validData());
        }
        m.finishNative();
    }
    Machine m(true,16);Request request(m.api);IOUSBHostIsochronousTransaction t{};
    t.offset=12;t.requestCount=12;t.options=kIsochronousTransactionOptionsWrap;
    assert(request.prepareIsochronous(m.client,m.endpoint,t,36,{})==Result::Success);
    std::memset(m.api.memories.back()->data,0x77,12);
    assert(request.submit(m.queue,m.tb.ring(),0xabc,7,1,0)==Result::Success);m.retire(request,12);
    m.api.clearFailures=1;const auto copies=m.api.copies;
    assert(request.finish(true)==Result::HardwareError && m.api.copies==copies && m.client->references==2);
    assert(request.finish(true)==Result::Success && m.api.copies==copies+2 && m.client->references==1);
    m.finishNative();
}
void missedServicePrefix() {
    Machine m(true,64,true);Request first(m.api),second(m.api),third(m.api);
    IOUSBHostIsochronousTransaction t{};t.requestCount=8;
    for (auto *request:{&first,&second,&third}) {
        assert(request->prepareIsochronous(m.client,m.endpoint,t,36,{})==Result::Success);m.api.bus+=0x10000;
        assert(request->submit(m.queue,m.tb.ring(),0xabc,7,1,0)==Result::Success);
    }
    XHCI::TransferRecord target,record;assert(m.queue.copy(second.ticket(),target));
    assert(target.first!=target.isoFirst && target.isoFirst==m.tb.ring().startAddress());
    auto event=XHCI::trb(target.isoFirst,23u<<24|8,XHCI::TransferEvent<<10|3u<<24|3u<<16);
    assert(m.queue.complete(event,6,record)==Result::Aborted && m.queue.outstanding()==3);
    assert(m.queue.complete(event,7,record)==Result::Success && record.ticket==second.ticket());
    assert(!m.queue.faulted() && m.queue.outstanding()==1);
    for (auto *request:{&first,&second}) {
        assert(m.queue.copy(request->ticket(),record) && record.result==Result::Timeout && record.completionCode==23);
        uintptr_t owner=0;assert(request->retire(m.queue,owner)==Result::Success && owner==0xabc);m.tb.release(owner);
        assert(request->finish(true)==Result::Success && !request->validData() && !request->transferred());
    }
    assert(third.currentState()==USB::DMAState::Owned && m.tb.refs[0xabc]==1);
    // The collapsed event can be delivered again after its payload retired.
    // A physical stale pointer must never release the following TD.
    assert(m.queue.complete(event,7,record)==Result::HardwareError && m.queue.outstanding()==1);
    third.quarantine();m.tb.isolated=true;assert(m.queue.isolate()==Result::Success);
    uintptr_t owner=0;assert(third.retire(m.queue,owner)==Result::Success);m.tb.release(owner);
    assert(third.finish(false)==Result::Success);m.finishNative();
    for (unsigned invalid:{0u,1u,2u}) {
        Machine n(true,64,true);Request a(n.api),b(n.api);
        for (auto *request:{&a,&b}) {
            assert(request->prepareIsochronous(n.client,n.endpoint,t,36,{})==Result::Success);n.api.bus+=0x10000;
            assert(request->submit(n.queue,n.tb.ring(),0xabc,7,1,0)==Result::Success);
        }
        assert(n.queue.copy(b.ticket(),target));
        event=XHCI::trb(invalid==0 ? target.first : invalid==1 ? target.last : target.isoFirst,
            23u<<24|(invalid==2 ? 9 : 8),XHCI::TransferEvent<<10|3u<<24|3u<<16);
        assert(n.queue.complete(event,7,record)==Result::HardwareError && n.queue.outstanding()==2);
        n.tb.isolated=true;assert(n.queue.isolate()==Result::Success);
        for (auto *request:{&a,&b}) {
            request->quarantine();assert(request->retire(n.queue,owner)==Result::Success);n.tb.release(owner);
            assert(request->finish(false)==Result::Success);
        }
        n.finishNative();
    }
}
void multiframeProgress() {
    // Use the same three cursors as the Native worker, the actual DMA owner,
    // and the real endpoint queue. One batch crosses Native request owners;
    // later payloads remain DMA owned while earlier ones publish to clients.
    Machine m(true,1024);XHCI::TransferQueue<TransferBackend,Commands,128> queue(m.tb,m.commands);
    assert(queue.initialize(7,3,m.endpoint,36,0xee)==Result::Success);
    constexpr unsigned perRequest=40,total=120;
    XHCI::IsochronousProgress progress[3]={XHCI::IsochronousProgress(perRequest),
        XHCI::IsochronousProgress(perRequest),XHCI::IsochronousProgress(perRequest)};
    std::vector<std::unique_ptr<Request>> frames;
    unsigned callbacks=0,updates[3]{};
    for (unsigned i=0;i<total;++i) {
        const auto owner=i/perRequest,index=i%perRequest;
        IOUSBHostIsochronousTransaction t{};t.offset=i*8;t.requestCount=8;
        auto frame=std::make_unique<Request>(m.api);
        XHCI::IsoSchedule schedule;
        assert(XHCI::isochronousSchedule(m.endpoint,20+owner*5,index,10,80,56,schedule)==Result::Success);
        assert(!schedule.asap && schedule.frame==20+i/8);
        assert(frame->prepareIsochronous(m.client,m.endpoint,t,36,schedule)==Result::Success);m.api.bus+=0x10000;
        std::memset(m.api.memories.back()->data,int(i+1),8);
        assert(progress[owner].prepare(index));
        assert(frame->submit(queue,m.tb.ring(),0xabc+owner,7,1,0)==Result::Success);
        assert(progress[owner].submit(index));frames.push_back(std::move(frame));
    }
    assert(queue.outstanding()==total && m.tb.refs.size()==3 && m.client->references==total+1);
    assert(!progress[0].publish(1,8) && !progress[1].done() && !callbacks);
    for (unsigned i=0;i<total;++i) {
        const auto owner=i/perRequest,index=i%perRequest;auto &frame=*frames[i];
        XHCI::TransferRecord record;
        assert(queue.complete(m.event(frame.ticket(),8),7,record)==Result::Success);
        uintptr_t released=0;assert(frame.retire(queue,released)==Result::Success && released==0xabc+owner);
        assert(!progress[owner].done());
        assert(frame.finish(true)==Result::Success && frame.currentState()==USB::DMAState::Empty);
        for (unsigned n=0;n<8;++n) assert(static_cast<uint8_t *>(m.client->data)[i*8+n]==i+1);
        ++updates[owner];assert(progress[owner].publish(index,frame.transferred()));m.tb.release(released);
        if (i+1<total) assert(frames[i+1]->currentState()==USB::DMAState::Owned);
        if (progress[owner].done()) { assert(updates[owner]==perRequest && progress[owner].transferred()==perRequest*8);++callbacks; }
    }
    assert(callbacks==3 && !queue.retained());
    for (auto &p:progress) assert(!p.prepare(perRequest) && !p.submit(perRequest) && !p.publish(perRequest,8));
    XHCI::IsochronousProgress empty;assert(empty.done() && !empty.prepare(0));
    XHCI::IsochronousProgress largeBytes(2);
    for (unsigned i=0;i<2;++i) assert(largeBytes.prepare(i) && largeBytes.submit(i) && largeBytes.publish(i,UINT32_MAX));
    assert(largeBytes.transferred()==uint64_t(UINT32_MAX)*2);
    m.finishNative();
}
void multiframeCancellation() {
    Machine m(true,32);std::vector<std::unique_ptr<Request>> frames;
    XHCI::IsochronousProgress progress[2]={XHCI::IsochronousProgress(2),XHCI::IsochronousProgress(2)};
    unsigned updates[2]{},callbacks=0;
    for (unsigned i=0;i<3;++i) {
        const auto owner=i/2,index=i%2;
        IOUSBHostIsochronousTransaction t{};t.offset=i*8;t.requestCount=8;
        auto frame=std::make_unique<Request>(m.api);
        assert(frame->prepareIsochronous(m.client,m.endpoint,t,36,{})==Result::Success);m.api.bus+=0x10000;
        std::memset(m.api.memories.back()->data,0x44,8);
        assert(progress[owner].prepare(index));
        assert(frame->submit(m.queue,m.tb.ring(),0xabc+owner,7,1,0)==Result::Success);
        assert(progress[owner].submit(index));frames.push_back(std::move(frame));
    }
    XHCI::TransferRecord record;
    assert(m.queue.complete(m.event(frames[0]->ticket(),8),7,record)==Result::Success);
    uintptr_t released=0;assert(frames[0]->retire(m.queue,released)==Result::Success);m.tb.release(released);
    assert(frames[0]->finish(true)==Result::Success && progress[0].publish(0,8));++updates[0];
    assert(!progress[0].done() && !progress[1].done() && !callbacks);
    assert(m.queue.quiesce()==Result::Aborted);
    for (unsigned i=1;i<3;++i) { frames[i]->quarantine();assert(frames[i]->finish(false)==Result::Busy); }
    const auto stop=m.issue(XHCI::StopEndpoint);
    const auto dequeue=m.issue(XHCI::SetDequeue,m.tb.ring().nextAddress()|m.tb.ring().producerCycle());
    assert(m.queue.recover(stop,dequeue)==Result::Success);
    for (auto ticket:{stop,dequeue}) assert(m.commands.acknowledge(ticket,0xee,released)==Result::Success);
    for (unsigned i=1;i<3;++i) {
        const auto owner=i/2,index=i%2;
        assert(frames[i]->retire(m.queue,released)==Result::Success && released==0xabc+owner);m.tb.release(released);
        assert(frames[i]->finish(false)==Result::Success && !frames[i]->validData());
        assert(progress[owner].publish(index,0));++updates[owner];
        if (progress[owner].done()) ++callbacks;
    }
    // The fourth Native frame was never prepared or exposed to xHCI. Its
    // cancellation still advances the legacy cursor once, without DMA work.
    assert(progress[1].buffered()==0 && progress[1].prepare(1) && progress[1].submit(1));
    assert(progress[1].publish(1,0));++updates[1];if (progress[1].done()) ++callbacks;
    assert(callbacks==2 && updates[0]==2 && updates[1]==2);
    assert(progress[0].transferred()==8 && !progress[1].transferred());
    for (unsigned i=0;i<32;++i) assert(static_cast<uint8_t *>(m.client->data)[i]==(i<8 ? 0x44 : i));
    m.finishNative();
}
void perFrameUSBErrors() {
    for (unsigned code:{2u,3u,4u,31u}) {
        Machine m(true,64);Request bad(m.api),following(m.api);
        IOUSBHostIsochronousTransaction a{},b{};a.offset=4;a.requestCount=8;b.offset=20;b.requestCount=8;
        assert(bad.prepareIsochronous(m.client,m.endpoint,a,36,{})==Result::Success);
        std::memset(m.api.memories.back()->data,0x11,8);m.api.bus+=0x10000;
        assert(following.prepareIsochronous(m.client,m.endpoint,b,36,{})==Result::Success);
        std::memset(m.api.memories.back()->data,0x22,8);
        for (auto *frame:{&bad,&following}) assert(frame->submit(m.queue,m.tb.ring(),0xabc,7,1,0)==Result::Success);
        XHCI::TransferRecord record;assert(m.queue.copy(bad.ticket(),record));
        auto event=XHCI::trb(record.isoFirst,code<<24|3,XHCI::TransferEvent<<10|3u<<24|3u<<16);
        assert(m.queue.complete(event,7,record)==Result::Success && record.completionCode==code);
        assert(record.transferred==5 && record.result!=Result::Success && record.dataValid && !m.queue.faulted());
        uintptr_t owner=0;assert(bad.retire(m.queue,owner)==Result::Success);m.tb.release(owner);
        assert(bad.validData() && bad.finish(true)==Result::Success && bad.transferred()==5);
        assert(following.currentState()==USB::DMAState::Owned && m.queue.outstanding()==1);
        m.retire(following,8);assert(following.finish(true)==Result::Success);
        for (unsigned i=0;i<64;++i) assert(static_cast<uint8_t *>(m.client->data)[i]==
            (i>=4 && i<9 ? 0x11 : i>=20 && i<28 ? 0x22 : i));
        m.finishNative();
    }
    // Physical-event residue is local to the referenced TRB. It is not a
    // whole-TD residue and does not include later, unexecuted fragments.
    Machine m(true);XHCI::TransferDescriptor<> descriptor;
    XHCI::TransferSegment parts[]={{0x800000,16},{0x910000,16}};
    assert(descriptor.build(m.endpoint,parts,2,32,36,nullptr,{})==Result::Success);
    uint64_t ticket=0;assert(m.queue.submit(descriptor,0xabc,1,0,ticket)==Result::Success);
    XHCI::TransferRecord record;assert(m.queue.copy(ticket,record));
    auto event=XHCI::trb(record.isoFirst+16,4u<<24|6,XHCI::TransferEvent<<10|3u<<24|3u<<16);
    assert(m.queue.complete(event,7,record)==Result::Success && record.transferred==26 && record.dataValid);
    uintptr_t owner=0;assert(m.queue.acknowledge(ticket,0xabc,owner)==Result::Success);m.tb.release(owner);
    XHCI::TRB item;assert(!m.tb.ring().copyPending(record.isoFirst,item));m.finishNative();
    for (bool badResidue:{false,true}) {
        Machine n(true);Request first(n.api),second(n.api);IOUSBHostIsochronousTransaction t{};t.requestCount=8;
        for (auto *frame:{&first,&second}) {
            assert(frame->prepareIsochronous(n.client,n.endpoint,t,36,{})==Result::Success);n.api.bus+=0x10000;
            assert(frame->submit(n.queue,n.tb.ring(),0xabc,7,1,0)==Result::Success);
        }
        assert(n.queue.copy(badResidue ? first.ticket() : second.ticket(),record));
        event=XHCI::trb(record.isoFirst,4u<<24|(badResidue ? 9 : 0),XHCI::TransferEvent<<10|3u<<24|3u<<16);
        assert(n.queue.complete(event,7,record)==Result::HardwareError && n.queue.outstanding()==2);
        n.tb.isolated=true;assert(n.queue.isolate()==Result::Success);
        for (auto *frame:{&first,&second}) {
            frame->quarantine();assert(frame->retire(n.queue,owner)==Result::Success);n.tb.release(owner);
            assert(!frame->validData() && frame->finish(false)==Result::Success);
        }
        n.finishNative();
    }
    for (bool malformed:{false,true}) {
        Machine n(true);assert(descriptor.build(n.endpoint,parts,2,32,36,nullptr,{})==Result::Success);
        assert(n.queue.submit(descriptor,0xabc,1,0,ticket)==Result::Success);
        assert(n.queue.copy(ticket,record));
        event=XHCI::trb(record.isoFirst+16,23u<<24|(malformed ? 17 : 1),XHCI::TransferEvent<<10|3u<<24|3u<<16);
        const auto result=n.queue.complete(event,7,record);
        if (malformed) {
            assert(result==Result::HardwareError && n.queue.outstanding()==1);
            n.tb.isolated=true;assert(n.queue.isolate()==Result::Success);
        } else assert(result==Result::Success && !record.dataValid && !record.transferred && !n.queue.faulted());
        assert(n.queue.acknowledge(ticket,0xabc,owner)==Result::Success);n.tb.release(owner);n.finishNative();
    }
}
void flowScheduling() {
    for (bool cfc:{false,true}) {
        Machine m(true,256);XHCI::IsochronousFlow flow;
        std::vector<std::unique_ptr<Request>> frames;
        for (unsigned i=0;i<24;++i) {
            const auto first=20+i/8,index=i%8;
            XHCI::IsoSchedule schedule;
            assert(flow.plan(m.endpoint,cfc,first,index,10,80,56,schedule)==Result::Success);
            assert(schedule.asap==(!cfc && i!=0));
            assert(schedule.frame==(schedule.asap ? 0 : first));
            IOUSBHostIsochronousTransaction t{};t.offset=i*8;t.requestCount=8;
            auto frame=std::make_unique<Request>(m.api);
            assert(frame->prepareIsochronous(m.client,m.endpoint,t,36,{})==Result::Success);m.api.bus+=0x10000;
            const auto copies=m.api.copies;
            assert(frame->scheduleIsochronous(m.endpoint,schedule)==Result::Success && m.api.copies==copies);
            const auto &descriptor=frame->transferDescriptor();
            assert(bool(descriptor.data()[descriptor.prefix()].control>>31)==schedule.asap);
            assert(frame->submit(m.queue,m.tb.ring(),0xabc+i/8,7,1,0)==Result::Success);
            assert(flow.submitted(m.endpoint,first,index));
            assert(frame->scheduleIsochronous(m.endpoint,{})==Result::NotReady);
            frames.push_back(std::move(frame));
        }
        assert(flow.active() && flow.hasAnchor() && flow.nextTarget()==23*8 && !flow.waitingForEmpty());
        for (unsigned i=0;i<24;++i) {
            XHCI::TransferRecord record;
            assert(m.queue.complete(m.event(frames[i]->ticket(),8),7,record)==Result::Success);
            uintptr_t owner=0;assert(frames[i]->retire(m.queue,owner)==Result::Success && owner==0xabc+i/8);m.tb.release(owner);
            assert(frames[i]->finish(true)==Result::Success);
        }
        // Queue retirement alone does not prove Ring Underrun/Overrun. With
        // IST=0, the next frame can still continue before its first ESIT.
        assert(!m.queue.outstanding() && flow.active());XHCI::IsoSchedule schedule;
        assert(flow.plan(m.endpoint,cfc,23,0,22,183,0,schedule)==Result::Success && schedule.asap==!cfc);
        assert(flow.plan(m.endpoint,cfc,25,0,22,183,0,schedule)==(cfc ? Result::Success : Result::Pending));
        if (!cfc) {
            assert(flow.waitingForEmpty());
            assert(flow.plan(m.endpoint,false,25,0,22,183,0,schedule)==Result::Pending);
        }
        auto empty=XHCI::trb(0,15u<<24,XHCI::TransferEvent<<10|3u<<24|3u<<16);
        XHCI::TransferRecord record;assert(m.queue.complete(empty,7,record)==Result::Pending);
        flow.ended();assert(!flow.active() && !flow.waitingForEmpty());
        assert(flow.plan(m.endpoint,cfc,25,0,22,183,0,schedule)==Result::Success && !schedule.asap && schedule.frame==25);
        assert(flow.plan(m.endpoint,cfc,25,1,22,183,0,schedule)==Result::Timeout);
        m.finishNative();
    }
    Machine m(true);XHCI::IsochronousFlow flow;XHCI::IsoSchedule schedule;
    assert(flow.plan(m.endpoint,false,0,0,8,64,0,schedule)==Result::Success && schedule.asap);
    assert(flow.submitted(m.endpoint,0,0) && !flow.hasAnchor());
    assert(flow.plan(m.endpoint,false,20,0,8,64,0,schedule)==Result::Pending);
    flow.ended();assert(flow.plan(m.endpoint,false,20,0,8,64,0,schedule)==Result::Success && !schedule.asap);
    assert(flow.submitted(m.endpoint,20,0));
    assert(flow.plan(m.endpoint,false,0,0,8,64,0,schedule)==Result::Success && schedule.asap);
    assert(flow.submitted(m.endpoint,0,0) && flow.nextTarget()==162);
    assert(flow.plan(m.endpoint,false,20,2,8,64,0,schedule)==Result::Success && schedule.asap);
    assert(flow.plan(m.endpoint,false,20,0,8,64,0,schedule)==Result::Timeout);
    flow.ended();const uint64_t far=uint64_t(1)<<35;
    assert(flow.plan(m.endpoint,false,far+2,0,far,3,0,schedule)==Result::Success && schedule.frame==2);
    assert(flow.submitted(m.endpoint,far+2,0));
    assert(flow.plan(m.endpoint,false,far+2,1,far,3,0,schedule)==Result::Success && schedule.asap);
    assert(flow.plan(m.endpoint,false,far+2,1,far,8,0,schedule)==Result::Invalid);
    for (bool cfc:{false,true}) {
        flow.ended();auto longInterval=m.endpoint;longInterval.interval=16;
        assert(flow.plan(longInterval,cfc,4096,0,3201,9224,0,schedule)==Result::Success && !schedule.asap);
        assert(flow.submitted(longInterval,4096,0));
        assert(flow.plan(longInterval,cfc,4096,1,3201,9224,0,schedule)==Result::Success && schedule.asap);
        assert(flow.nextTarget()==8192*8);
    }
    m.finishNative();
}
void flowRecoveryAndReadyRebind() {
    Machine m(true,64,true);XHCI::IsochronousFlow flow;XHCI::IsoSchedule schedule;
    Request first(m.api),owned(m.api),ready(m.api);
    IOUSBHostIsochronousTransaction t{};t.requestCount=8;
    for (auto *frame:{&first,&owned,&ready}) {
        assert(frame->prepareIsochronous(m.client,m.endpoint,t,36,{})==Result::Success);m.api.bus+=0x10000;
    }
    for (unsigned i=0;i<2;++i) {
        auto &frame=i ? owned : first;
        assert(flow.plan(m.endpoint,false,20,i,10,80,0,schedule)==Result::Success);
        assert(frame.scheduleIsochronous(m.endpoint,schedule)==Result::Success);
        assert(frame.submit(m.queue,m.tb.ring(),0xabc,7,1,0)==Result::Success);
        assert(flow.submitted(m.endpoint,20,i));
    }
    XHCI::TransferRecord record;assert(m.queue.copy(first.ticket(),record));
    auto missed=XHCI::trb(record.isoFirst,23u<<24|8,XHCI::TransferEvent<<10|3u<<24|3u<<16);
    assert(m.queue.complete(missed,7,record)==Result::Success);
    uintptr_t owner=0;assert(first.retire(m.queue,owner)==Result::Success);m.tb.release(owner);
    assert(first.finish(false)==Result::Success);flow.drain();
    assert(m.queue.quiesce()==Result::Aborted);owned.quarantine();
    assert(owned.finish(false)==Result::Busy && ready.currentState()==USB::DMAState::Ready);
    XHCI::Command stop;stop.type=XHCI::StopEndpoint;stop.slot=3;stop.endpoint=3;
    uint64_t stopped=0;assert(m.commands.submit(stop,0xee,1,1000,stopped)==Result::Success);
    auto forced=XHCI::trb(m.tb.ring().nextAddress(),26u<<24,XHCI::TransferEvent<<10|3u<<24|3u<<16);
    assert(m.queue.stoppedEvent(stopped,forced,7) && m.queue.outstanding()==1);
    assert(!m.queue.stoppedEvent(0,forced,7) && !m.queue.stoppedEvent(stopped,forced,6));
    auto invalid=forced;invalid.lo+=16;assert(!m.queue.stoppedEvent(stopped,invalid,7));
    invalid=forced;invalid.control|=XHCI::EventDataFlag;assert(!m.queue.stoppedEvent(stopped,invalid,7));
    auto link=forced;link.lo=uint32_t(m.tb.ring().startAddress()+m.tb.ring().dataEntries()*16);
    assert(m.queue.stoppedEvent(stopped,link,7));
    XHCI::CommandRecord pending,done;assert(m.commands.copy(stopped,pending));
    assert(m.commands.complete(XHCI::trb(pending.address,1u<<24,XHCI::CommandEvent<<10|3u<<24),7,done)==Result::Success);
    const auto dequeue=m.issue(XHCI::SetDequeue,m.tb.ring().nextAddress()|m.tb.ring().producerCycle());
    assert(m.queue.recover(stopped,dequeue)==Result::Success);
    assert(owned.retire(m.queue,owner)==Result::Success);m.tb.release(owner);assert(owned.finish(false)==Result::Success);
    for (auto ticket:{stopped,dequeue}) assert(m.commands.acknowledge(ticket,0xee,owner)==Result::Success);
    assert(!m.queue.stoppedEvent(stopped,forced,7) && flow.waitingForEmpty());
    assert(flow.plan(m.endpoint,false,22,0,20,160,0,schedule)==Result::Pending);
    // Native uses a verified fresh Drop+Add output context before this queue
    // rebind. The endpoint memory/context proof is exercised by pipe models.
    auto fresh=m.endpoint;fresh.dequeue=m.tb.ring().nextAddress();fresh.cycle=bool(m.tb.ring().producerCycle());
    assert(m.queue.reconfigure(fresh)==Result::Success);flow.ended();
    assert(flow.plan(fresh,false,22,0,20,160,0,schedule)==Result::Success && !schedule.asap);
    const auto copies=m.api.copies;
    auto changed=fresh;changed.maxPacket=512;
    assert(ready.scheduleIsochronous(changed,schedule)==Result::Invalid && m.api.copies==copies);
    assert(ready.scheduleIsochronous(fresh,schedule)==Result::Success && m.api.copies==copies);
    assert(ready.submit(m.queue,m.tb.ring(),0xabc,7,2,0)==Result::Success);
    assert(flow.submitted(fresh,22,0));m.retire(ready,8);assert(ready.finish(true)==Result::Success);
    assert(ready.scheduleIsochronous(fresh,{})==Result::NotReady);m.finishNative();
}
}
void xhciIsochronousTests() {
    schedulingWindow();outputRangesAndWrap();pipelinedInputAndLink();invalidAndZero();capacityRetryRepacking();recoveryAndCleanup();
    missedServicePrefix();multiframeProgress();multiframeCancellation();perFrameUSBErrors();
    flowScheduling();flowRecoveryAndReadyRebind();
}
