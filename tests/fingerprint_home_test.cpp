// SPDX-License-Identifier: BSD-2-Clause
#include <HomeProtocol.h>
#include <HomeWorker.h>
#include <SCMTransport.h>
#include <SecureMonitorAction.h>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace QcomHomeProtocol;
struct SecureMonitor {
    std::vector<FingerprintSCM::Frame> replies,requests;
    unsigned delays=0;
    void invoke(FingerprintSCM::Frame &frame) {
        requests.push_back(frame);
        assert(requests.size()<=replies.size()); frame=replies[requests.size()-1];
    }
    void busyDelay() { ++delays; }
};
static void transport() {
    using FingerprintSCM::Frame;
    const Frame success={{0,0,0xee01,17,0,0,0}};
    uint64_t args[]={17,0xa0000000,64,0xa0000040,64,0xa0000200,32},extra[4]={};
    bool quarantined=false;
    SecureMonitor smc;smc.replies={success};
    auto reply=FingerprintSCM::call(smc,0x30000006,7|(2<<6)|(2<<10)|(2<<14),args,7,extra,0xa0000100,quarantined);
    assert(!reply.status && !reply.result && reply.data==17 && !quarantined);
    const auto &request=smc.requests[0];
    assert(request.x[0]==0x70000006 && request.x[1]==0x8887 && request.x[2]==17);
    assert(request.x[3]==0xa0000000 && request.x[4]==64 && request.x[5]==0xa0000100 && !request.x[6]);
    for (unsigned i=0;i<4;++i) assert(extra[i]==args[i+3]);
    smc={};smc.replies={{{1,123,456,789,12,34,0xcafebabe}},success};
    FingerprintSCM::call(smc,0x30000001,5|(2<<6)|(2<<10),args,5,extra,0xa0000100,quarantined);
    assert(smc.requests[1].x[0]==1 && smc.requests[1].x[6]==0xcafebabe);
    assert(smc.requests[1].x[1]==(5|(2<<6)|(2<<10)) && smc.requests[1].x[2]==args[0]);
    assert(smc.requests[1].x[3]==args[1] && smc.requests[1].x[4]==args[2] && smc.requests[1].x[5]==0xa0000100);
    smc={};const Frame busy={{uint32_t(-12),99,0,0,0,0,111}};smc.replies={busy,success};
    FingerprintSCM::call(smc,0x32000101,3,args,3,extra,0xa0000100,quarantined);
    assert(smc.delays==1 && smc.requests[1].x[0]==0x72000101 && !smc.requests[1].x[6]);
    smc={};smc.replies={busy,busy,busy,busy};
    reply=FingerprintSCM::call(smc,0x32000101,3,args,3,extra,0xa0000100,quarantined);
    assert(reply.status==-12 && smc.requests.size()==4 && smc.delays==3 && !quarantined);
    smc={};smc.replies={{{0,1,0,55,0,0,0}}};
    FingerprintSCM::call(smc,0x02000603,1,args,1,extra,0xa0000100,quarantined);
    assert(!quarantined); // feature version 1 is not a QSEE listener result
    smc={};smc.replies={{{0,2,0xee02,55,0,0,0}}};
    FingerprintSCM::call(smc,0x30000006,7,args,7,extra,0xa0000100,quarantined);
    assert(quarantined);
    extra[0]=0xabcdef;
    reply=FingerprintSCM::call(smc,0x30000006,7,args,7,extra,0xa0000100,quarantined);
    assert(reply.status==-1 && extra[0]==0xabcdef && smc.requests.size()==1);
    quarantined=false;smc={};smc.replies.resize(1024,{{1,0,0,0,0,0,99}});
    reply=FingerprintSCM::call(smc,0x30000001,5,args,5,extra,0xa0000100,quarantined);
    assert(quarantined && reply.status==-1 && smc.requests.size()==1024);
    SecureMonitorAction::Request action;
    assert(!SecureMonitorAction::valid(action));
    action.action=[](void *)->int32_t { return 0; };action.context=&smc;
    assert(SecureMonitorAction::valid(action));
    action.reserved=1; assert(!SecureMonitorAction::valid(action));
}
int main() {
    transport();
    {
        WorkerQueue queue;
        assert(queue.next().kind==WorkerQueue::Kind::None);
        assert(queue.setEnabled(true));
        auto configure=queue.next();
        assert(configure.kind==WorkerQueue::Kind::Configure && configure.enabled);
        assert(queue.current(configure) && queue.queueSample());
        auto sample=queue.next();assert(sample.kind==WorkerQueue::Kind::Sample);
        assert(!queue.samplePending());
        // An IRQ and an expired timer can arrive during a secure query.
        // Neither may cause an unmask before the coalesced rerun is drained.
        for(unsigned n=0;n<2;++n)assert(queue.queueSample());
        assert(queue.current(sample) && queue.samplePending());
        auto rerun=queue.next();assert(rerun.kind==WorkerQueue::Kind::Sample);
        assert(!queue.samplePending() && queue.current(rerun));
        // Power-off during a blocked secure query immediately invalidates it.
        queue.queueSample();assert(queue.samplePending());
        queue.setEnabled(false);assert(!queue.current(sample) && !queue.samplePending());
        auto off=queue.next();assert(off.kind==WorkerQueue::Kind::Configure && !off.enabled);
        assert(!queue.queueSample());
        // Rapid off/on coalesces configuration and rejects the old completion.
        queue.setEnabled(true);assert(!queue.current(off));
        assert(queue.next().enabled);
        for (unsigned n=0;n<100;++n) assert(queue.queueSample());
        assert(queue.next().kind==WorkerQueue::Kind::Sample);
        assert(queue.next().kind==WorkerQueue::Kind::None);
        queue.cancel();assert(!queue.isEnabled() && !queue.current(sample));
        queue.setEnabled(true);configure=queue.next();queue.stop();
        assert(!queue.current(configure) && !queue.setEnabled(true));
        assert(!queue.queueSample() && queue.next().kind==WorkerQueue::Kind::None);
    }
    assert(!goodixProfileSupported(0));
    for (unsigned profile=1;profile<=3;++profile) assert(goodixProfileSupported(profile));
    assert(!goodixProfileSupported(4) && !goodixProfileSupported(0xffffffff));
    bool down=false,changed=false;
    assert(fpcPresence(0,down)==Result::Success && down);
    assert(fpcPresence(1,down)==Result::Success && !down);
    assert(fpcPresence(-12,down)==Result::Busy && !down);
    assert(fpcPresence(-6,down)==Result::Sensor && !down);
    assert(fpcPresence(2,down)==Result::Sensor && !down);
    assert(goodixPresence(0x8080,down,changed)==Result::Success && !down && !changed);
    assert(goodixPresence(0x4002,down,changed)==Result::Success && down && changed);
    assert(goodixPresence(0,down,changed)==Result::Success && down && !changed);
    assert(goodixPresence(4,down,changed)==Result::Success && !down && changed);
    assert(goodixPresence(6,down,changed)==Result::Sensor && !down);
    State state;
    state.update(true,true); assert(state.pending());
    state.update(false,false); assert(state.pending() && state.desired()); // failed report survives empty IRQ
    state.delivered(true); assert(!state.pending());
    state.update(true,true); assert(!state.pending()); // repeated down is not a repeat report
    state.update(false,true); assert(state.pending());
    state.delivered(false); assert(!state.pending());
    state.update(true,true); state.delivered(true);
    state.release(); assert(state.pending() && state.reported() && !state.desired());
    state.delivered(false); assert(!state.pending());
    uint8_t bytes[FPCPayloadSize];
    assert(!fpcRequest(bytes,608,10,2));
    assert(fpcRequest(bytes,sizeof(bytes),10,2));
    assert(read32(bytes)==10 && read32(bytes+4)==2 && read32(bytes+8)==0xffffffffU);
    for (unsigned i=12;i<sizeof(bytes);++i) assert(!bytes[i]);
    assert(goodixEnvelope(bytes,64,7,1016,0xa0101000,GoodixIRQSize,123456));
    assert(read32(bytes)==7 && read32(bytes+4)==1 && read32(bytes+16)==1016);
    assert(read32(bytes+20)==0xa0101000 && read32(bytes+24)==223776);
    assert(read32(bytes+8)==123456 && !read32(bytes+12));
    assert(!goodixEnvelope(bytes,63,7,1016,0xa0101000,GoodixIRQSize,123456));
    assert(!goodixEnvelope(bytes,64,7,1016,0,GoodixIRQSize,123456));
    unsigned page=0,size=0,count=0,bits=0,depth=0; std::vector<unsigned> usages,controls;
    for (unsigned pos=0;pos<sizeof(ReportDescriptor);) {
        unsigned prefix=ReportDescriptor[pos++],length=prefix&3,value=0;
        if (length==3) length=4;
        assert(pos+length<=sizeof(ReportDescriptor));
        for (unsigned i=0;i<length;++i) value|=unsigned(ReportDescriptor[pos++])<<(8*i);
        unsigned type=(prefix>>2)&3,tag=prefix>>4;
        if (type==1) { if (tag==0) page=value; if (tag==7) size=value; if (tag==9) count=value; }
        else if (type==2 && tag==0) usages.push_back(value);
        else if (type==0) {
            if (tag==10) { assert(page==12 && usages==std::vector<unsigned>{1} && value==1); ++depth; }
            if (tag==12) { assert(depth); --depth; }
            if (tag==8) {
                if (!(value&1)) { assert(value==2 && page==12 && size==1 && count==usages.size()); controls.insert(controls.end(),usages.begin(),usages.end()); }
                bits+=size*count;
            }
            usages.clear();
        }
    }
    assert(!depth && bits==8 && controls==std::vector<unsigned>{0x40});
    puts("Fingerprint Home: FPC lost/present polarity, Goodix IRQ filtering, failed-report retry, release and native HID Menu descriptor passed");
}
