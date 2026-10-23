// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandLink.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
using namespace Baseband;
namespace G=Baseband::GLINK;
static bool normal(Result r) { return r==Result::OK || r==Result::Busy || r==Result::NotReady; }
struct Captured {
    Result status=Result::OK;
    std::vector<std::vector<uint8_t>> frames;
    Result send(const void *p,size_t n,bool) {
        if(status!=Result::OK)return status;
        const auto *b=static_cast<const uint8_t *>(p);frames.emplace_back(b,b+n);return Result::OK;
    }
};
static void frameChecks() {
    Captured tx;auto mux=std::make_unique<G::Multiplexer<Captured>>(tx);
    assert(mux->add("sys_mon")==Result::OK && mux->add("glink_ssr")==Result::OK);
    assert(mux->add("sys_mon")==Result::Invalid && mux->add("")==Result::Invalid);
    assert(mux->start(1,1,0)==Result::OK && mux->add("late")==Result::Invalid);
    auto feed=[&](G::Command command,uint16_t cid,uint32_t value,const char *name=nullptr) {
        const size_t bytes=name?strlen(name)+1:0;
        G::Frame f{command,cid,value,0,0,reinterpret_cast<const uint8_t *>(name),bytes,(8+bytes+7)&~size_t(7)};
        return mux->feed(1,f,0);
    };
    // Retain an early remote OPEN, without holding the stream ahead of a
    // VERSION_ACK or sending a second independent VERSION for this channel.
    assert(feed(G::Open,42,8,"sys_mon")==Result::OK);
    assert(mux->instanceCount()==0 && mux->progress(0)==Result::OK);
    assert(read16(tx.frames.back().data())==G::Version);
    assert(feed(G::VersionAck,1,1)==Result::OK);
    assert(mux->progress(0)==Result::OK && mux->instanceCount()==2);
    assert(mux->progress(0)==Result::OK);
    assert(read16(tx.frames.back().data())==G::OpenAck && read16(tx.frames.back().data()+2)==42);
    assert(feed(G::Open,42,10,"glink_ssr")==Result::Invalid && mux->quarantined()); // Remote CID collision.
    Captured noReuse;auto fallback=std::make_unique<G::Multiplexer<Captured>>(noReuse);
    assert(fallback->start(1,1,0)==Result::OK && fallback->progress(0)==Result::OK);
    G::Frame downgrade{G::VersionAck,1,0,0,0,nullptr,0,8};
    assert(fallback->feed(1,downgrade,0)==Result::OK && fallback->progress(0)==Result::OK);
    assert(read16(noReuse.frames.back().data())==G::Version && !read32(noReuse.frames.back().data()+4));
    assert(fallback->feed(1,downgrade,0)==Result::OK && fallback->progress(0)==Result::OK);
    G::Frame crossed{G::Version,1,1,0,0,nullptr,0,8};
    assert(fallback->feed(1,crossed,0)==Result::OK && fallback->progress(0)==Result::OK);
    assert(read16(noReuse.frames.back().data())==G::VersionAck && !read32(noReuse.frames.back().data()+4));
    Captured last;auto exhausted=std::make_unique<G::Multiplexer<Captured>>(last);
    assert(exhausted->add("sys_mon")==Result::OK && exhausted->start(1,UINT16_MAX,0)==Result::OK);
    assert(exhausted->progress(0)==Result::OK);
    G::Frame ack{G::VersionAck,1,1,0,0,nullptr,0,8};
    assert(exhausted->feed(1,ack,0)==Result::OK && exhausted->progress(0)==Result::OK);
    assert(exhausted->open(1,"sys_mon",0)==Result::OK && exhausted->progress(0)==Result::NoMemory);
}
int main() {
    frameChecks();
    struct Rings {
        alignas(8) uint8_t a[16384]{},b[16384]{};
        alignas(8) uint32_t cursors[4]{};
        unsigned kicks=0;
        static bool kick(void *p) { ++static_cast<Rings *>(p)->kicks;return true; }
    } memory;
    G::MemoryRing atx,arx,btx,brx;
    assert(atx.bind(memory.a,16384,memory.cursors+1,memory.cursors,&memory,Rings::kick));
    assert(brx.bind(memory.a,16384,memory.cursors+1,memory.cursors,&memory,Rings::kick));
    assert(btx.bind(memory.b,16384,memory.cursors+3,memory.cursors+2,&memory,Rings::kick));
    assert(arx.bind(memory.b,16384,memory.cursors+3,memory.cursors+2,&memory,Rings::kick));
    using Link=G::Link<G::MemoryRing>;
    auto a=std::make_unique<Link>(atx,arx),b=std::make_unique<Link>(btx,brx);
    assert(a->addChannel("sys_mon")==Result::OK && a->addChannel("glink_ssr")==Result::OK);
    assert(b->addChannel("sys_mon",true)==Result::OK && b->addChannel("glink_ssr",true)==Result::OK);
    assert(a->start(1,1,0)==Result::OK && b->start(1,17,0)==Result::OK);
    uint64_t now=0;
    auto pump=[&]() { now+=1000;assert(normal(a->pump(1,now)));assert(normal(b->pump(1,now))); };
    const char *names[]={"IPCRTR","sys_mon","glink_ssr"};
    for(unsigned i=0;i<100;++i)pump();
    for(auto name:names)assert(a->connected(name) && b->connected(name));
    assert(a->channelIdentity()==1 && b->channelIdentity()==17);
    FIFO<G::MemoryRing> inject(btx,16384);
    const auto stable=a->channelIdentity("sys_mon");
    uint8_t version[8]{};G::header(version,G::Version,1,1);
    assert(inject.push(version,sizeof(version))==Result::OK);
    for(unsigned i=0;i<20;++i)pump();
    assert(a->connected() && b->connected() && a->channelIdentity("sys_mon")==stable);
    std::vector<uint8_t> payloads[3],reverse[3];
    for(unsigned i=0;i<3;++i) {
        payloads[i].resize(i?48+i:65536);reverse[i].resize(payloads[i].size());
        for(size_t j=0;j<payloads[i].size();++j) { payloads[i][j]=uint8_t(j*7+i);reverse[i][j]=uint8_t(j*11+101+i); }
        assert(a->sendOn(1,names[i],payloads[i].data(),payloads[i].size(),now)==Result::OK);
        assert(b->sendOn(1,names[i],reverse[i].data(),reverse[i].size(),now)==Result::OK);
    }
    bool blocked=false;
    for(unsigned i=0;i<20;++i) { now+=1000;const auto r=a->pump(1,now);assert(normal(r));blocked|=r==Result::Busy; }
    assert(blocked && !a->transmitIdle());
    bool gotA[3]{},gotB[3]{};
    for(unsigned i=0;i<300;++i) {
        pump();
        for(unsigned j=0;j<3;++j) {
            const uint8_t *p=nullptr;size_t n=0;
            auto r=a->datagramOn(1,names[j],p,n);assert(r==Result::OK || r==Result::NotReady);
            if(r==Result::OK) {
                assert(n==reverse[j].size() && !memcmp(p,reverse[j].data(),n));gotA[j]=true;
                if(j)assert(a->acceptOn(1,names[j])==Result::OK); // Keep IPCRTR's completed RX intent held.
            }
            r=b->datagramOn(1,names[j],p,n);assert(r==Result::OK || r==Result::NotReady);
            if(r==Result::OK) {
                assert(n==payloads[j].size() && !memcmp(p,payloads[j].data(),n));gotB[j]=true;
                if(j)assert(b->acceptOn(1,names[j])==Result::OK);
            }
        }
    }
    for(unsigned i=0;i<3;++i)assert(gotA[i] && gotB[i]);
    const uint8_t *p=nullptr;size_t n=0;
    assert(a->acceptOn(1,"IPCRTR")==Result::OK && b->acceptOn(1,"IPCRTR")==Result::OK);
    for(unsigned i=0;i<20;++i)pump();
    // Optional-channel CLOSE and re-open leave IPCRTR and its NV transport
    // alive. Each endpoint gets a fresh local ID; late old ACKs cannot close it.
    const auto oldA=a->channelIdentity("sys_mon"),oldB=b->channelIdentity("sys_mon");
    assert(b->closeChannel(1,"sys_mon",now)==Result::OK);
    for(unsigned i=0;i<50;++i)pump();
    assert(a->channelClosed("sys_mon") && b->channelClosed("sys_mon"));
    assert(a->connected() && b->connected() && a->connected("glink_ssr"));
    assert(b->openChannel(1,"sys_mon",now)==Result::OK);
    for(unsigned i=0;i<80;++i)pump();
    assert(a->connected("sys_mon") && b->connected("sys_mon"));
    assert(a->channelIdentity("sys_mon")>oldA && b->channelIdentity("sys_mon")>oldB);
    uint8_t stale[24]{};G::header(stale,G::CloseAck,oldA,0);
    assert(inject.push(stale,8)==Result::OK);
    G::header(stale,G::TXData,oldB,1);write32(stale+8,8);write32(stale+12,0);
    assert(inject.push(stale,sizeof(stale))==Result::OK);
    for(unsigned i=0;i<20;++i)pump();
    assert(a->connected("sys_mon") && a->datagramOn(1,"sys_mon",p,n)==Result::NotReady);
    const uint8_t ack[]="ssr:ack";
    assert(b->sendOn(1,"sys_mon",ack,sizeof(ack),now)==Result::OK);
    for(unsigned i=0;i<40;++i)pump();
    assert(a->datagramOn(1,"sys_mon",p,n)==Result::OK && n==sizeof(ack) && !memcmp(p,ack,n));
    assert(a->acceptOn(1,"sys_mon")==Result::OK);
    assert(a->datagramOn(0,"sys_mon",p,n)==Result::Stale);
    assert(memory.kicks && !a->quarantined() && !b->quarantined());
    std::puts("baseband shared GLINK negotiation, three-channel isolation, backpressure and fresh-ID reopen checks passed");
}
