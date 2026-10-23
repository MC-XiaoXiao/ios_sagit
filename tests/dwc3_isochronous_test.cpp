// SPDX-License-Identifier: BSD-2-Clause
#include "DWC3Isochronous.h"
#include "USBDMA.h"
#include "USBDeviceRequests.h"
#include "USBDeviceIsochronous.h"
#include "usb_memory_model.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

using USB::Result;
struct Controller {
    struct Call { DWC3::Command command;unsigned endpoint;uint64_t address;uint32_t parameter; };
    std::vector<Call> calls;
    unsigned resource=7,raw=0;
    bool halted=false;
    Result startResult=Result::Success,updateResult=Result::Success,endResult=Result::Pending;
    unsigned endpointCount() const { return 16; }
    unsigned dmaBits() const { return 40; }
    bool stopped() const { return halted; }
    Result currentFrame(uint32_t &frame) { frame=raw;return Result::Success; }
    Result startIsochronous(unsigned ep,uint64_t address,uint32_t frame,uint32_t &reply) {
        calls.push_back({DWC3::Command::Start,ep,address,frame});reply=resource<<16;return startResult;
    }
    Result command(unsigned ep,DWC3::Command command,uint32_t a,uint32_t b,uint32_t c,uint32_t options,uint32_t &reply) {
        assert(!a && !b && !c && ((options>>16)&127)==resource);
        if (command==DWC3::Command::End) assert((options&(DWC3::CommandForce|DWC3::CommandInterrupt))==(DWC3::CommandForce|DWC3::CommandInterrupt));
        else assert(command==DWC3::Command::Update);
        calls.push_back({command,ep,0,options});reply=resource<<16;
        return command==DWC3::Command::End ? endResult : updateResult;
    }
};
struct Order {
    unsigned reads=0,writes=0;
    void dmaReadBarrier() { ++reads; }
    void dmaWriteBarrier() { ++writes; }
};
using Ring=DWC3::IsochronousRing<Controller,Order>;
DWC3::Event ready(unsigned ep,unsigned frame) { return {ep<<1|3u<<6|frame<<16}; }
DWC3::Event progress(unsigned ep,unsigned frame=0,unsigned status=0) { return {ep<<1|2u<<6|status<<12|frame<<16}; }
DWC3::Event ended(unsigned ep,unsigned status=0) { return {ep<<1|7u<<6|status<<12|8u<<24}; }
struct Fixture {
    Controller core;Order order;
    alignas(64) std::array<DWC3::TRB,8> trbs{};
    Ring ring{core,order};
    DWC3::EndpointConfiguration config;
    Fixture(bool in=true,unsigned entries=8):config{in ? 3u:2u,DWC3::EndpointType::Isochronous,USB::Speed::High,1024,1,0,2} {
        assert(ring.initialize(config,trbs.data(),0x100000,entries,4)==Result::Success);
    }
    unsigned endpoint() const { return config.physicalEndpoint; }
    void begin(unsigned frame=100) {
        core.raw=frame&0x3fff;
        assert(ring.handle(ready(endpoint(),frame),4,100)==Result::Success);
        assert(ring.start(100)==Result::Success && ring.active());
        assert(core.calls.back().parameter==((frame+4)&0xffff));
    }
    void complete(unsigned slot,unsigned bytes,unsigned frame,unsigned status=0) {
        assert(slot<7 && (trbs[slot].control&DWC3::Owned));
        auto programmed=trbs[slot].size&DWC3::TRBLengthMask;
        assert(bytes<=programmed);
        trbs[slot].size=(trbs[slot].size&(3u<<24))|(programmed-bytes)|(status<<28);
        trbs[slot].control=(trbs[slot].control&~(DWC3::Owned|(0xffffu<<14)))|(frame<<14);
    }
};

static void schedulingAndCapabilities() {
    DWC3::EndpointConfiguration config{3,DWC3::EndpointType::Isochronous,USB::Speed::High,1024,1,0,2};
    assert(config.valid() && config.bytesPerInterval()==3072 && config.intervalTicks()==1);
    assert((config.parameter1()&(7u<<8))==(7u<<8));
    uint32_t target=0;
    assert(DWC3::isochronousStartFrame(config,0x3ffe,2,10,100,target)==Result::Success && target==0x4006);
    assert(DWC3::isochronousStartFrame(config,0xfffe,2,10,100,target)==Result::Success && target==6);
    assert(DWC3::isochronousStartFrame(config,0,0,100,99,target)==Result::Timeout);
    assert(DWC3::isochronousStartFrame(config,0,0,0,0x4000*125,target)==Result::Timeout);
    assert(DWC3::isochronousStartFrame(config,0x10000,0,0,0,target)==Result::Invalid);
    assert(DWC3::isochronousStartFrame(config,0,0x4000,0,0,target)==Result::Invalid);
    config.interval=4;
    assert(DWC3::isochronousStartFrame(config,100,101,0,10,target)==Result::Success && target==104);
    config.interval=16;
    assert(config.valid() && ((config.parameter1()>>16)&255)==13);
    assert(DWC3::isochronousStartFrame(config,0x4000,0x3fff,0,0,target)==Result::Success && target==0x8000);
    config.speed=USB::Speed::Full;config.mult=0;config.maximumPacketSize=1023;config.interval=1;
    assert(config.valid());
    assert(DWC3::isochronousStartFrame(config,100,900,0,10,target)==Result::Success && target==101);
    config.maximumPacketSize=1024;assert(!config.valid());
    config.maximumPacketSize=1023;config.mult=1;assert(!config.valid());
    config.mult=0;config.interval=0;assert(!config.valid());
    config.speed=USB::Speed::Super;config.interval=2;config.burst=15;config.mult=2;config.maximumPacketSize=1024;
    assert(config.valid() && config.bytesPerInterval()==49152);
    config.mult=3;assert(!config.valid());
    config.mult=0;config.type=DWC3::EndpointType::Bulk;config.interval=0;assert(config.valid());
    config.mult=1;assert(!config.valid());
    DWC3::EndpointConfiguration native{};
    USB::Speed negotiated=USB::Speed::Unknown;
    for (auto maximum:{USB::Speed::High,USB::Speed::Super}) {
        for (unsigned value=0;value<8;++value) {
            const bool valid=value==0 || value==1 || value==3 || (value==4 && maximum==USB::Speed::Super);
            assert(USB::deviceConnectionSpeed(maximum,value,negotiated)==(valid ? Result::Success:Result::Unsupported));
        }
        assert(USB::deviceConnectionSpeed(maximum,UINT32_MAX,negotiated)==Result::HardwareError);
        assert(USB::deviceConnectionSpeed(maximum,1,negotiated)==Result::Success && negotiated==USB::Speed::Full);
        assert(USB::deviceConnectionSpeed(maximum,0,negotiated)==Result::Success && negotiated==USB::Speed::High);
    }
    assert(USB::deviceConnectionSpeed(USB::Speed::Super,4,negotiated)==Result::Success && negotiated==USB::Speed::Super);
    assert(USB::deviceConnectionSpeed(USB::Speed::Low,0,negotiated)==Result::Invalid);
    assert(USB::deviceEndpointConfiguration(2,1,USB::Speed::High,3072,1,native)==Result::Success);
    assert(native.maximumPacketSize==1024 && native.mult==2 && native.bytesPerInterval()==3072);
    assert(USB::deviceEndpointConfiguration(3,1,USB::Speed::High,1500,4,native)==Result::Success);
    assert(native.maximumPacketSize==750 && native.mult==1 && native.interval==4);
    assert(USB::deviceEndpointConfiguration(3,1,USB::Speed::High,3071,1,native)==Result::Invalid);
    assert(USB::deviceEndpointConfiguration(3,257,USB::Speed::High,64,1,native)==Result::Invalid);
    assert(USB::deviceEndpointConfiguration(3,-255,USB::Speed::High,64,1,native)==Result::Invalid);
    for (unsigned n=1;n<=16;++n) {
        assert(USB::deviceEndpointConfiguration(2,1,USB::Speed::Full,1023,n,native)==Result::Success);
        assert(native.interval==n && native.intervalTicks()==(1u<<(n-1)));
    }
    assert(USB::deviceEndpointConfiguration(2,1,USB::Speed::Full,1023,17,native)==Result::Invalid);
    assert(USB::deviceEndpointConfiguration(2,1,USB::Speed::Full,1024,1,native)==Result::Invalid);
    for (unsigned property=0;property<3;++property) {
        assert(USB::deviceEndpointConfiguration(3,1,USB::Speed::High,3072,1,native,property)==Result::Success);
        assert(native.maximumPacketSize==1024 && native.mult==2 && !native.burst);
        assert(USB::deviceEndpointConfiguration(2,1,USB::Speed::Full,1023,4,native,property)==Result::Success);
        assert(native.interval==4 && native.intervalTicks()==8 && !native.mult && !native.burst);
        assert(USB::deviceEndpointConfiguration(3,1,USB::Speed::Super,1024,1,native,property)==Result::Success);
        assert(native.maximumPacketSize==1024 && !native.mult && !native.burst && native.bytesPerInterval()==1024);
        assert(USB::deviceEndpointConfiguration(3,2,USB::Speed::Super,1024,0,native,property)==Result::Success);
        assert(native.maximumPacketSize==1024 && !native.mult && native.burst==15);
    }
    assert(USB::deviceEndpointConfiguration(3,1,USB::Speed::High,64,1,native,3)==Result::Unsupported);
    assert(USB::deviceEndpointConfiguration(3,1,USB::Speed::Super,64,1,native,3)==Result::Unsupported);
    assert(USB::deviceEndpointConfiguration(3,2,USB::Speed::Super,1024,0,native)==Result::Success);
    assert(native.burst==15 && !native.mult && ((native.parameter0(false)>>22)&15)==15);
    assert(USB::deviceEndpointConfiguration(2,1,USB::Speed::Super,1024,16,native)==Result::Success);
    assert(!native.burst && !native.mult && native.bytesPerInterval()==1024);
    assert(USB::deviceEndpointConfiguration(2,1,USB::Speed::Super,3072,1,native)==Result::Invalid);
    for (unsigned speed=0;speed<2;++speed) {
        const auto maximum=speed ? USB::Speed::Super:USB::Speed::High;
        int packet=-1;
        assert(USB::devicePipeParameters(maximum,1,1,packet,1,1)==Result::Success);
        assert(packet==(speed ? 1024:-1));
        packet=3072;
        assert(USB::devicePipeParameters(maximum,1,0,packet,1,0)==
            (speed ? Result::Unsupported:Result::Success));
        assert(packet==3072);
    }
    int packet=3071;
    assert(USB::devicePipeParameters(USB::Speed::High,1,0,packet,1,0)==Result::Invalid);
    packet=1024;
    assert(USB::devicePipeParameters(USB::Speed::Super,1,0,packet,0,0)==Result::Invalid);
    assert(USB::devicePipeParameters(USB::Speed::Super,1,0,packet,17,0)==Result::Invalid);
    assert(USB::devicePipeParameters(USB::Speed::High,1,0,packet,9,0)==Result::Unsupported);
    assert(USB::devicePipeParameters(USB::Speed::Super,1,0,packet,16,0)==Result::Unsupported);
    // Pinned Native copyDescriptor/getInterval pairs for creation intervals
    // 1..8. FS bInterval4 describes8 ms, despite the helper's arithmetic
    // producing4; the hardware must follow the descriptor's wire meaning.
    const unsigned fullIntervals[]={1,1,1,1,2,4,8,16};
    for (unsigned i=0;i<8;++i) {
        assert(USB::devicePipeParameters(USB::Speed::High,1,0,packet,int(i+1),0)==Result::Success);
        assert(USB::deviceEndpointConfiguration(2,1,USB::Speed::Full,1023,fullIntervals[i],native)==Result::Success);
        assert(((native.parameter1()>>16)&255)==(fullIntervals[i]>14 ? 13:fullIntervals[i]-1));
    }
    assert(USB::devicePipeParameters(USB::Speed::Super,1,2,packet,1,0)==Result::Invalid);
    assert(USB::devicePipeParameters(USB::Speed::Super,1,0,packet,1,4)==Result::Invalid);
    assert(USB::devicePipeParameters(USB::Speed::Super,2,0,packet,0,1)==Result::Invalid);
    assert(USB::devicePipeParameters(USB::Speed::Super,2,0,packet,0,0)==Result::Success);
    packet=64;
    assert(USB::devicePipeParameters(USB::Speed::High,2,0,packet,0,0)==Result::Invalid);
    assert(USB::devicePipeParameters(USB::Speed::Super,0,0,packet,0,0)==Result::Invalid);
    assert(USB::devicePipeParameters(USB::Speed::High,0,0,packet,0,0)==Result::Success);
    packet=-2;
    assert(USB::devicePipeParameters(USB::Speed::High,1,0,packet,1,0)==Result::Invalid);
}

static void pipelineAndReuse() {
    Fixture f;
    assert(f.ring.push(0x200000,1500,1500,1)==Result::Success);
    assert(((f.trbs[0].size>>24)&3)==1 && !(f.trbs[0].control&(DWC3::Last|DWC3::Chain)));
    assert(((f.trbs[7].control>>4)&63)==unsigned(DWC3::TRBType::Link) && f.trbs[7].addressLo==0x100000);
    assert(f.ring.start(0)==Result::NotReady && f.core.calls.empty());
    f.begin();
    unsigned expected=0;
    for (unsigned n=1;n<=1000;++n) {
        const auto slot=(n-1)%7;
        DWC3::IsochronousCompletion done{};
        assert(f.ring.takeCompletion(n,done)==Result::Pending);
        if (n<1000) assert(f.ring.push(0x200000+n*0x10000,1500,1500,n+1)==Result::Success);
        f.complete(slot,1500,100+n);
        assert(f.ring.handle(progress(f.endpoint(),100+n),4,100+n)==Result::Success);
        assert(f.ring.takeCompletion(n,done)==Result::Success && done.cookie==n && done.epoch==4 && done.bytes==1500 && done.result==Result::Success);
        assert(f.ring.takeCompletion(n,done)==Result::Invalid);
        ++expected;
    }
    assert(expected==1000 && !f.ring.pending() && f.ring.ownsHardware());
    assert(f.ring.idle()==Result::Pending && f.ring.stopping());
    assert(f.ring.push(0x200000,1,1,1001)==Result::NotReady);
    assert(f.ring.handle(ended(f.endpoint()),4,2000)==Result::Success && !f.ring.ownsHardware());
    assert(f.ring.push(0x200000,1,1,1001)==Result::Success);
    f.begin(2000);
    assert(f.core.calls.back().address==0x100000+(1000%7)*sizeof(DWC3::TRB));
}

static void outShortMissedAndOverrun() {
    Fixture f(false);
    for (unsigned n=0;n<3;++n) assert(f.ring.push(0x200000+n*0x1000,73,1024,n+1)==Result::Success);
    assert(f.trbs[0].control&DWC3::ContinueShort);
    f.begin();
    f.complete(0,25,104);f.complete(1,0,105,1);f.complete(2,100,106);
    assert(f.ring.handle(progress(2,105,8),4,200)==Result::Success);
    DWC3::IsochronousCompletion done{};
    assert(f.ring.takeCompletion(1,done)==Result::Success && done.result==Result::Success && done.bytes==25 && done.frame==104);
    assert(f.ring.takeCompletion(2,done)==Result::Success && done.result==Result::MissedService && !done.bytes);
    assert(f.ring.takeCompletion(3,done)==Result::Success && done.result==Result::Overflow && done.bytes==73);
    assert(!f.ring.pending());
}

static void cancelAndCommandFailures() {
    for (bool started:{false,true}) {
        Fixture f;
        assert(f.ring.push(0x200000,512,512,1)==Result::Success);
        if (started) f.begin();
        DWC3::IsochronousCompletion done{};
        assert(f.ring.cancel(Result::Timeout)==(started ? Result::Pending : Result::Success));
        if (started) {
            assert(f.ring.takeCompletion(1,done)==Result::Pending && f.ring.ownsHardware());
            f.complete(0,512,104);
            assert(f.ring.handle(progress(3),4,201)==Result::Pending);
            assert(f.ring.takeCompletion(1,done)==Result::Pending);
            assert(f.ring.handle(ready(3,120),4,201)==Result::Pending);
            assert(f.ring.handle(ended(3),4,202)==Result::Success);
            assert(f.ring.start(203)==Result::NotReady); // ready before END was stale
        }
        assert(f.ring.takeCompletion(1,done)==Result::Success && done.result==Result::Timeout && !done.bytes);
    }
    for (unsigned failure=0;failure<4;++failure) {
        Fixture f;
        assert(f.ring.push(0x200000,512,512,1)==Result::Success);
        if (!failure) {
            f.core.startResult=Result::Timeout;
            assert(f.ring.handle(ready(3,100),4,100)==Result::Success);
            assert(f.ring.start(100)==Result::Timeout);
        } else {
            f.begin();
            if (failure==1) {
                f.core.updateResult=Result::HardwareError;
                assert(f.ring.push(0x210000,512,512,2)==Result::HardwareError);
            } else if (failure==2) {
                f.core.endResult=Result::Timeout;
                assert(f.ring.cancel()==Result::Timeout);
            } else {
                assert(f.ring.cancel()==Result::Pending);
                assert(f.ring.handle(ended(3,1),4,202)==Result::HardwareError);
            }
        }
        assert(f.ring.faulted() && f.ring.ownsHardware());
        assert(f.ring.reclaimAfterHalt()==Result::NotReady);
        f.core.halted=true;
        assert(f.ring.reclaimAfterHalt()==Result::Success && !f.ring.ownsHardware());
        DWC3::IsochronousCompletion done{};
        assert(f.ring.takeCompletion(1,done)==Result::Success && done.result==Result::HardwareError);
        if (failure==1) assert(f.ring.takeCompletion(2,done)==Result::Success && done.result==Result::HardwareError);
    }
}

static void busExpiry() {
    Fixture f;
    assert(f.ring.push(0x200000,512,512,1)==Result::Success);
    f.core.startResult=Result::BusExpired;
    DWC3::IsochronousCompletion done{};
    for (unsigned n=0;n<6;++n) {
        assert(f.ring.handle(ready(3,100+n*8),4,100+n)==Result::Success);
        assert(f.ring.start(100+n)==Result::Pending && f.ring.stopping());
        assert(f.ring.takeCompletion(1,done)==Result::Pending);
        assert(f.ring.handle(ended(3),4,101+n)==Result::Success);
        assert(f.ring.takeCompletion(1,done)==(n==5 ? Result::Success : Result::Pending));
    }
    assert(done.result==Result::Timeout && !f.ring.pending() && !f.ring.ownsHardware());
    Fixture retry;
    assert(retry.ring.push(0x200000,512,512,1)==Result::Success);
    retry.core.startResult=Result::BusExpired;
    assert(retry.ring.handle(ready(3,100),4,100)==Result::Success && retry.ring.start(100)==Result::Pending);
    // A user abort supersedes a pending internal timing retry.
    assert(retry.ring.cancel()==Result::Pending);
    assert(retry.ring.handle(ended(3),4,101)==Result::Success);
    assert(retry.ring.takeCompletion(1,done)==Result::Success && done.result==Result::Aborted);
}

static void malformedAndAdmission() {
    Fixture f;
    assert(f.ring.push(0x100000,512,512,1)==Result::Invalid);
    assert(f.ring.push(0x200000,3073,3073,1)==Result::Invalid);
    assert(f.ring.push(0x200000,512,512,0)==Result::Invalid);
    assert(f.ring.push(0x200000,512,513,1)==Result::Invalid);
    assert(f.ring.push(0x200000,512,512,1)==Result::Success);
    assert(f.ring.push(0x210000,512,512,1)==Result::Busy);
    assert(f.ring.handle(ready(2,100),4,100)==Result::Invalid);
    assert(f.ring.handle(ready(3,100),5,100)==Result::Invalid);
    f.begin();
    for (unsigned mode=0;mode<4;++mode) {
        Fixture bad;
        assert(bad.ring.push(0x200000,512,512,1)==Result::Success);bad.begin();bad.complete(0,512,104);
        if (!mode) bad.trbs[0].addressLo+=16;
        if (mode==1) bad.trbs[0].size=513;
        if (mode==2) bad.trbs[0].control|=DWC3::Last;
        if (mode==3) bad.trbs[0].size=2u<<28;
        assert(bad.ring.handle(progress(3),4,200)==Result::HardwareError && bad.ring.ownsHardware());
        DWC3::IsochronousCompletion done{};
        assert(bad.ring.takeCompletion(1,done)==Result::Pending);
        bad.core.halted=true;assert(bad.ring.reclaimAfterHalt()==Result::Success);
    }
    Fixture zero;
    assert(zero.ring.push(0,0,0,1)==Result::Success);zero.begin();zero.complete(0,0,104);
    assert(zero.ring.handle(progress(3),4,200)==Result::Success);
    Fixture out(false);
    assert(out.ring.push(0,0,0,1)==Result::Invalid);
    assert(out.ring.push(0x200000,0,1024,1)==Result::Success);
}

static void packedFrameDMA() {
    MemoryAPI api;auto *client=api.allocate(128,64);assert(client);
    auto *bytes=static_cast<uint8_t *>(client->data);
    for (unsigned n=0;n<128;++n) bytes[n]=0xa5;
    USB::DeviceBounce<MemoryAPI> first(api),second(api);
    assert(first.prepare(client,13,true,64,0)==Result::Success);
    assert(second.prepare(client,19,true,64,13)==Result::Success);
    assert(first.capacity()==64 && second.capacity()==64 && client->prepares==2);
    assert(first.publish(4)==Result::Success && second.publish(4)==Result::Success);
    std::fill_n(static_cast<uint8_t *>(first.bytes()),64,0x11);
    std::fill_n(static_cast<uint8_t *>(second.bytes()),64,0x22);
    assert(first.finish(8,true)==Result::Busy);
    assert(first.retire(4)==Result::Success && first.finish(8,true)==Result::Success);
    // Cancellation cannot copy an uncompleted OUT frame into the packed MD.
    assert(second.finish(0,false)==Result::Busy);second.retireAfterIsolation();
    assert(second.finish(0,false)==Result::Success);
    for (unsigned n=0;n<128;++n) assert(bytes[n]==(n<8 ? 0x11 : 0xa5));
    assert(!client->prepares);
    assert(second.prepare(client,19,false,64,13)==Result::Success);
    for (unsigned n=0;n<19;++n) assert(static_cast<uint8_t *>(second.bytes())[n]==0xa5);
    assert(second.finish(0,false)==Result::Success);
    assert(second.prepare(client,20,true,64,109)==Result::Invalid);
    assert(second.prepare(client,1,true,64,UINT64_MAX)==Result::Invalid);
    api.releaseMemory(client);
}

static void periodicRequestPipeline() {
    Fixture f(false);MemoryAPI api;
    auto *client=api.allocate(128,64);assert(client);
    std::memset(client->data,0xa5,128);
    USB::DeviceRequests<MemoryAPI,4> requests(api);
    USB::DeviceBounce<MemoryAPI> first(api),second(api);
    assert(requests.attach(0,&first) && requests.attach(1,&second));
    uint64_t a=0,b=0,invalid=0;unsigned context=7;
    assert(requests.admit(2,client,73,1024,4,0,&context,a,0,1000)==Result::Success);
    assert(requests.admit(3,client,55,1024,4,0,&context,invalid,73,1000)==Result::Invalid);
    assert(requests.admit(2,client,55,1024,4,0,&context,b,73,1001)==Result::Success);
    using Requests=decltype(requests);
    for (unsigned n=0;n<2;++n) {
        Requests::Work ticket;assert(requests.beginWork(ticket));
        assert(ticket.endpoint==2 && ticket.offset==(n ? 73u : 0u));
        api.bus=0x200000+n*0x1000;
        assert(requests.commit(ticket,requests.execute(ticket)));
    }
    Requests::View one{},two{};
    // Both frames are prepared before the first service interval starts.
    assert(requests.preparedRequest(a,one) && requests.preparedRequest(b,two));
    assert(one.capacity==1024 && two.capacity==1024);
    for (auto view:{one,two}) {
        assert(requests.publish(view.cookie,4)==Result::Success);
        assert(f.ring.push(view.bus,view.length,view.capacity,view.cookie)==Result::Success);
    }
    f.begin();
    std::memset(first.bytes(),0x11,1024);std::memset(second.bytes(),0x22,1024);
    f.complete(0,32,104);
    assert(f.ring.handle(progress(2,104),4,200)==Result::Success);
    DWC3::IsochronousCompletion hardware{};
    assert(f.ring.takeCompletion(a,hardware)==Result::Success);
    assert(requests.completed(a,4,hardware.result,hardware.bytes)==Result::Success);
    Requests::Work cleanup;assert(requests.beginWork(cleanup) && !cleanup.preparing);
    assert(requests.commit(cleanup,requests.execute(cleanup)));
    Requests::Completion completion;
    assert(requests.takeCompletion(a,completion)==Result::Success && completion.bytes==32 && completion.context==&context);
    assert(requests.cancel(b) && f.ring.cancel()==Result::Pending);
    assert(!requests.beginWork(cleanup)); // neither timer nor cancellation unmaps live DMA
    assert(f.ring.handle(ended(2),4,201)==Result::Success);
    assert(f.ring.takeCompletion(b,hardware)==Result::Success && hardware.result==Result::Aborted);
    assert(requests.completed(b,4,hardware.result,0)==Result::Success);
    assert(requests.beginWork(cleanup) && requests.commit(cleanup,requests.execute(cleanup)));
    assert(requests.takeCompletion(b,completion)==Result::Success && completion.result==Result::Aborted);
    auto *bytes=static_cast<uint8_t *>(client->data);
    for (unsigned n=0;n<128;++n) assert(bytes[n]==(n<32 ? 0x11 : 0xa5));
    assert(requests.empty() && !client->prepares);
    api.releaseMemory(client);
}

struct AggregateFixture {
    using Queue=USB::DeviceRequests<MemoryAPI,4>;
    using Iso=USB::DeviceIsochronous<MemoryAPI,4,4>;
    MemoryAPI api;
    Queue dma{api};
    Iso requests{api,dma};
    using Bounce=USB::DeviceBounce<MemoryAPI>;
    Bounce buffers[4]{Bounce(api),Bounce(api),Bounce(api),Bounce(api)};
    Fixture hardware{false};
    MemoryAPI::Memory *client=api.allocate(16384,64);
    AggregateFixture() {
        std::memset(client->data,0xa5,client->size);api.bus=0x200000;
        for (unsigned n=0;n<4;++n) assert(dma.attach(n,&buffers[n]));
    }
    ~AggregateFixture() { assert(requests.empty() && !client->prepares);api.releaseMemory(client); }
    void work() {
        Queue::Work ticket;
        while (dma.beginWork(ticket)) {
            api.bus+=0x10000;
            assert(dma.commit(ticket,dma.execute(ticket)));
        }
        assert(requests.reap()==Result::Success);
    }
    void fill() {
        assert(requests.prefetch()==Result::Success);work();
        assert(requests.publish(2,hardware.ring,4)==Result::Success);
    }
    void collect(unsigned raw=110) {
        assert(requests.collect(2,hardware.ring,4,100000000,raw,125000,true)==Result::Success);
    }
    void cancel(Result reason=Result::Aborted,bool halt=false) {
        requests.cancel(2,reason);
        if (halt) {
            hardware.core.halted=true;assert(hardware.ring.reclaimAfterHalt(reason)==Result::Success);
            assert(requests.collect(2,hardware.ring,4,0,0,0,false,true)==Result::Success);dma.isolated();
        } else {
            auto result=hardware.ring.cancel(reason);
            assert(result==Result::Success || result==Result::Pending);
            if (result==Result::Pending) assert(hardware.ring.handle(ended(2),4,200)==Result::Success);
            collect();
        }
        work();
    }
    void finishIdle() {
        auto result=hardware.ring.idle();
        assert(result==Result::Success || result==Result::Pending);
        if (result==Result::Pending) assert(hardware.ring.handle(ended(2),4,200)==Result::Success);
    }
};

static void aggregateBoundedFIFO() {
    AggregateFixture f;using Iso=AggregateFixture::Iso;
    Iso::Request one{},two{};Iso::Frame first[9]{},second[3]{};
    for (auto &frame:first) frame.requested=17;
    for (auto &frame:second) frame.requested=19;
    int contexts[2]{1,2};
    assert(f.requests.admit(one,first,9,f.client,2,1024,3072,7,0,&contexts[0])==Result::Success);
    assert(f.requests.admit(two,second,3,f.client,2,1024,3072,7,0,&contexts[1])==Result::Success);
    assert(first[8].offset==136 && second[0].offset==0 && second[2].offset==38);
    f.fill();f.hardware.begin();
    assert(one.prepare==4 && one.publish==4 && !two.prepare);
    unsigned slot=0;
    for (unsigned frame=0;frame<12;++frame) {
        f.hardware.complete(slot,frame<9 ? 17:19,104+frame);
        assert(f.hardware.ring.handle(progress(2),4,200)==Result::Success);
        f.collect(110+frame);
        // Hardware completion alone does not publish an aggregate callback.
        assert(!f.requests.notice());f.work();
        if (frame==8) {
            assert(f.requests.notice()==one.serial);
            f.requests.announce(one.serial);Result result;
            assert(f.requests.take(one.serial,result)==&one && result==Result::Success);
            assert(one.context==&contexts[0]);
        }
        f.fill();slot=(slot+1)%7;
    }
    assert(f.requests.notice()==two.serial);Result result;
    assert(f.requests.take(two.serial,result)==&two && result==Result::Success);
    assert(two.context==&contexts[1] && !f.requests.contains(two.serial));f.finishIdle();
    assert(f.api.allocationCalls==13); // one client plus at most four live frame allocations
}

static void aggregateOUTAndCancellation() {
    AggregateFixture f;using Iso=AggregateFixture::Iso;
    Iso::Request one{};Iso::Frame frames[6]{};
    for (auto &frame:frames) frame.requested=17;
    unsigned context=1;
    assert(f.requests.admit(one,frames,6,f.client,2,1024,3072,7,1000,&context)==Result::Success);
    f.fill();f.hardware.begin();
    for (unsigned n=0;n<4;++n) std::memset(f.buffers[n].bytes(),0x11+n,1024);
    f.hardware.complete(0,10,104);f.hardware.complete(1,32,105);f.hardware.complete(2,0,106,1);
    assert(f.hardware.ring.handle(progress(2),4,200)==Result::Success);f.collect();
    f.requests.expire(1000);
    assert(f.hardware.ring.cancel(Result::Timeout)==Result::Pending);
    f.work();
    assert(frames[0].result==Result::Success && frames[0].actual==10 && frames[0].timestampNS==99250000);
    assert(frames[1].result==Result::Overflow && frames[1].actual==17 && frames[1].timestampNS==99375000);
    assert(frames[2].result==Result::MissedService && !frames[2].actual && !frames[2].timestampNS);
    assert(frames[4].result==Result::Timeout && frames[5].result==Result::Timeout);
    assert(!f.requests.notice() && !frames[3].complete);
    // No completion and no unmap of the fourth DMA frame before actual END.
    assert(f.client->prepares==1);
    assert(f.hardware.ring.handle(ended(2),4,201)==Result::Success);f.collect();f.work();
    assert(f.requests.notice()==one.serial);Result result;
    assert(f.requests.take(one.serial,result)==&one && result==Result::Timeout);
    const auto *bytes=static_cast<uint8_t *>(f.client->data);
    for (unsigned n=0;n<102;++n) assert(bytes[n]==(n<10 ? 0x11 : n>=17 && n<34 ? 0x12:0xa5));
}

static void aggregateMappingAndHalt() {
    AggregateFixture f;using Iso=AggregateFixture::Iso;
    Iso::Request one{},two{};Iso::Frame frames[5]{},tail[2]{};
    for (auto &frame:frames) frame.requested=64;
    for (auto &frame:tail) frame.requested=64;
    int context=1;
    assert(f.requests.admit(one,frames,5,f.client,2,1024,3072,7,0,&context)==Result::Success);
    assert(f.requests.admit(two,tail,2,f.client,2,1024,3072,7,0,&context)==Result::Success);
    assert(f.requests.prefetch()==Result::Success);
    AggregateFixture::Queue::Work ticket;assert(f.dma.beginWork(ticket) && ticket.preparing);
    f.requests.cancel(2);
    // The late preparation ticket has no native frame pointer and cannot
    // publish after cancellation, even if its mapping succeeds.
    assert(f.dma.commit(ticket,f.dma.execute(ticket)));f.work();
    Result result;
    assert(f.requests.take(one.serial,result)==&one && result==Result::Aborted);
    assert(f.requests.take(two.serial,result)==&two && result==Result::Aborted);
    one={};for (auto &frame:frames) frame.requested=64;
    assert(f.requests.admit(one,frames,5,f.client,2,1024,3072,8,0,&context)==Result::Success);
    f.fill();f.hardware.begin();
    f.hardware.core.endResult=Result::Timeout;
    f.requests.cancel(2);assert(f.hardware.ring.cancel()==Result::Timeout && f.hardware.ring.faulted());
    f.work();assert(!f.requests.notice() && f.client->prepares==4);
    // A whole-core halt is the only remaining reclaim proof.
    f.cancel(Result::Aborted,true);
    assert(f.requests.take(one.serial,result)==&one && result==Result::Aborted);
}

static void aggregatePreparationErrorAndPinnedCleanup() {
    AggregateFixture f;using Iso=AggregateFixture::Iso;
    Iso::Request one{};Iso::Frame frames[2]{};frames[0].requested=frames[1].requested=64;
    int context=1;
    assert(f.requests.admit(one,frames,2,f.client,2,1024,3072,7,0,&context)==Result::Success);
    assert(f.requests.prefetch()==Result::Success);
    AggregateFixture::Queue::Work ticket;assert(f.dma.beginWork(ticket));f.api.fail=1;
    assert(f.dma.commit(ticket,f.dma.execute(ticket)));f.api.fail=0;f.work();
    assert(frames[0].complete && frames[0].result==Result::NotReady);
    assert(f.requests.publish(2,f.hardware.ring,4)==Result::Success && one.publish==2);
    f.hardware.begin();f.hardware.complete(0,64,104);
    assert(f.hardware.ring.handle(progress(2),4,200)==Result::Success);f.collect();
    // Every frame was already terminal before abort; valid completions and
    // the earlier preparation error remain immutable during copy/unmap.
    f.requests.cancel(2);f.work();Result result;
    assert(f.requests.take(one.serial,result)==&one && result==Result::NotReady);
    assert(frames[1].result==Result::Success && frames[1].actual==64);f.finishIdle();
    one={};frames[0]={};frames[0].requested=64;
    assert(f.requests.admit(one,frames,1,f.client,2,1024,3072,8,0,&context)==Result::Success);
    f.fill();f.hardware.begin();f.hardware.complete(1,64,108);
    assert(f.hardware.ring.handle(progress(2),4,200)==Result::Success);f.collect();
    f.api.clearFailures=1;f.work();
    const auto pinned=frames[0].cookie;
    assert(f.requests.take(one.serial,result)==&one && result==Result::HardwareError && f.dma.pinned());
    frames[0]={};one={}; // native completion may now free all aggregate storage
    assert(!f.dma.retryCleanup(pinned));
    assert(f.dma.retryCleanup(pinned,true));f.work();f.finishIdle();
}

static void aggregateCompletedDeadlineIsImmutable() {
    AggregateFixture f;using Iso=AggregateFixture::Iso;
    Iso::Request one{},two{};Iso::Frame a[1]{},b[1]{};a[0].requested=b[0].requested=64;
    int context=1;
    assert(f.requests.admit(one,a,1,f.client,2,1024,3072,7,1000,&context)==Result::Success);
    f.fill();f.hardware.begin();f.hardware.complete(0,64,104);
    assert(f.hardware.ring.handle(progress(2),4,200)==Result::Success);f.collect();
    assert(f.requests.admit(two,b,1,f.client,2,1024,3072,7,0,&context)==Result::Success);
    f.requests.expire(1000);
    assert(one.cancellation==Result::Pending && two.cancellation==Result::Pending);
    f.work();Result result;
    assert(f.requests.take(one.serial,result)==&one && result==Result::Success);
    f.fill();f.cancel();assert(f.requests.take(two.serial,result)==&two && result==Result::Aborted);
}

int main() {
    schedulingAndCapabilities();pipelineAndReuse();outShortMissedAndOverrun();
    cancelAndCommandFailures();busExpiry();malformedAndAdmission();packedFrameDMA();periodicRequestPipeline();
    aggregateBoundedFIFO();aggregateOUTAndCancellation();aggregateMappingAndHalt();aggregatePreparationErrorAndPinnedCleanup();
    aggregateCompletedDeadlineIsImmutable();
    std::puts("DWC3 ISO: periodic TRBs, frame epochs, Link reuse, bounded native aggregates, OUT prefixes, END retries, cancellation, timeout and DMA quarantine passed");
}
