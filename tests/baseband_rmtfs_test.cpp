// SPDX-License-Identifier: GPL-2.0-only
#include "BasebandRMTFS.h"
#include "BasebandLink.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
using namespace Baseband;
namespace R = Baseband::RMTFS;
namespace Q = Baseband::QRTR;
using Bytes = std::vector<uint8_t>;
static void tlv(Bytes &out, uint8_t tag, const Bytes &data) {
    out.push_back(tag); out.push_back(uint8_t(data.size())); out.push_back(uint8_t(data.size() >> 8));
    out.insert(out.end(), data.begin(), data.end());
}
static Bytes number(uint32_t value) { Bytes b(4); write32(b.data(), value); return b; }
static Bytes path(unsigned i) {
    const auto *p = reinterpret_cast<const uint8_t *>(R::Partitions[i].path);
    return Bytes(p, p + strlen(R::Partitions[i].path));
}
static Bytes request(uint16_t id, const Bytes &payload, uint16_t transaction = 1) {
    Bytes b(7 + payload.size()); b[0] = QMI::Request;
    write16(b.data() + 1, transaction); write16(b.data() + 3, id); write16(b.data() + 5, uint16_t(payload.size()));
    memcpy(b.data() + 7, payload.data(), payload.size()); return b;
}
struct Backend {
    Bytes storage[4], shared = Bytes(R::SharedBytes);
    unsigned opened[4]{}, reads = 0, writes = 0, syncs = 0, sharedReads = 0, sharedWrites = 0;
    unsigned failWrite = 0, failSync = 0;
    unsigned stopAfterWrites = 0;
    bool online = true;
    Result error = Result::IO;
    Backend() { for (auto &b : storage) b.resize(65536, 0xa5); }
    bool ready() const { return online; }
    Result open(unsigned p) { ++opened[p]; return Result::OK; }
    Result close(unsigned p) { assert(opened[p]); --opened[p]; return Result::OK; }
    Result size(unsigned p, uint64_t &n) { n = storage[p].size(); return Result::OK; }
    Result read(unsigned p, uint64_t offset, void *b, size_t n) {
        assert(opened[p] && range(offset, n, storage[p].size())); ++reads;
        memcpy(b, storage[p].data() + offset, n); return Result::OK;
    }
    Result write(unsigned p, uint64_t offset, const void *b, size_t n) {
        assert(opened[p] && range(offset, n, storage[p].size()));
        if (++writes == failWrite) return error;
        memcpy(storage[p].data() + offset, b, n); return Result::OK;
    }
    Result sync(unsigned p) { assert(opened[p]); return ++syncs == failSync ? Result::IO : Result::OK; }
    Result readShared(uint64_t physical, void *b, size_t n) {
        if (stopAfterWrites && writes >= stopAfterWrites) return Result::NotReady;
        assert(physical >= R::SharedBase && range(physical - R::SharedBase, n, shared.size())); ++sharedReads;
        memcpy(b, shared.data() + physical - R::SharedBase, n); return Result::OK;
    }
    Result writeShared(uint64_t physical, const void *b, size_t n) {
        assert(physical >= R::SharedBase && range(physical - R::SharedBase, n, shared.size())); ++sharedWrites;
        memcpy(shared.data() + physical - R::SharedBase, b, n); return Result::OK;
    }
};
struct Reply { uint16_t error; uint64_t extra; unsigned extraBytes; };
static Reply decode(const void *p, size_t n, uint16_t tid = 1) {
    QMI::Message m; assert(QMI::parse(p, n, m, true) == Result::OK && m.transaction == tid);
    uint16_t error; const auto r = QMI::response(m, error);
    assert(r == (error ? Result::IO : Result::OK));
    Reply out{error, 0, 0}; QMI::TLVs fields(m.payload, m.bytes);
    uint8_t tag; const uint8_t *v; uint16_t bytes;
    while (fields.next(tag, v, bytes)) if (tag == 0x10) {
        assert(bytes <= 8); out.extraBytes = bytes;
        for (unsigned i = 0; i < bytes; ++i) out.extra |= uint64_t(v[i]) << (8 * i);
    }
    assert(fields.valid()); return out;
}
static QMI::Message message(const Bytes &b) {
    QMI::Message m; assert(QMI::parse(b.data(), b.size(), m, true) == Result::OK); return m;
}
static Bytes open(unsigned part = 0, uint16_t tid = 1) { Bytes p; tlv(p, 1, path(part)); return request(R::Open, p, tid); }
static Bytes handle(uint16_t id, uint32_t caller) { Bytes p; tlv(p, 1, number(caller)); return request(id, p); }
struct Vec { uint32_t sector, physical, count; };
static Bytes iovec(uint32_t caller, bool write, const std::vector<Vec> &vectors, bool force = false) {
    Bytes p, v(1 + 12 * vectors.size()); v[0] = uint8_t(vectors.size());
    for (size_t i = 0; i < vectors.size(); ++i) {
        write32(v.data() + 1 + 12 * i, vectors[i].sector);
        write32(v.data() + 5 + 12 * i, vectors[i].physical);
        write32(v.data() + 9 + 12 * i, vectors[i].count);
    }
    tlv(p, 1, number(caller)); tlv(p, 2, {uint8_t(write)}); tlv(p, 3, v); tlv(p, 4, {uint8_t(force)});
    return request(R::ReadWrite, p);
}
struct Endpoint {
    Result status = Result::OK;
    Bytes wire;
    unsigned sends = 0;
    Result send(uint64_t epoch, const void *data, size_t bytes, uint64_t) {
        assert(epoch == 1); if (status != Result::OK) return status;
        ++sends; const auto *p = static_cast<const uint8_t *>(data); wire.assign(p, p + bytes); return Result::OK;
    }
    Q::Packet last() const { Q::Packet p; assert(Q::parse(wire.data(), wire.size(), p) == Result::OK); return p; }
};
static Bytes packet(uint32_t type, uint32_t node, uint32_t port, const Bytes &payload,
                    uint32_t destination = 0x3001, bool confirm = false) {
    Bytes b(payload.size() + 36); size_t n;
    assert(Q::encode(b.data(), b.size(), {type,node,port,1,destination,confirm,payload.data(),uint32_t(payload.size())}, n) == Result::OK);
    b.resize(n); return b;
}
static Bytes control(uint32_t type, uint32_t node = 0, uint32_t port = 7) {
    Bytes p(20); write32(p.data(), type); write32(p.data() + 4, node); write32(p.data() + 8, port);
    return packet(type, node, Q::Control, p, Q::Control);
}
static bool waiting(Result r) { return r==Result::OK || r==Result::Busy || r==Result::NotReady || r==Result::Unsupported; }
static void fullLinkChecks() {
    struct Rings {
        alignas(8) uint8_t ap[16384]{},mss[16384]{};
        alignas(8) uint32_t cursors[4]{};
        unsigned kicks=0;
        static bool kick(void *p) { ++static_cast<Rings *>(p)->kicks;return true; }
    } memory;
    using Link=GLINK::Link<GLINK::MemoryRing>;
    GLINK::MemoryRing atx,arx,btx,brx;
    assert(atx.bind(memory.ap,16384,memory.cursors+1,memory.cursors,&memory,Rings::kick));
    assert(brx.bind(memory.ap,16384,memory.cursors+1,memory.cursors,&memory,Rings::kick));
    assert(btx.bind(memory.mss,16384,memory.cursors+3,memory.cursors+2,&memory,Rings::kick));
    assert(arx.bind(memory.mss,16384,memory.cursors+3,memory.cursors+2,&memory,Rings::kick));
    auto ap=std::make_unique<Link>(atx,arx),mss=std::make_unique<Link>(btx,brx);
    auto apRouter=std::make_unique<Q::Router<Link>>(*ap),client=std::make_unique<Q::Router<Link>>(*mss);
    Backend disk;auto server=std::make_unique<R::ServiceEndpoint<Link,Backend>>(*ap,disk);
    assert(ap->start(1,3,0)==Result::OK && mss->start(1,17,0)==Result::OK);
    assert(apRouter->start(1,1,0)==Result::OK && client->start(1,0,0)==Result::OK);
    assert(server->start(1,1,0x3001,0)==Result::OK);
    Bytes response;unsigned responses=0;uint64_t now=0;
    auto pump=[&]() {
        now+=1000;assert(waiting(ap->pump(1,now)) && waiting(mss->pump(1,now)));
        const uint8_t *data=nullptr;size_t n=0;Q::Event event;
        auto r=ap->datagram(1,data,n);
        if(r==Result::OK) {
            r=Q::feedService(*apRouter,*server,1,data,n,now,event);assert(waiting(r));
            if(r!=Result::Busy)assert(ap->accept(1)==Result::OK);
        } else assert(r==Result::NotReady);
        assert(waiting(Q::progressService(*apRouter,*server,1,now,event)));
        r=mss->datagram(1,data,n);
        if(r==Result::OK) {
            r=client->feed(1,data,n,now,event);assert(waiting(r));
            if(r==Result::OK && event.type==Q::EventType::Response) {
                Q::Packet packet;assert(Q::parse(data,n,packet)==Result::OK);
                response.assign(packet.payload,packet.payload+packet.bytes);++responses;
            }
            if(r!=Result::Busy)assert(mss->accept(1)==Result::OK);
        } else assert(r==Result::NotReady);
        assert(waiting(client->progress(1,now,event)));
    };
    for(unsigned i=0;i<200;++i)pump();
    assert(ap->connected() && mss->connected() && server->published() && memory.kicks);
    uint32_t port=0;assert(client->open(1,R::Service,1,0,port)==Result::OK);
    uint16_t transaction=0;
    auto exchange=[&](const Bytes &raw) {
        const auto m=message(raw);const unsigned before=responses;
        auto r=client->request(1,port,m.id,m.payload,m.bytes,now);
        for(unsigned i=0;r==Result::Busy && i<200;++i) { pump();r=client->request(1,port,m.id,m.payload,m.bytes,now); }
        assert(r==Result::OK);
        for(unsigned i=0;responses==before && i<200;++i)pump();
        assert(responses==before+1 && read16(response.data()+3)==m.id);
        return decode(response.data(),response.size(),++transaction);
    };
    const auto h=uint32_t(exchange(open()).extra);assert(h && disk.opened[0]);
    Bytes allocation;tlv(allocation,1,number(h));tlv(allocation,2,number(32768));
    assert(exchange(request(R::Allocate,allocation)).extra==R::SharedBase);
    for(unsigned i=0;i<20480;++i)disk.shared[i+512]=uint8_t(i*31);
    assert(!exchange(iovec(h,true,{{2,uint32_t(R::SharedBase+512),40}})).error && disk.writes==2 && disk.syncs==1);
    assert(!exchange(iovec(h,false,{{2,uint32_t(R::SharedBase+32768),40}})).error);
    assert(!memcmp(disk.shared.data()+512,disk.shared.data()+32768,20480));
    assert(!exchange(handle(R::Close,h)).error && !disk.opened[0]);
    assert(server->stopped()==Result::OK);
}
int main() {
    fullLinkChecks();
    Backend io; auto engine = std::make_unique<R::Engine<Backend>>(io);
    io.online = false; assert(engine->start(1) == Result::NotReady); io.online = true;
    assert(engine->start(1) == Result::OK);
    auto run = [&](const Bytes &b, uint32_t node = 0, uint32_t port = 7) {
        const auto m = message(b);
        assert(engine->prepare(1, node, port, m) == Result::OK);
        const uint8_t *p; size_t n; assert(engine->reply(p, n) == Result::OK);
        const auto r = decode(p, n, m.transaction); assert(engine->committed(1) == Result::OK); return r;
    };
    auto result = run(open(0, 0)); assert(!result.error && result.extraBytes == 4);
    const auto caller = uint32_t(result.extra); assert(caller && io.opened[0] == 1);
    assert(run(open()).extra == caller && io.opened[0] == 1);
    assert(run(handle(R::DeviceError, caller), 2).error == R::Internal);
    assert(run(handle(R::DeviceError, caller)).extra == 0);
    Bytes p; tlv(p, 1, {'/','b','o','o','t','/','.','.','/','m','o','d','e','m','_','f','s','1'});
    assert(run(request(R::Open,p)).error == R::Internal);
    tlv(p,1,path(0)); assert(run(request(R::Open,p)).error == R::Malformed);
    p.clear(); tlv(p,1,number(caller)); tlv(p,2,number(uint32_t(R::SharedBytes)));
    result = run(request(R::Allocate,p)); assert(!result.error && result.extra == R::SharedBase && result.extraBytes == 8);
    write32(p.data() + p.size() - 4, uint32_t(R::SharedBytes + 1));
    assert(run(request(R::Allocate,p)).error == R::Internal);
    for (size_t i = 0; i < io.shared.size(); ++i) io.shared[i] = uint8_t(i ^ (i >> 8));
    auto write = iovec(caller, true, {{3,uint32_t(R::SharedBase + 512),40}});
    auto m = message(write); assert(engine->prepare(1,0,7,m) == Result::OK);
    assert(io.writes == 2 && io.syncs == 1);
    assert(engine->prepare(1,0,7,m) == Result::OK && io.writes == 2); // TX busy: same response, no repeated I/O.
    assert(engine->prepare(1,0,8,m) == Result::Busy);
    assert(engine->committed(1) == Result::OK);
    assert(!memcmp(io.storage[0].data()+1536,io.shared.data()+512,40*512));
    memset(io.shared.data()+512,0x35,40*512);
    assert(!run(write).error && io.writes == 4 && io.syncs == 2); // TID/IOVEC reuse reads the new shared bytes.
    assert(io.storage[0][1536] == 0x35);
    assert(!run(iovec(caller,false,{{3,uint32_t(R::SharedBase+32768),40}})).error);
    assert(!memcmp(io.storage[0].data()+1536,io.shared.data()+32768,40*512));
    const unsigned writes = io.writes, reads = io.reads, sharedWrites = io.sharedWrites, sharedReads = io.sharedReads;
    for (const auto &vectors : std::vector<std::vector<Vec>>{
        {{0,uint32_t(R::SharedBase),1},{0,uint32_t(R::SharedBase-1),1}},
        {{0,uint32_t(R::SharedBase+R::SharedBytes-1),1}},
        {{0,uint32_t(R::SharedBase),UINT32_MAX}},
        {{UINT32_MAX,uint32_t(R::SharedBase),1}},
        {{128,uint32_t(R::SharedBase),1}}, {{0,uint32_t(R::SharedBase),0}},
        {{0,512,1}}}) assert(run(iovec(caller,true,vectors)).error == R::Malformed);
    assert(io.writes == writes && io.reads == reads && io.sharedWrites == sharedWrites && io.sharedReads == sharedReads);
    // A truncated array and a wrong scalar length are rejected without I/O.
    auto bad = write; bad[21] = 255; // TLV 3 count (after caller and direction).
    assert(run(bad).error == R::Malformed);
    p.clear();tlv(p,1,number(caller));tlv(p,2,{0,1});tlv(p,3,{0});tlv(p,4,{0});
    assert(run(request(R::ReadWrite,p)).error == R::Malformed);
    io.failWrite = io.writes + 2;
    assert(run(write).error == R::Internal && io.syncs == 3); // Flush the completed prefix.
    assert(run(handle(R::DeviceError,caller)).extra == 1);
    assert(!run(handle(R::Close,caller)).error && !io.opened[0]);
    assert(run(handle(R::DeviceError,caller)).error == R::Internal); // No null dereference.
    result = run(open()); assert(result.extra > caller);
    Q::PeerRemoved removed{0,7,true,false};
    assert(engine->removed(1,removed) == Result::OK && io.opened[0]);
    removed.wholeNode = true; assert(engine->removed(1,removed) == Result::OK && !io.opened[0]);
    assert(engine->resetAfterRemoteStopped() == Result::OK && engine->start(2) == Result::OK);
    assert(engine->prepare(1,0,7,message(open())) == Result::Stale);
    assert(engine->resetAfterRemoteStopped() == Result::OK);

    Endpoint wire; Backend disk; auto server = std::make_unique<R::ServiceEndpoint<Endpoint,Backend>>(wire,disk);
    assert(server->start(1,1,0x3001,0) == Result::OK);
    assert(server->progress(1,0) == Result::NotReady && server->published());
    auto advertised = wire.last();
    assert(advertised.type == Q::NewServer && read32(advertised.payload+4) == 14 && read32(advertised.payload+8) == 1);
    assert(read32(advertised.payload+12) == 1 && read32(advertised.payload+16) == 0x3001);
    auto feed = [&](const Bytes &b) { return server->feed(1,b.data(),b.size(),1); };
    assert(feed(packet(Q::Data,0,7,open(),0x4001)) == Result::NotReady);
    assert(feed(packet(Q::Data,0,7,open(),0x3001,true)) == Result::OK);
    wire.status = Result::Busy;
    assert(server->progress(1,1) == Result::Busy && disk.opened[0] == 1);
    wire.status = Result::OK;
    assert(server->progress(1,1) == Result::OK);
    const auto sent = wire.last(); assert(sent.type == Q::Data && sent.destinationNode == 0 && sent.destinationPort == 7);
    const uint32_t h = uint32_t(decode(sent.payload,sent.bytes).extra); assert(h && disk.opened[0] == 1);
    assert(feed(packet(Q::Data,0,7,iovec(h,true,{{0,uint32_t(R::SharedBase),1}}))) == Result::OK);
    wire.status = Result::Busy; assert(server->progress(1,1) == Result::Busy && disk.writes == 1);
    assert(server->progress(1,1) == Result::Busy && disk.writes == 1);
    // Socket deletion discards the retained response and its flow slot. The
    // node-scoped handle still works through another port.
    assert(feed(control(Q::DeleteClient)) == Result::OK);
    wire.status = Result::OK;
    assert(feed(packet(Q::Data,0,8,handle(R::DeviceError,h))) == Result::OK);
    assert(server->progress(1,1) == Result::OK && !decode(wire.last().payload,wire.last().bytes).error);
    for (unsigned i=0;i<40;++i) {
        const uint32_t port = 100+i;
        assert(feed(packet(Q::Data,0,port,handle(R::DeviceError,h))) == Result::OK);
        assert(server->progress(1,1) == Result::OK);
        assert(feed(control(Q::DeleteClient,0,port)) == Result::OK);
    }
    assert(feed(control(Q::Bye)) == Result::OK && !disk.opened[0]);
    assert(feed(packet(Q::Data,0,7,open())) == Result::OK);
    assert(server->progress(1,1) == Result::OK);
    for (unsigned i=2;i<=10;++i) {
        assert(feed(packet(Q::Data,0,7,open())) == Result::OK);
        assert(server->progress(1,1) == Result::OK && wire.last().confirm == (i==5));
    }
    assert(feed(packet(Q::Data,0,7,open())) == Result::OK && server->progress(1,1) == Result::Busy);
    Bytes resume(20); write32(resume.data(),Q::ResumeTX); write32(resume.data()+4,0); write32(resume.data()+8,7);
    assert(feed(packet(Q::ResumeTX,0,7,resume)) == Result::OK && server->progress(1,1) == Result::OK);
    assert(feed(packet(Q::Data,0,7,open())) == Result::OK);
    wire.status = Result::Busy;
    assert(server->progress(1,1) == Result::Busy);
    assert(server->progress(1,1+Q::ServiceRequestTimeoutUS) == Result::Timeout);
    assert(server->stopped() == Result::OK && !disk.opened[0]);
    // A watchdog between chunks stops further data transfers, but still
    // flushes the known completed prefix and returns an actual error.
    Backend interrupted;auto stopped=std::make_unique<R::Engine<Backend>>(interrupted);
    assert(stopped->start(1)==Result::OK);
    auto start=open();assert(stopped->prepare(1,0,7,message(start))==Result::OK);
    const uint8_t *stoppedReply;size_t stoppedBytes;
    assert(stopped->reply(stoppedReply,stoppedBytes)==Result::OK);
    const auto stoppedID=uint32_t(decode(stoppedReply,stoppedBytes).extra);
    assert(stopped->committed(1)==Result::OK);interrupted.stopAfterWrites=1;
    auto batch=iovec(stoppedID,true,{{0,uint32_t(R::SharedBase),40}});
    batch[17]=0xff;batch.back()=0xff; // Upstream u8 direction/force fields use nonzero truth values.
    assert(stopped->prepare(1,0,7,message(batch))==Result::OK);
    assert(stopped->reply(stoppedReply,stoppedBytes)==Result::OK && decode(stoppedReply,stoppedBytes).error==R::Internal);
    assert(interrupted.writes==1 && interrupted.syncs==1 && interrupted.sharedReads==1);
    assert(stopped->resetAfterRemoteStopped()==Result::OK && !interrupted.opened[0]);
    Backend retrySync;auto flushing=std::make_unique<R::Engine<Backend>>(retrySync);
    assert(flushing->start(1)==Result::OK && flushing->prepare(1,0,7,message(start))==Result::OK);
    assert(flushing->reply(stoppedReply,stoppedBytes)==Result::OK);
    const auto flushID=uint32_t(decode(stoppedReply,stoppedBytes).extra);
    assert(flushing->committed(1)==Result::OK);retrySync.failSync=1;
    auto one=iovec(flushID,true,{{0,uint32_t(R::SharedBase),1}});
    assert(flushing->prepare(1,0,7,message(one))==Result::OK && flushing->reply(stoppedReply,stoppedBytes)==Result::OK);
    assert(decode(stoppedReply,stoppedBytes).error==R::Internal && retrySync.writes==1 && retrySync.syncs==1);
    assert(flushing->committed(1)==Result::OK);
    auto empty=iovec(flushID,true,{});
    assert(flushing->prepare(1,0,7,message(empty))==Result::OK && flushing->reply(stoppedReply,stoppedBytes)==Result::OK);
    assert(!decode(stoppedReply,stoppedBytes).error && retrySync.writes==1 && retrySync.syncs==2);
    assert(flushing->resetAfterRemoteStopped()==Result::OK);
    // Unknown I/O completion prevents a successful reply and pins buffers.
    Backend unknown; auto uncertain=std::make_unique<R::Engine<Backend>>(unknown);
    assert(uncertain->start(1)==Result::OK);
    auto o=open(); assert(uncertain->prepare(1,0,7,message(o))==Result::OK);
    const uint8_t *reply;size_t bytes;assert(uncertain->reply(reply,bytes)==Result::OK);
    const auto id=uint32_t(decode(reply,bytes).extra);assert(uncertain->committed(1)==Result::OK);
    unknown.failWrite=1;unknown.error=Result::Quarantined;
    auto w=iovec(id,true,{{0,uint32_t(R::SharedBase),1}});
    assert(uncertain->prepare(1,0,7,message(w))==Result::Quarantined && uncertain->quarantined());
    assert(uncertain->reply(reply,bytes)==Result::Quarantined && !unknown.syncs);
    assert(uncertain->resetAfterRemoteStopped()==Result::Quarantined);
    std::puts("baseband RMTFS protocol, real-byte I/O model, flow control and peer retirement checks passed");
}
