// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandSysmon.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
using namespace Baseband;
namespace S=Baseband::Sysmon;
namespace R=Baseband::QRTR;
using Bytes=std::vector<uint8_t>;
struct Link {
    uint16_t ipc=1,legacy=2;
    Result status=Result::OK;
    Bytes wire,text,incoming;
    unsigned texts=0;
    bool connected(const char *name="IPCRTR") const { return channelIdentity(name)!=0; }
    uint16_t channelIdentity(const char *name="IPCRTR") const { return !strcmp(name,"IPCRTR")?ipc:legacy; }
    Result send(uint64_t epoch,const void *data,size_t bytes,uint64_t) {
        assert(epoch==1);if(status!=Result::OK)return status;
        if(!ipc)return Result::NotReady;
        const auto *p=static_cast<const uint8_t *>(data);wire.assign(p,p+bytes);return Result::OK;
    }
    Result sendOn(uint64_t epoch,const char *name,const void *data,size_t bytes,uint64_t) {
        assert(epoch==1 && !strcmp(name,S::ChannelName));if(status!=Result::OK)return status;
        if(!legacy)return Result::NotReady;
        const auto *p=static_cast<const uint8_t *>(data);text.assign(p,p+bytes);++texts;return Result::OK;
    }
    Result datagramOn(uint64_t epoch,const char *name,const uint8_t *&p,size_t &n) const {
        assert(epoch==1 && !strcmp(name,S::ChannelName));p=incoming.data();n=incoming.size();
        return n?Result::OK:Result::NotReady;
    }
    Result acceptOn(uint64_t epoch,const char *name) { assert(epoch==1 && !strcmp(name,S::ChannelName));incoming.clear();return Result::OK; }
    R::Packet last() const { R::Packet p;assert(R::parse(wire.data(),wire.size(),p)==Result::OK);return p; }
};
struct Fixture {
    Link io;
    std::unique_ptr<R::Router<Link>> router=std::make_unique<R::Router<Link>>(io);
    std::unique_ptr<S::Client<Link>> client=std::make_unique<S::Client<Link>>(io,*router);
    uint64_t now=0;
    Fixture() {
        R::Event e;assert(router->start(1,1,0)==Result::OK && router->progress(1,0,e)==Result::OK);
        assert(client->start(1,0)==Result::OK);
    }
    Result feed(uint32_t type,const Bytes &payload,uint32_t node=0,uint32_t port=7,uint32_t destination=R::Control) {
        Bytes wire(payload.size()+36);size_t bytes;
        assert(R::encode(wire.data(),wire.size(),{type,node,port,1,destination,false,payload.data(),uint32_t(payload.size())},bytes)==Result::OK);
        R::Event event;const auto result=router->feed(1,wire.data(),bytes,now,event);
        if(result==Result::OK)client->onEvent(1,event);
        return result;
    }
    void server(unsigned version=2,unsigned instance=S::ModemInstance,unsigned port=7,unsigned service=S::Service) {
        Bytes payload(20);write32(payload.data(),R::NewServer);write32(payload.data()+4,service);
        write32(payload.data()+8,(instance<<8)|version);write32(payload.data()+12,0);write32(payload.data()+16,port);
        assert(feed(R::NewServer,payload,0,R::Control)==Result::OK);
    }
    void response(const R::Packet &sent,uint16_t qmiError=0,bool indication=false) {
        QMI::Message request;assert(QMI::parse(sent.payload,sent.bytes,request)==Result::OK);
        Bytes payload(indication?7:14);payload[0]=indication?QMI::Indication:QMI::Response;
        write16(payload.data()+1,indication?0:request.transaction);write16(payload.data()+3,request.id);
        write16(payload.data()+5,indication?0:7);
        if(!indication) { payload[7]=2;write16(payload.data()+8,4);write16(payload.data()+10,qmiError?1:0);write16(payload.data()+12,qmiError); }
        assert(feed(R::Data,payload,sent.destinationNode,sent.destinationPort,sent.sourcePort)==Result::OK);
    }
};
int main() {
    {
        Fixture f;f.server(1,0,5);f.server(2,S::ModemInstance,7);
        assert(f.client->shutdown(1,0)==Result::OK);
        assert(f.client->progress(1,0)==Result::OK && f.client->mode()==S::Mode::SSCTLv2);
        const auto sent=f.io.last();assert(sent.type==R::Data && sent.destinationPort==7);
        QMI::Message m;assert(QMI::parse(sent.payload,sent.bytes,m)==Result::OK && m.id==0x21 && !m.bytes);
        f.response(sent,0,true);assert(f.client->shutdownIndicated() && f.client->outcome()==Result::Busy);
        f.response(sent);assert(f.client->outcome()==Result::OK && !f.io.texts);
        const uint32_t oldPort=sent.sourcePort;
        f.now=1;
        assert(f.client->notify(1,"adsp",S::Event::BeforeShutdown,S::Driven::Forced,1)==Result::OK);
        assert(f.client->progress(1,1)==Result::OK);
        const auto event=f.io.last();assert(event.sourcePort>oldPort && event.destinationPort==7);
        assert(QMI::parse(event.payload,event.bytes,m)==Result::OK && m.id==0x23 && m.bytes==22);
        assert(m.payload[0]==1 && read16(m.payload+1)==5 && m.payload[3]==4 && !memcmp(m.payload+4,"adsp",4));
        assert(m.payload[8]==2 && read32(m.payload+11)==2 && m.payload[15]==0x10 && !read32(m.payload+18));
        f.response(event,16);assert(f.client->outcome()==Result::IO && f.client->qmiError()==16);
    }
    {
        Fixture f;f.server(1,0);
        assert(f.client->shutdown(1,0)==Result::OK && f.client->progress(1,0)==Result::OK);
        assert(f.client->mode()==S::Mode::SSCTLv1);f.response(f.io.last());assert(f.client->outcome()==Result::OK);
        // Version 1 does not encode subsystem notifications; use an offered
        // legacy channel, without sending an unsupported QMI request.
        assert(f.client->notify(1,"adsp",S::Event::AfterPowerUp,S::Driven::Forced,1)==Result::OK);
        assert(f.client->progress(1,1)==Result::NotReady);
        assert(f.client->progress(1,S::DiscoveryUS+1)==Result::OK && f.client->mode()==S::Mode::Legacy);
        const char expected[]="ssr:adsp:after_powerup";
        assert(f.io.text.size()==sizeof(expected)-1 && !memcmp(f.io.text.data(),expected,sizeof(expected)-1));
        f.io.incoming={'s','s','r',':','a','c','k',0};assert(f.client->progress(1,S::DiscoveryUS+2)==Result::NotReady);
        assert(f.client->outcome()==Result::OK);
    }
    {
        Fixture f;assert(f.client->shutdown(1,0)==Result::OK);
        assert(f.client->progress(1,0)==Result::NotReady);
        auto query=f.io.last();assert(query.type==R::NewLookup && read32(query.payload+4)==43 && !read32(query.payload+8));
        f.io.status=Result::Busy;assert(f.client->progress(1,S::DiscoveryUS)==Result::Busy && !f.io.texts);
        f.io.status=Result::OK;assert(f.client->progress(1,S::DiscoveryUS+1)==Result::OK && f.io.texts==1);
        const char expected[]="ssr:shutdown";assert(f.io.text.size()==sizeof(expected) && !memcmp(f.io.text.data(),expected,sizeof(expected)));
        assert(f.client->progress(1,S::TimeoutUS)==Result::NotReady && f.client->outcome()==Result::Timeout);
        f.io.incoming={'s','s','r',':','a','c','k',0};assert(f.client->progress(1,S::TimeoutUS+1)==Result::NotReady);
        assert(f.client->outcome()==Result::Timeout);
        assert(f.client->shutdown(1,S::TimeoutUS+2)==Result::OK);
        assert(f.client->progress(1,S::TimeoutUS+S::DiscoveryUS+2)==Result::OK && f.client->outcome()==Result::Stale);
        // A fresh channel retires the ambiguous old ACK context.
        f.io.legacy=3;assert(f.client->shutdown(1,6000000)==Result::OK);
        assert(f.client->progress(1,6500000)==Result::OK);
        f.io.incoming={'s','s','r',':','a','c','k',0,'x'};
        assert(f.client->progress(1,6500001)==Result::NotReady && f.client->outcome()==Result::OK);
    }
    {
        Fixture f;f.server(2,S::ModemInstance,7);f.server(2,S::ModemInstance,8);
        assert(f.client->shutdown(1,0)==Result::OK && f.client->progress(1,0)==Result::OK);
        assert(f.client->outcome()==Result::Busy && !f.io.texts); // Ambiguous server selection is reported, not guessed.
        assert(f.client->state()==S::Phase::Failed);
    }
    {
        Fixture f;f.server();assert(f.client->shutdown(1,0)==Result::OK && f.client->progress(1,0)==Result::OK);
        f.io.ipc=0;assert(f.client->progress(1,1)==Result::OK && f.client->outcome()==Result::Stale);
        assert(f.client->notify(1,"modem",S::Event::BeforeShutdown,S::Driven::Forced,2)==Result::Invalid);
        assert(f.client->notify(1,"../adsp",S::Event::BeforeShutdown,S::Driven::Forced,2)==Result::Invalid);
        assert(f.client->shutdown(0,2)==Result::Stale);
    }
    std::puts("baseband sysmon SSCTL v2/v1, legacy wire, real ACK correlation, timeout and stale-channel checks passed");
}
