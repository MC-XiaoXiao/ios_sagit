// SPDX-License-Identifier: BSD-2-Clause
#include "BlueToolBootstrap.h"
#include <assert.h>
#include <deque>
#include <vector>
#include <stdio.h>
using namespace Bluetooth;
using namespace Bluetooth::BlueTool;
static constexpr uint64_t Address=0x060403020100ULL;
struct Backend {
    Snapshot state;
    uint64_t time=0;
    bool powered=false,opened=false,powerFailure=false,openFailure=false,closeFailure=false,stuck=false;
    unsigned pauses=0,powerCalls=0,commands=0,closes=0;
    uint64_t readyAt=0,responseDelay=1;
    bool optionalCC=true,sharedLE=false;
    uint16_t corruptOpcode=0;
    enum Corruption { None, Status, Size, Company, Product, SoC, ROM, Patch, MAC, Credit,
        Hardware, WrongOpcode, MissingVendor, Truncated, Timeout, NoLE, NoACL, LEOverflow, ACLOverflow,
        CommandStatusOnly, EpochChanged, PolicyChanged, LateVendorError } corrupt=None;
    std::deque<std::vector<uint8_t>> replies;
    Backend() {
        state.v[BootstrapVersion]=1;state.v[BootstrapState]=uint32_t(State::Off);
        state.v[BootstrapFlags]=BootstrapConfigured|BootstrapDrained;
        state.v[BootstrapProduct]=0x1234;state.v[BootstrapSoC]=0x400a0200;
        state.v[BootstrapROM]=0x201;state.v[BootstrapPatch]=0x345;state.v[BootstrapAddress]=Address;
    }
    uint64_t now() { return time; }
    void pause() {
        time+=10;++pauses;
        if (stuck || time<readyAt) return;
        state.v[BootstrapState]=uint32_t(powered?State::Ready:State::Off);
        state.v[BootstrapResources]=powered?AllResources:0;
        if (powered) state.v[BootstrapFlags]|=BootstrapEndpoints;
    }
    bool snapshot(Snapshot& out) { out=state;return true; }
    bool power(bool on,uint64_t expected,uint64_t& accepted) {
        ++powerCalls;
        if (powerFailure || expected != state.v[BootstrapGeneration]) return false;
        powered=on;
        state.v[BootstrapState]=uint32_t(on?State::Starting:State::Stopping);
        if (on) {
            ++state.v[BootstrapGeneration];state.v[BootstrapFlags]|=BootstrapPowerRequested;
        } else state.v[BootstrapFlags]&=~uint64_t(BootstrapPowerRequested);
        accepted=state.v[BootstrapGeneration];return true;
    }
    bool open() {
        if (openFailure) return false;
        assert(!opened && powered);opened=true;state.v[BootstrapFlags]|=BootstrapPortOpen;return true;
    }
    bool close(bool success) {
        assert(opened);++closes;opened=false;state.v[BootstrapFlags]&=~uint64_t(BootstrapPortOpen);
        if (!success) { uint64_t ignored;power(false,state.v[BootstrapGeneration],ignored); }
        if (corrupt==EpochChanged) ++state.v[BootstrapGeneration];
        if (corrupt==PolicyChanged) ++state.v[BootstrapSoC];
        return !closeFailure;
    }
    bool send(const uint8_t* p,size_t n,uint64_t deadline) {
        assert(opened && n>=4 && deadline>time);++commands;
        const auto opcode=read16(p+1);
        assert(p[0]==Command && n==size_t(4+p[3]));
        if (opcode==DownloadOpcode) assert(n==5 && p[4]==0x19);
        else assert(n==4);
        std::vector<uint8_t> event={Event,0x0e,4,1,uint8_t(opcode),uint8_t(opcode>>8),0};
        event.reserve(258);
        switch (opcode) {
        case 0x1001:event.insert(event.end(),{9,0x78,0x56,9,29,0,0x34,0x12});break;
        case DownloadOpcode: {
            if (optionalCC) replies.push_back(event); // CC cannot complete the vendor query
            event={Event,0xff,14,0,2};event.resize(17);
            write32(event.data()+5,uint32_t(state.v[BootstrapProduct]));
            write16(event.data()+9,uint16_t(state.v[BootstrapPatch]));
            write16(event.data()+11,uint16_t(state.v[BootstrapROM]));
            write32(event.data()+13,uint32_t(state.v[BootstrapSoC]));break;
        }
        case 0x1009:event.insert(event.end(),{0,1,2,3,4,6});break;
        case 0x1002:event.resize(71,0xff);break;
        case 0x1003:event.resize(15,0);event[11]=0x40;break;
        case 0x1005:event.insert(event.end(),{0xfd,3,0x40,8,0,4,0});break;
        case 0x2002:
            if (sharedLE) event.insert(event.end(),{0,0,0});
            else event.insert(event.end(),{0xfb,0,6});
            break;
        default:assert(false);
        }
        event[2]=uint8_t(event.size()-3);
        if (opcode==corruptOpcode) switch (corrupt) {
        case Status:event[6]=1;break;
        case Size:event.push_back(0);++event[2];break;
        case Company:event[11]=15;break;
        case Product:event[5]^=1;break;
        case SoC:event[13]^=1;break;
        case ROM:event[11]^=1;break;
        case Patch:event[9]^=1;break;
        case MAC:event[7]^=1;break;
        case Credit:event[3]=0;break;
        case Hardware:event={Event,0x10,1,1};break;
        case WrongOpcode:event[4]^=1;break;
        case MissingVendor:return true;
        case Truncated:event.pop_back();break;
        case NoLE:event[11]=0;break;
        case NoACL:event[10]=event[11]=0;break;
        case ACLOverflow:event[7]=1;event[8]=0x10;break;
        case LEOverflow:event[7]=1;event[8]=0x10;break;
        case CommandStatusOnly:event={Event,0x0f,4,0,1,uint8_t(opcode),uint8_t(opcode>>8)};break;
        default:break;
        }
        replies.push_back(event);
        if (opcode==DownloadOpcode && optionalCC) {
            std::vector<uint8_t> late={Event,0x0e,4,1,0,0xfc,0};
            if (corrupt==LateVendorError) late[6]=1;
            replies.push_back(late); // after VSE, across the next command's receive
        }
        return true;
    }
    Error receive(uint8_t* out,size_t capacity,size_t& n,uint64_t deadline) {
        if (corrupt==Timeout || replies.empty() || responseDelay>=deadline-time) { time=deadline;return Error::Timeout; }
        auto event=replies.front();replies.pop_front();assert(event.size()<=capacity);
        memcpy(out,event.data(),event.size());n=event.size();time+=responseDelay;return Error::Success;
    }
};
static void successfulLifecycle() {
    Backend io;Bootstrap<Backend> bootstrap(io);
    assert(bootstrap.run(Script::Boot,"06:04:03:02:01:00")==Error::Success);
    auto p=bootstrap.evidence();
    assert(p.complete && p.generation==1 && p.company==29 && p.address==Address);
    assert(p.soc.patch==0x345 && p.soc.rom==0x201 && p.aclBytes==1021 && p.aclPackets==8);
    assert(p.leBytes==251 && p.lePackets==6 && io.commands==7 && io.closes==1 && !io.opened);
    assert(bootstrap.run(Script::DeepSleep,nullptr)==Error::Success && !io.powered);
    assert(!io.state.v[BootstrapResources] && !bootstrap.evidence().complete);
    assert(bootstrap.run(Script::Init,nullptr)==Error::Success && bootstrap.evidence().generation==2);
    Backend noCC;noCC.optionalCC=false;Bootstrap<Backend> v(noCC);
    assert(v.run(Script::Boot,nullptr)==Error::Success && v.evidence().complete);
    Backend shared;shared.sharedLE=true;Bootstrap<Backend> h(shared);
    assert(h.run(Script::Init,nullptr)==Error::Success && !h.evidence().leBytes && h.evidence().aclPackets);
}
static void refusals() {
    uint64_t value=0;
    for (const char* invalid:{"", "01:02:03:04:05:06", "00:00:00:00:00:00", "GG:00:00:00:00:01",
             "06:04:03:02:01:0", "06:04:03:02:01:00junk"}) assert(!parseAddress(invalid,value));
    assert(parseAddress("aA:bb:cc:dd:ee:ff",value) && value==0xaabbccddeeffULL);
    assert(script("boot")==Script::Boot && script("init")==Script::Init && script("deepsleep")==Script::DeepSleep);
    assert(script("bootx")==Script::Invalid && script(nullptr)==Script::Invalid);
    for (unsigned kind=0;kind<7;++kind) {
        Backend io;
        if (kind==0) io.state.v[BootstrapFlags]|=BootstrapPortOpen;
        if (kind==1) io.state.v[BootstrapVersion]=2;
        if (kind==2) io.state.v[BootstrapSoC]=0;
        if (kind==3) io.state.v[BootstrapFlags]|=0x20;
        if (kind==4) io.state.v[BootstrapAddress]=0x010403020100ULL;
        Bootstrap<Backend> bootstrap(io);
        auto e=bootstrap.run(kind==6?Script::Invalid:Script::Boot,kind==5?"08:04:03:02:01:00":nullptr);
        assert(e!=Error::Success && !io.powerCalls && !bootstrap.evidence().complete);
    }
    Backend stuck;stuck.stuck=true;Bootstrap<Backend> b(stuck);
    assert(b.run(Script::Boot,nullptr)==Error::Timeout && stuck.time==20000 && !b.evidence().complete);
    assert(!stuck.powered); // cancel an admitted startup on its exact generation
    Backend off;off.stuck=true;off.state.v[BootstrapResources]=AllResources;Bootstrap<Backend> d(off);
    assert(d.run(Script::DeepSleep,nullptr)==Error::Timeout && off.time==3000);
    Backend noPower;noPower.powerFailure=true;Bootstrap<Backend> n(noPower);
    assert(n.run(Script::Boot,nullptr)==Error::Failed);
    Backend slow;slow.readyAt=19000;slow.responseDelay=1900;slow.optionalCC=false;Bootstrap<Backend> s(slow);
    assert(s.run(Script::Boot,nullptr)==Error::Timeout && slow.time==25000 && !s.evidence().complete);
    Backend overflow;overflow.time=UINT64_MAX;Bootstrap<Backend> o(overflow);
    assert(o.run(Script::Boot,nullptr)==Error::Invalid && !overflow.powerCalls);
}
static void corruptions() {
    const std::pair<uint16_t,Backend::Corruption> cases[]={
        {0x1001,Backend::Company},{0x1001,Backend::Credit},{0x1001,Backend::Status},
        {0x1001,Backend::WrongOpcode},{0x1001,Backend::Hardware},{0x1001,Backend::Size},
        {0x1001,Backend::Truncated},{0x1001,Backend::Timeout},{0x1001,Backend::CommandStatusOnly},
        {DownloadOpcode,Backend::Product},{DownloadOpcode,Backend::SoC},{DownloadOpcode,Backend::ROM},
        {DownloadOpcode,Backend::Patch},{DownloadOpcode,Backend::MissingVendor},
        {0x1009,Backend::MAC},{0x1009,Backend::LateVendorError},
        {0x1002,Backend::Size},{0x1003,Backend::NoLE},{0x1005,Backend::NoACL},
        {0x1005,Backend::ACLOverflow},{0x2002,Backend::LEOverflow},
        {0,Backend::EpochChanged},{0,Backend::PolicyChanged}};
    for (auto c:cases) {
        Backend io;io.corruptOpcode=c.first;io.corrupt=c.second;Bootstrap<Backend> b(io);
        assert(b.run(Script::Boot,nullptr)!=Error::Success && !b.evidence().complete && !io.opened);
        assert(io.closes==1);
        if (c.second==Backend::EpochChanged) assert(io.powered); // stale cleanup cannot stop generation 2
    }
    Backend io;io.closeFailure=true;Bootstrap<Backend> b(io);
    assert(b.run(Script::Boot,nullptr)==Error::Failed && !b.evidence().complete);
}
int main() {
    successfulLifecycle();refusals();corruptions();
    assert(validCall(GetBootstrap,0,BootstrapScalars) && !validCall(GetBootstrap,1,BootstrapScalars));
    puts("BlueTool WCN3990 boot/init/shutdown, real HCI proof, late QCA responses, identity/epoch failures and deadlines passed");
}
