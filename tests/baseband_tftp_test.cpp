// SPDX-License-Identifier: GPL-2.0-only
// Real byte store, protocol correlation, retry/flow boundaries; no device I/O.
#include "BasebandTFTP.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>
using namespace Baseband;
namespace T = Baseband::TFTP;
namespace Q = Baseband::QRTR;
using Bytes = std::vector<uint8_t>;
static const char *firmwarePath = "/readonly/firmware/image/modemr.jsn";
static const char *temporaryPath = "/readwrite/server_check.txt";
static Bytes pattern(size_t n) {
    Bytes out(n); for (size_t i=0;i<n;++i) out[i]=uint8_t(i*37+(i>>8)); return out;
}
static Bytes catalog(const std::vector<std::pair<std::string,Bytes>> &files) {
    Bytes out(16+T::CatalogEntryBytes*files.size());
    write32(out.data(),T::CatalogMagic);write16(out.data()+4,1);write16(out.data()+6,T::CatalogEntryBytes);
    write32(out.data()+8,uint32_t(files.size()));
    for(size_t i=0;i<files.size();++i) {
        const auto &file=files[i];assert(file.first.size()<T::PathBytes);
        const size_t e=16+i*T::CatalogEntryBytes;
        memcpy(out.data()+e,file.first.c_str(),file.first.size());
        write32(out.data()+e+128,uint32_t(out.size()));write32(out.data()+e+132,uint32_t(file.second.size()));
        out.insert(out.end(),file.second.begin(),file.second.end());while(out.size()&3)out.push_back(0);
    }
    write32(out.data()+12,uint32_t(out.size()));return out;
}
static Bytes request(bool write,const char *path,std::initializer_list<std::pair<const char *,const char *>> options={}) {
    Bytes out{0,uint8_t(write?T::WRQ:T::RRQ)};
    auto string=[&](const char *p){while(*p)out.push_back(uint8_t(*p++));out.push_back(0);};
    string(path);string("octet");for(const auto &p:options){string(p.first);string(p.second);}return out;
}
static Bytes ack(uint16_t block) {Bytes p(4);T::putBE16(p.data(),T::ACK);T::putBE16(p.data()+2,block);return p;}
static Bytes data(uint16_t block,const Bytes &bytes) {
    Bytes p(4);T::putBE16(p.data(),T::DATA);T::putBE16(p.data()+2,block);p.insert(p.end(),bytes.begin(),bytes.end());return p;
}
struct Frame {
    uint32_t type=0,node=0,port=0,peerNode=0,peerPort=0;bool confirm=false;Bytes bytes;
    uint16_t opcode() const {assert(bytes.size()>=2);return T::be16(bytes.data());}
    uint16_t block() const {assert(bytes.size()>=4);return T::be16(bytes.data()+2);}
};
struct Endpoint {
    std::deque<Frame> sent;bool busy=false,uncertain=false;
    Result send(uint64_t epoch,const void *data,size_t n,uint64_t) {
        assert(epoch);if(uncertain)return Result::Quarantined;if(busy)return Result::Busy;
        Q::Packet p;assert(Q::parse(data,n,p)==Result::OK);
        sent.push_back({p.type,p.sourceNode,p.sourcePort,p.destinationNode,p.destinationPort,p.confirm,
                       Bytes(p.payload,p.payload+p.bytes)});return Result::OK;
    }
};
struct Model {
    Endpoint link;T::Files files;Q::PortAllocator ports;
    T::Server<Endpoint> server{link,files,&ports};
    uint64_t now=0,epoch=1;bool autoResume=true;
    explicit Model(const Bytes &blob) {
        assert(files.bind(blob.data(),blob.size())==Result::OK);
        assert(server.start(epoch,1,0x3003,now)==Result::OK);step();
        assert(link.sent.front().type==Q::NewServer && read32(link.sent.front().bytes.data()+4)==4096);
        link.sent.clear();
    }
    Result feed(const Bytes &body,uint32_t destination=0x3003,uint32_t node=0,uint32_t peer=0x9000,bool confirm=false) {
        Bytes wire(32+((body.size()+3)&~size_t(3)));size_t n=0;
        assert(Q::encode(wire.data(),wire.size(),{Q::Data,node,peer,1,destination,confirm,body.data(),uint32_t(body.size())},n)==Result::OK);
        return server.feed(epoch,wire.data(),n,now);
    }
    Result control(uint32_t type,uint32_t node=0,uint32_t peer=0x9000,uint32_t destination=Q::Control) {
        uint8_t body[20]{},wire[52];size_t n=0;write32(body,type);write32(body+4,node);write32(body+8,peer);
        assert(Q::encode(wire,sizeof(wire),{type,node,type==Q::ResumeTX?peer:Q::Control,1,destination,false,body,20},n)==Result::OK);
        return server.feed(epoch,wire,n,now);
    }
    Result step(uint64_t delta=1) {
        now+=delta;const auto r=server.progress(epoch,now);
        assert(r==Result::OK || r==Result::Busy || r==Result::NotReady);return r;
    }
    Frame take() {
        for(unsigned i=0;i<100;++i) {
            while(!link.sent.empty()) {
                auto f=std::move(link.sent.front());link.sent.pop_front();
                if(f.type!=Q::Data)continue;
                if(autoResume && f.confirm)assert(control(Q::ResumeTX,f.peerNode,f.peerPort,f.port)==Result::OK);
                return f;
            }
            step();
        }
        assert(false);return {};
    }
    Frame start(bool write,const char *path,std::initializer_list<std::pair<const char *,const char *>> options={},
                uint32_t node=0,uint32_t peer=0x9000) {
        assert(feed(request(write,path,options),0x3003,node,peer)==Result::OK);return take();
    }
    Bytes read(const char *path,uint32_t node=0,uint32_t peer=0x9000) {
        Frame f=start(false,path,{},node,peer);Bytes result;
        assert(f.opcode()==T::DATA);const auto port=f.port;uint16_t block=1;
        for(;;) {
            assert(f.port==port && f.block()==block++);result.insert(result.end(),f.bytes.begin()+4,f.bytes.end());
            assert(feed(ack(f.block()),port,node,peer)==Result::OK);
            if(f.bytes.size()<516)break;
            f=take();
        }
        step();return result;
    }
};
static void testCatalogAndStore() {
    Bytes actual=pattern(27),blob=catalog({{firmwarePath,actual},{"/readonly/firmware/image/empty",{}}});
    auto store=std::make_unique<T::Files>();assert(store->bind(blob.data(),blob.size())==Result::OK);
    blob.back()^=1;assert(store->start(1)==Result::OK);
    uint32_t h=0;assert(store->open(0,firmwarePath,false,false,h)==Result::OK);
    Bytes copy(actual.size());assert(store->read(h,0,copy.data(),copy.size())==Result::OK && copy==actual);
    assert(store->write(h,0,copy.data(),1)==Result::Stale);assert(store->close(h)==Result::OK);
    assert(store->read(h,0,copy.data(),1)==Result::Stale);
    assert(store->open(0,temporaryPath,true,true,h)==Result::OK);
    uint32_t other=0;assert(store->open(0,temporaryPath,false,false,other)==Result::Busy);
    assert(store->write(h,4,actual.data(),actual.size())==Result::OK);assert(store->close(h)==Result::OK);
    assert(store->open(2,temporaryPath,false,false,other)==Result::NotReady);
    assert(store->open(0,temporaryPath,false,false,h)==Result::OK);copy.assign(actual.size()+4,0xff);
    assert(store->read(h,0,copy.data(),copy.size())==Result::OK);
    assert(!copy[0] && !copy[1] && !copy[2] && !copy[3]);assert(!memcmp(copy.data()+4,actual.data(),actual.size()));
    assert(store->close(h)==Result::OK);store->stopped();assert(store->start(2)==Result::OK);
    assert(store->open(0,temporaryPath,false,false,h)==Result::NotReady);
    assert(store->open(0,firmwarePath,false,false,h)==Result::OK);
    assert(store->close(h)==Result::OK);store->stopped();
    const auto good=catalog({{firmwarePath,actual}});
    std::vector<Bytes> bad;
    for(size_t offset:{size_t(4),size_t(6),size_t(8),size_t(12),size_t(16+128),size_t(16+132),size_t(16+136),good.size()-1}) {
        auto x=good;x[offset]^=0xff;bad.push_back(std::move(x));
    }
    bad.push_back(catalog({{firmwarePath,actual},{firmwarePath,actual}}));
    bad.push_back(catalog({{"/readonly/firmware/image/../secret",actual}}));
    bad.push_back(catalog({{temporaryPath,actual}}));
    for(const auto &x:bad) {auto f=std::make_unique<T::Files>();assert(f->bind(x.data(),x.size())==Result::Invalid);}
}
static void testReadAndOptions() {
    const auto actual=pattern(17021);auto m=std::make_unique<Model>(catalog({{firmwarePath,actual}}));
    assert(m->read(firmwarePath)==actual && m->server.completed()==1);
    auto f=m->start(false,firmwarePath,{{"blksize","65464"},{"wsize","100"},{"tsize","0"},{"seek","13"},{"rsize","16384"},{"timeoutms","1000"}});
    const auto port=f.port;assert(f.opcode()==T::OACK);
    std::string o(reinterpret_cast<const char *>(f.bytes.data()+2),f.bytes.size()-2);
    assert(o.find(std::string("blksize\0" "8192\0",13))!=std::string::npos);
    assert(o.find(std::string("wsize\0" "16\0",9))!=std::string::npos);
    assert(m->feed(ack(3),port)==Result::OK);m->step();assert(m->link.sent.empty());
    const auto retry=request(false,firmwarePath,{{"blksize","65464"},{"wsize","100"},{"tsize","0"},{"seek","13"},{"rsize","16384"},{"timeoutms","1000"}});
    assert(m->feed(retry)==Result::OK);auto duplicate=m->take();assert(duplicate.port==port && duplicate.bytes==f.bytes);
    assert(m->feed(ack(0),port)==Result::OK);Bytes read;
    for(unsigned i=1;i<=3;++i) {
        f=m->take();assert(f.opcode()==T::DATA && f.port==port && f.block()==i);
        read.insert(read.end(),f.bytes.begin()+4,f.bytes.end());
    }
    assert(read==Bytes(actual.begin()+13,actual.begin()+13+16384) && f.bytes.size()==4);
    assert(m->feed(ack(4),port)==Result::OK && m->server.completed()==1);
    assert(m->feed(ack(3),port)==Result::OK && m->server.completed()==2);m->step();
    f=m->start(false,firmwarePath,{{"unknown","abc"}});assert(f.opcode()==T::DATA && f.block()==1);
    assert(m->control(Q::DeleteClient)==Result::OK);m->step();
    for(const auto &req:{request(false,"/readonly/firmware/image/absent"),request(true,firmwarePath),
        request(false,"/readonly/firmware/image/../secret"),request(false,firmwarePath,{{"seek","18446744073709551616"}}),
        request(false,firmwarePath,{{"blksize","7"}}),request(false,firmwarePath,{{"tsize","1"}}),
        request(false,firmwarePath,{{"seek","17022"}}),request(false,firmwarePath,{{"rsize","17022"}}),
        request(false,firmwarePath,{{"wsize","2"},{"WSIZE","3"}})}) {
        assert(m->feed(req)==Result::OK);f=m->take();assert(f.opcode()==T::ERROR && f.block()!=0);m->step();
    }
    T::Request parsed;uint64_t state=0x2cab35;
    for(unsigned i=0;i<10000;++i) {
        state=state*6364136223846793005ULL+1;Bytes random(state%1100);
        for(auto &b:random){state=state*6364136223846793005ULL+1;b=uint8_t(state>>32);}
        T::parseRequest(random.data(),random.size(),parsed);
    }
}
static void testWritesAndIdempotence() {
    auto m=std::make_unique<Model>(catalog({}));
    auto f=m->start(true,temporaryPath);const auto port=f.port;assert(f.opcode()==T::ACK && !f.block());
    const auto full=pattern(512),tail=pattern(27);
    assert(m->feed(data(1,full),port)==Result::OK);
    m->link.busy=true;m->step(); // Bytes committed while the ACK cannot publish.
    assert(m->feed(data(1,full),port)==Result::OK);
    assert(m->feed(data(2,tail),port)==Result::OK); // Premature window cannot hide RESUME behind it.
    m->link.busy=false;f=m->take();assert(f.opcode()==T::ACK && f.block()==1);
    assert(m->feed(data(3,tail),port)==Result::OK);f=m->take();assert(f.block()==1);
    assert(m->feed(data(2,tail),port)==Result::OK);f=m->take();assert(f.block()==2 && m->server.completed()==1);
    // Another writer really replaces the file. A lost-final-ACK retry must
    // resend ACK 2 without reapplying the old DATA over the newer bytes.
    uint32_t h=0;const Bytes replacement{7,8,9};
    assert(m->files.open(0,temporaryPath,true,true,h)==Result::OK);
    assert(m->files.write(h,0,replacement.data(),replacement.size())==Result::OK);
    assert(m->files.close(h)==Result::OK);
    assert(m->feed(data(2,tail),port)==Result::OK);f=m->take();assert(f.block()==2);
    assert(m->read(temporaryPath,0,0x9001)==replacement);
    f=m->start(false,temporaryPath,{},2,0x9000);assert(f.opcode()==T::ERROR && f.block()==T::NotFound);m->step();
    f=m->start(true,"/readwrite/window",{{"blksize","8"},{"wsize","4"},{"tsize","19"}});
    assert(f.opcode()==T::OACK);const auto windowPort=f.port;const auto bytes=pattern(19);
    assert(m->feed(data(1,Bytes(bytes.begin(),bytes.begin()+8)),windowPort)==Result::OK);m->step();
    assert(m->link.sent.empty());
    assert(m->feed(data(2,Bytes(bytes.begin()+8,bytes.begin()+16)),windowPort)==Result::OK);
    assert(m->feed(data(3,Bytes(bytes.begin()+16,bytes.end())),windowPort)==Result::OK);
    f=m->take();assert(f.opcode()==T::ACK && f.block()==3);
    assert(m->read("/readwrite/window",0,0x9002)==bytes);
    f=m->start(true,"/readwrite/empty");const auto emptyPort=f.port;
    assert(m->feed(data(1,{}),emptyPort)==Result::OK);f=m->take();assert(f.opcode()==T::ACK && f.block()==1);
    assert(m->read("/readwrite/empty",0,0x9003).empty());
    f=m->start(true,"/readwrite/badsize",{{"tsize","10"}});const auto badPort=f.port;
    assert(m->feed(data(1,Bytes{1,2}),badPort)==Result::OK);f=m->take();assert(f.opcode()==T::ERROR && f.block()==T::BadOptions);
    m->step();
    f=m->start(true,"/readwrite/toolarge",{{"tsize","65536"},{"seek","1"}});
    assert(f.opcode()==T::ERROR && f.block()==T::NoSpace);
}
static void testFlowTimeoutAndPeerExit() {
    const auto actual=pattern(512*20+1);auto m=std::make_unique<Model>(catalog({{firmwarePath,actual}}));
    m->autoResume=false;
    auto f=m->start(false,firmwarePath,{{"wsize","16"}});const auto port=f.port;
    assert(f.opcode()==T::OACK && m->feed(ack(0),port)==Result::OK);
    // OACK + 9 DATA exhaust the ten-datagram QRTR high watermark.
    for(unsigned i=1;i<=9;++i){f=m->take();assert(f.block()==i);}
    assert(m->step()==Result::Busy && m->link.sent.empty());
    assert(m->feed(ack(9),port,0,0x9000,true)==Result::OK);m->step();
    assert(m->link.sent.front().type==Q::ResumeTX && m->link.sent.front().port==port);m->link.sent.clear();
    assert(m->control(Q::ResumeTX,0,0x9000,port)==Result::OK);
    f=m->take();assert(f.block()==10);
    // A spoofed peer only receives ERROR 5; it cannot retire the real reader.
    assert(m->feed(ack(10),port,2,0x9001)==Result::OK);f=m->take();
    assert(f.opcode()==T::ERROR && f.block()==T::BadTransfer && f.peerNode==2);
    assert(m->control(Q::Bye)==Result::OK);m->step();
    for(unsigned i=0;i<20;++i)m->step();
    for(const auto &sent:m->link.sent)assert(sent.type!=Q::Data);
    m->link.sent.clear();
    f=m->start(false,firmwarePath,{{"timeoutms","1"}});const auto retryPort=f.port;
    m->step(1000);auto retry=m->take();assert(retry.port==retryPort && retry.bytes==f.bytes);
    for(unsigned i=0;i<10;++i){m->step(1000);m->link.sent.clear();}
    assert(m->server.timedOut()==1);
    assert(m->control(Q::DeleteClient)==Result::OK);m->step();m->link.sent.clear();
    f=m->start(true,temporaryPath);const auto writePort=f.port;
    assert(m->feed(data(1,Bytes{1,2,3}),writePort)==Result::OK);m->take();
    m->server.stopped();m->epoch=2;assert(m->server.start(2,1,0x3003,m->now)==Result::OK);m->step();m->link.sent.clear();
    f=m->start(false,temporaryPath);assert(f.opcode()==T::ERROR && f.block()==T::NotFound);m->step();
    f=m->start(false,firmwarePath);assert(f.opcode()==T::DATA && f.port!=port && f.port!=retryPort);
    assert(m->feed(ack(1),f.port)==Result::OK);
    m->link.uncertain=true;
    const auto r=m->server.progress(2,++m->now);assert(r==Result::Quarantined);
    assert(m->server.progress(2,++m->now)==Result::Stale);
}
static void testBlockWrapAndSharedPorts() {
    const auto actual=pattern(524289);auto m=std::make_unique<Model>(catalog({{firmwarePath,actual}}));
    auto f=m->start(false,firmwarePath,{{"blksize","8"},{"wsize","16"}});const auto port=f.port;
    assert(f.opcode()==T::OACK && m->feed(ack(0),port)==Result::OK);Bytes output;unsigned count=0;
    for(;;) {
        f=m->take();assert(f.opcode()==T::DATA && f.block()==uint16_t(++count));
        output.insert(output.end(),f.bytes.begin()+4,f.bytes.end());
        if(!(count%16) || f.bytes.size()<12)assert(m->feed(ack(f.block()),port)==Result::OK);
        if(f.bytes.size()<12)break;
    }
    assert(count==65537 && output==actual && m->server.completed()==1);m->step();
    Q::Router<Endpoint> router(m->link,&m->ports);assert(router.start(1,1,m->now)==Result::OK);
    uint8_t payload[20]{},wire[52];size_t n=0;write32(payload,Q::NewServer);write32(payload+4,2);
    write32(payload+8,1);write32(payload+12,0);write32(payload+16,99);
    assert(Q::encode(wire,sizeof(wire),{Q::NewServer,0,Q::Control,1,Q::Control,false,payload,20},n)==Result::OK);
    Q::Event event;assert(router.feed(1,wire,n,m->now,event)==Result::OK);
    uint32_t client=0;assert(router.open(1,2,1,0,client)==Result::OK && client>port);
    f=m->start(false,"/readonly/firmware/image/missing");assert(f.opcode()==T::ERROR && f.port>client);
}
static void testBoundedResourcesAndIndependentDeadlines() {
    auto m=std::make_unique<Model>(catalog({}));m->link.busy=true;
    for(unsigned i=0;i<40;++i) {
        const auto req=request(false,"/readonly/firmware/image/absent");
        assert(m->feed(req,0x3003,0,0x9000+i,i<32)==Result::OK);
        // Control confirmations drain without requiring peer DATA credits.
        m->step();
    }
    m->link.busy=false;
    for(unsigned i=0;i<100;++i)m->step();
    m->link.sent.clear();
    // A writer's lease expires even with an RX confirmation queued first.
    assert(m->feed(request(true,temporaryPath),0x3003,0,0x9100,true)==Result::OK);
    uint32_t h=0;assert(m->files.open(0,temporaryPath,true,true,h)==Result::Busy);
    m->step(400000000);
    assert(m->server.timedOut()>=1);
    assert(m->files.open(0,temporaryPath,true,true,h)==Result::OK);
    assert(m->files.close(h)==Result::OK);
    m->server.stopped();
    assert(m->files.start(2)==Result::OK);
    for(unsigned i=0;i<T::TemporaryFiles;++i) {
        const auto name="/readwrite/f"+std::to_string(i);
        assert(m->files.open(0,name.c_str(),true,true,h)==Result::OK);
        assert(m->files.close(h)==Result::OK);
    }
    assert(m->files.open(0,"/readwrite/full",true,true,h)==Result::NoMemory);
    m->files.stopped();
}
int main() {
    testCatalogAndStore();testReadAndOptions();testWritesAndIdempotence();testFlowTimeoutAndPeerExit();testBlockWrapAndSharedPorts();
    testBoundedResourcesAndIndependentDeadlines();
    std::puts("baseband TFTP real-byte catalog/temp store, options/windows, write ACKs, retries, flow, peer retirement and block wrap checks passed");
}
