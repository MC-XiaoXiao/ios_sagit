// SPDX-License-Identifier: GPL-2.0-only
// The production GLINK/QRTR/RMTFS/sysmon/stop sequence on two host rings.
#include "BasebandLink.h"
#include "BasebandRMTFS.h"
#include "BasebandSysmon.h"
#include "BasebandSSR.h"
#include "BasebandShutdown.h"
#include "BasebandTFTP.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <deque>
#include <fstream>
#include <iterator>
using namespace Baseband;
namespace G=Baseband::GLINK;
namespace Q=Baseband::QRTR;
namespace F=Baseband::RMTFS;
namespace S=Baseband::Sysmon;
static bool normal(Result r) { return r==Result::OK || r==Result::Busy || r==Result::NotReady || r==Result::Unsupported; }
struct NV {
    std::vector<uint8_t> disk=std::vector<uint8_t>(65536,0x8e),shared=std::vector<uint8_t>(F::SharedBytes);
    unsigned opened=0,writes=0,flushes=0;
    bool ready() const { return true; }
    Result open(unsigned part) { assert(!part);++opened;return Result::OK; }
    Result close(unsigned part) { assert(!part && opened);--opened;return Result::OK; }
    Result size(unsigned part,uint64_t &bytes) { assert(!part && opened);bytes=disk.size();return Result::OK; }
    Result read(unsigned part,uint64_t offset,void *p,size_t n) {
        assert(!part && opened && range(offset,n,disk.size()));memcpy(p,disk.data()+offset,n);return Result::OK;
    }
    Result write(unsigned part,uint64_t offset,const void *p,size_t n) {
        assert(!part && opened && range(offset,n,disk.size()));memcpy(disk.data()+offset,p,n);++writes;return Result::OK;
    }
    Result sync(unsigned part) { assert(!part && opened);++flushes;return Result::OK; }
    Result readShared(uint64_t physical,void *p,size_t n) {
        assert(physical>=F::SharedBase && range(physical-F::SharedBase,n,shared.size()));
        memcpy(p,shared.data()+physical-F::SharedBase,n);return Result::OK;
    }
    Result writeShared(uint64_t physical,const void *p,size_t n) {
        assert(physical>=F::SharedBase && range(physical-F::SharedBase,n,shared.size()));
        memcpy(shared.data()+physical-F::SharedBase,p,n);return Result::OK;
    }
};
struct Simulation {
    struct Rings {
        alignas(8) uint8_t a[16384]{},b[16384]{};
        alignas(8) uint32_t cursors[4]{};
        static bool kick(void *) { return true; }
    } memory;
    G::MemoryRing atx,arx,btx,brx;
    using Link=G::Link<G::MemoryRing>;
    std::unique_ptr<Link> ap,mss;
    std::unique_ptr<Q::Router<Link>> apRouter,mssRouter;
    NV nv;
    std::unique_ptr<F::ServiceEndpoint<Link,NV>> rmtfs;
    std::unique_ptr<Q::ServiceHost<Link>> ssctl;
    std::unique_ptr<S::Client<Link>> sysmon;
    std::unique_ptr<SSR::Client<Link>> ssr;
    TFTP::Files files;
    Q::PortAllocator ports;
    std::unique_ptr<TFTP::Server<Link>> tftp;
    unsigned serviceCursor=0;
    std::vector<uint8_t> descriptor,receivedFile;
    std::deque<std::vector<uint8_t>> fileReplies;
    uint32_t filePort=0;
    uint16_t fileBlock=0;
    bool fileRequested=false,fileDone=false;
    uint64_t now=0;
    uint32_t nvClient=0,caller=0;
    bool shutdownReceived=false,nvSent=false,nvDone=false,shutdownReplied=false,resetProven=false;
    unsigned pumps=0,stopSet=0,stopClear=0;
    Result graceful=Result::NotReady;
    Simulation() {
        assert(atx.bind(memory.a,16384,memory.cursors+1,memory.cursors,&memory,Rings::kick));
        assert(brx.bind(memory.a,16384,memory.cursors+1,memory.cursors,&memory,Rings::kick));
        assert(btx.bind(memory.b,16384,memory.cursors+3,memory.cursors+2,&memory,Rings::kick));
        assert(arx.bind(memory.b,16384,memory.cursors+3,memory.cursors+2,&memory,Rings::kick));
        ap=std::make_unique<Link>(atx,arx);mss=std::make_unique<Link>(btx,brx);
        for(auto name:{S::ChannelName,SSR::ChannelName}) {
            assert(ap->addChannel(name)==Result::OK && mss->addChannel(name,true)==Result::OK);
        }
        assert(ap->start(1,1,0)==Result::OK && mss->start(1,17,0)==Result::OK);
        apRouter=std::make_unique<Q::Router<Link>>(*ap,&ports);mssRouter=std::make_unique<Q::Router<Link>>(*mss);
        assert(apRouter->start(1,1,0)==Result::OK && mssRouter->start(1,0,0)==Result::OK);
        rmtfs=std::make_unique<F::ServiceEndpoint<Link,NV>>(*ap,nv);
        ssctl=std::make_unique<Q::ServiceHost<Link>>(*mss);
        sysmon=std::make_unique<S::Client<Link>>(*ap,*apRouter);ssr=std::make_unique<SSR::Client<Link>>(*ap);
        assert(rmtfs->start(1,1,0x3001,0)==Result::OK && ssctl->start(1,0,0x3002,43,2,0x12,0)==Result::OK);
        assert(sysmon->start(1,0)==Result::OK && ssr->start(1,0)==Result::OK);
        std::ifstream source("build/baseband/stock-files/modem_pr/mcfg/configs/mcfg_sw/generic/common/row/commerci/mcfg_sw.mbn",std::ios::binary);assert(source);
        descriptor=std::vector<uint8_t>(std::istreambuf_iterator<char>(source),{});assert(descriptor.size()==67732);
        std::ifstream packed("build/baseband/firmware/tftp-files.bin",std::ios::binary);assert(packed);
        const std::vector<uint8_t> catalog=std::vector<uint8_t>(std::istreambuf_iterator<char>(packed),{});
        assert(files.bind(catalog.data(),catalog.size())==Result::OK);
        assert(files.immutableCount()==113);
        tftp=std::make_unique<TFTP::Server<Link>>(*ap,files,&ports);
        assert(tftp->start(1,1,0x3003,0)==Result::OK);
        for(unsigned i=0;i<200;++i)step();
        assert(mssRouter->open(1,14,1,0,nvClient)==Result::OK);
        const char path[]="/boot/modem_fs1";uint8_t payload[3+sizeof(path)]{};
        payload[0]=1;write16(payload+1,sizeof(path));memcpy(payload+3,path,sizeof(path));
        auto r=mssRouter->request(1,nvClient,1,payload,sizeof(payload),now);
        for(unsigned i=0;r==Result::Busy && i<100;++i) { step();r=mssRouter->request(1,nvClient,1,payload,sizeof(payload),now); }
        assert(r==Result::OK);
        for(unsigned i=0;!caller && i<200;++i)step();
        assert(caller && nv.opened==1);
        for(unsigned i=0;i<20480;++i)nv.shared[512+i]=uint8_t(i*17+(i>>8));
    }
    void writeLastNV() {
        if(!shutdownReceived || nvSent)return;
        uint8_t payload[31]{}; // caller TLV, direction TLV, count+IOVEC TLV, force TLV
        payload[0]=1;write16(payload+1,4);write32(payload+3,caller);
        payload[7]=2;write16(payload+8,1);payload[10]=1;
        payload[11]=3;write16(payload+12,13);payload[14]=1;
        write32(payload+15,2);write32(payload+19,uint32_t(F::SharedBase+512));write32(payload+23,40);
        payload[27]=4;write16(payload+28,1);payload[30]=1;
        const auto r=mssRouter->request(1,nvClient,3,payload,sizeof(payload),now);
        assert(normal(r));if(r==Result::OK)nvSent=true;
    }
    void queueFilePacket(uint32_t type,uint32_t destination,const uint8_t *payload,size_t bytes) {
        std::vector<uint8_t> wire(32+((bytes+3)&~size_t(3)));size_t n=0;
        assert(Q::encode(wire.data(),wire.size(),{type,0,0x3100,1,destination,false,payload,uint32_t(bytes)},n)==Result::OK);
        fileReplies.push_back(std::move(wire));
    }
    void requestFile() {
        if(!shutdownReceived || fileRequested)return;
        const char name[]="/readonly/firmware/image/modem_pr/mcfg/configs/mcfg_sw/generic/common/row/commerci/mcfg_sw.mbn\0octet\0blksize\0" "512\0wsize\0" "4\0tsize\0" "0";
        std::vector<uint8_t> request(2+sizeof(name));TFTP::putBE16(request.data(),TFTP::RRQ);
        memcpy(request.data()+2,name,sizeof(name));queueFilePacket(Q::Data,0x3003,request.data(),request.size());fileRequested=true;
    }
    void receiveFile(const Q::Packet &p) {
        assert(p.sourceNode==1 && p.bytes>=4);
        if(p.confirm) {
            uint8_t resume[20]{};write32(resume,Q::ResumeTX);write32(resume+4,0);write32(resume+8,0x3100);
            queueFilePacket(Q::ResumeTX,p.sourcePort,resume,sizeof(resume));
        }
        const auto opcode=TFTP::be16(p.payload);uint16_t ack=0;
        if(opcode==TFTP::OACK) {assert(!filePort);filePort=p.sourcePort;}
        else {
            assert(opcode==TFTP::DATA && p.sourcePort==filePort && TFTP::be16(p.payload+2)==++fileBlock);
            receivedFile.insert(receivedFile.end(),p.payload+4,p.payload+p.bytes);
            const bool final=p.bytes<516;
            if(final) {assert(receivedFile==descriptor);fileDone=true;}
            if(!final && fileBlock%4)return;
            ack=fileBlock;
        }
        uint8_t payload[4];TFTP::putBE16(payload,TFTP::ACK);TFTP::putBE16(payload+2,ack);
        queueFilePacket(Q::Data,p.sourcePort,payload,sizeof(payload));
    }
    void service() {
        ++pumps;assert(normal(ap->pump(1,now)) && normal(mss->pump(1,now)));
        const uint8_t *data=nullptr;size_t bytes=0;Q::Event event;
        auto r=ap->datagram(1,data,bytes);
        if(r==Result::OK) {
            r=Q::feedServices(*apRouter,*rmtfs,*tftp,1,data,bytes,now,event);assert(normal(r));
            if(r==Result::OK)sysmon->onEvent(1,event);
            if(r!=Result::Busy)assert(ap->accept(1)==Result::OK);
        } else assert(r==Result::NotReady);
        assert(normal(Q::progressServices(*apRouter,*rmtfs,*tftp,serviceCursor,1,now,event)));
        sysmon->onEvent(1,event);assert(normal(sysmon->progress(1,now)) && normal(ssr->progress(1,now)));
        r=mss->datagram(1,data,bytes);
        if(r==Result::OK) {
            Q::Packet packet;assert(Q::parse(data,bytes,packet)==Result::OK);
            if(packet.type==Q::Data && packet.destinationPort==0x3100) {receiveFile(packet);r=Result::OK;event={};}
            else r=Q::feedService(*mssRouter,*ssctl,1,data,bytes,now,event);
            assert(normal(r));
            if(r==Result::OK && event.type==Q::EventType::Response) {
                assert(event.client==nvClient && event.result==Result::OK);
                if(event.message.id==1) {
                    QMI::TLVs fields(event.message.payload,event.message.bytes);uint8_t tag;const uint8_t *p;uint16_t n;
                    while(fields.next(tag,p,n))if(tag==0x10) { assert(n==4);caller=read32(p); }
                    assert(fields.valid() && caller);
                } else {
                    assert(event.message.id==3 && nvSent && nv.flushes==1 && nv.writes==2);nvDone=true;
                }
            }
            if(r!=Result::Busy)assert(mss->accept(1)==Result::OK);
        } else assert(r==Result::NotReady);
        assert(normal(Q::progressService(*mssRouter,*ssctl,1,now,event)));
        Q::ServiceRequest request;
        if(ssctl->request(1,request)==Result::OK) {
            assert(request.message.id==S::Shutdown && !request.message.bytes);
            shutdownReceived=true;writeLastNV();requestFile();
            if(nvDone && fileDone) {
                assert(!memcmp(nv.disk.data()+1024,nv.shared.data()+512,20480));
                const uint8_t result[]={2,4,0,0,0,0,0};uint8_t wire[14];size_t n=0;
                assert(QMI::reply(wire,sizeof(wire),request.message.transaction,S::Shutdown,result,sizeof(result),n)==Result::OK);
                r=ssctl->respond(1,wire,n,now);assert(normal(r));if(r==Result::OK)shutdownReplied=true;
            }
        }
        if(!fileReplies.empty()) {
            const auto &wire=fileReplies.front();r=mss->send(1,wire.data(),wire.size(),now);assert(normal(r));
            if(r==Result::OK)fileReplies.pop_front();
        }
    }
    void step() { now+=1000;service(); }
    uint64_t shutdownNowUS() const { return now; }
    bool shutdownAborted() const { return false; }
    void shutdownDelayMS(unsigned ms) { now+=ms*1000; }
    Result beginGraceful(uint64_t epoch,uint64_t time,unsigned timeout) { return sysmon->shutdown(epoch,time,timeout); }
    bool gracefulPending() const {
        return sysmon->state()==S::Phase::Selecting || sysmon->state()==S::Phase::Sending || sysmon->state()==S::Phase::Waiting;
    }
    Result gracefulOutcome() const { return sysmon->outcome(); }
    void recordGraceful(Result r) { graceful=r; }
    Result sampleStop(bool &ack) { ack=false;return Result::OK; }
    Result stopBit(uint64_t epoch,bool value) { assert(epoch==1);if(value)++stopSet;else ++stopClear;return Result::OK; }
    Result pumpStop(uint64_t epoch,uint64_t time) { assert(epoch==1 && time==now);service();return Result::OK; }
};
int main() {
    auto model=std::make_unique<Simulation>();
    ShutdownSequence<Simulation> sequence(*model);
    assert(sequence.run(1,5000000)==Result::OK);
    assert(model->shutdownReceived && model->nvDone && model->fileDone && model->shutdownReplied && model->nv.flushes==1);
    assert(model->graceful==Result::OK && model->sysmon->mode()==S::Mode::SSCTLv2);
    assert(!model->stopSet && !model->stopClear && !model->resetProven && model->nv.opened==1);
    // Only the separate modeled AXI/reset owner can retire the service. A
    // sysmon result itself neither closes the NV handle nor reclaims memory.
    model->resetProven=true;model->apRouter->disconnect();model->sysmon->stopped();model->ssr->stopped();model->tftp->stopped();
    assert(model->rmtfs->stopped()==Result::OK && !model->nv.opened);
    std::puts("baseband live-ring service integration: shutdown waits for persisted NV, actual sagit TFTP bytes and matching SSCTL ACK");
}
