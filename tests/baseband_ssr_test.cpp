// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandSSR.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace Baseband;
namespace S=Baseband::SSR;
struct Link {
    uint16_t identity=3;
    unsigned sends=0;
    Result status=Result::OK;
    std::vector<uint8_t> sent,incoming;
    bool connected(const char *name) const { assert(!strcmp(name,S::ChannelName));return identity!=0; }
    uint16_t channelIdentity(const char *name) const { assert(!strcmp(name,S::ChannelName));return identity; }
    Result sendOn(uint64_t epoch,const char *name,const void *data,size_t bytes,uint64_t) {
        assert(epoch==1 && !strcmp(name,S::ChannelName));if(status!=Result::OK)return status;
        const auto *p=static_cast<const uint8_t *>(data);sent.assign(p,p+bytes);++sends;return Result::OK;
    }
    Result datagramOn(uint64_t epoch,const char *name,const uint8_t *&p,size_t &n) const {
        assert(epoch==1 && !strcmp(name,S::ChannelName));p=incoming.data();n=incoming.size();return n?Result::OK:Result::NotReady;
    }
    Result acceptOn(uint64_t epoch,const char *name) { assert(epoch==1 && !strcmp(name,S::ChannelName));incoming.clear();return Result::OK; }
    void response(uint32_t sequence,uint32_t version=0,uint32_t command=1) {
        incoming.resize(12);write32(incoming.data(),version);write32(incoming.data()+4,command);write32(incoming.data()+8,sequence);
    }
};
struct StopOwner {
    bool stopped=false;
    unsigned calls=0;
    Result confirmedStopped(S::Edge edge,uint64_t epoch) {
        assert(edge==S::Edge::ADSP && epoch>=7);++calls;return stopped?Result::OK:Result::NotReady;
    }
};
int main() {
    Link wire;S::Client<Link> client(wire);StopOwner owner;
    assert(client.start(1,0)==Result::OK);
    assert(client.cleanup(1,S::Edge::ADSP,7,owner,0)==Result::NotReady && !wire.sends);
    owner.stopped=true;assert(client.cleanup(1,S::Edge::ADSP,7,owner,0)==Result::OK);
    wire.status=Result::Busy;assert(client.progress(1,0)==Result::Busy && !wire.sends);
    wire.status=Result::OK;assert(client.progress(1,1)==Result::OK && wire.sends==1 && wire.sent.size()==48);
    assert(!read32(wire.sent.data()) && !read32(wire.sent.data()+4) && read32(wire.sent.data()+8)==1);
    assert(read32(wire.sent.data()+12)==5 && !memcmp(wire.sent.data()+16,"lpass\0",6));
    wire.response(0);assert(client.progress(1,2)==Result::NotReady && client.outcome()==Result::Busy);
    wire.response(1,1);assert(client.progress(1,3)==Result::NotReady && client.outcome()==Result::Busy);
    wire.response(1,0,0);assert(client.progress(1,4)==Result::NotReady && client.outcome()==Result::Busy);
    wire.response(1);assert(client.progress(1,5)==Result::NotReady && client.outcome()==Result::OK);
    assert(client.discardedResponses()==3);
    assert(client.cleanup(1,S::Edge::ADSP,7,owner,6)==Result::Stale);
    assert(client.cleanup(1,S::Edge::ADSP,8,owner,6)==Result::OK && client.progress(1,6)==Result::OK);
    wire.response(1);assert(client.progress(1,7)==Result::NotReady && client.outcome()==Result::Busy);
    assert(client.progress(1,1000006)==Result::NotReady && client.outcome()==Result::Timeout);
    wire.response(2);assert(client.progress(1,1000007)==Result::NotReady && client.outcome()==Result::Timeout);
    assert(client.cleanup(1,S::Edge::ADSP,9,owner,1000008)==Result::OK);
    ++wire.identity;assert(client.progress(1,1000009)==Result::NotReady && client.outcome()==Result::Stale);
    // Nothing was published for generation 9, so a fresh channel may retry.
    assert(client.cleanup(1,S::Edge::ADSP,9,owner,1000010)==Result::OK);
    assert(client.progress(1,1000010)==Result::OK && wire.sends==3);
    wire.response(4);assert(client.progress(1,1000011)==Result::NotReady && client.outcome()==Result::OK);
    assert(client.cleanup(0,S::Edge::ADSP,10,owner,1000012)==Result::Stale);
    client.stopped();assert(client.progress(1,1000013)==Result::Stale);
    std::puts("baseband GLINK SSR stop-owner witness, wire/sequence correlation, stale ACK and channel retirement checks passed");
}
