#include "TouchHID.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include <utility>
struct Bus {
    std::map<std::pair<unsigned,unsigned>,std::vector<uint8_t>> reads;
    std::vector<std::pair<unsigned,std::vector<uint8_t>>> writes;
    int failRead=-1,failWrite=-1;
    unsigned delays=0,crcReads=0,pdtReads=0;
    bool crc=false;
    void data(unsigned a,std::initializer_list<uint8_t> values) { reads[{a,values.size()}]=values; }
    bool read(uint16_t a,uint8_t *out,size_t n) {
        if (a==failRead) return false;
        if ((a&255)==0xe9) ++pdtReads;
        memset(out,0,n);
        if (crc && a==0x60 && n==1) {out[0]=6;return true;}
        if (crcReads && a==0x60 && n==1) {--crcReads;out[0]=6;return true;}
        auto found=reads.find({a,n});
        if (found!=reads.end()) memcpy(out,found->second.data(),n);
        return true;
    }
    bool write(uint16_t a,const uint8_t *data,size_t n) {
        writes.push_back({a,{data,data+n}});
        return a!=failWrite;
    }
    void delayMS(unsigned ms) {delays+=ms;}
};
Bus fixture(bool f12=true) {
    Bus bus;
    bus.data(0xe9,{0x10,0x28,0x30,0x60,1,1});
    bus.data(0xe3,{0,0,0,0,6,0x54});
    bus.data(0x1e9,{0x00,0x30,0x40,0x80,2,uint8_t(f12 ? 0x12 : 0x11)});
    std::vector<uint8_t> identity(21,0);identity[0]=1;
    memcpy(identity.data()+11,"S3330TEST0",10);bus.reads[{0x10,21}]=identity;
    bus.data(0x30,{0x04});bus.data(0x31,{0x55,0x02});
    if (f12) {
        bus.data(0x104,{5});
        bus.data(0x105,{0x20,0x01,0x01,0x80,0x10});
        bus.data(0x142,{1,10});bus.data(0x141,{0x37,0x04,0x7f,0x07}); // 1079x1919
        bus.data(0x143,{0x1f});
        bus.data(0x107,{3});bus.data(0x108,{0x10,0x03,0x80});
        bus.reads[{0x181,80}]=std::vector<uint8_t>(80);
    } else {
        bus.data(0x100,{0,0x15,0,0,0,0});
        bus.data(0x146,{0x37,0x04,0x7f,0x07});
        bus.reads[{0x180,53}]=std::vector<uint8_t>(53);
    }
    return bus;
}
void checkDescriptor() {
    uint8_t descriptor[TouchHID::DescriptorBytes];
    auto length=TouchHID::descriptor(descriptor,sizeof(descriptor));assert(length);
    // Independent HID short-item walk checks wire lengths, globals and nesting.
    unsigned size=0,count=0,id=0,page=0,depth=0,fingers=0;
    bool exposesPressure=false;
    std::vector<unsigned> usages;
    std::map<unsigned,unsigned> inputs,features;
    for (unsigned i=0;i<length;) {
        unsigned prefix=descriptor[i++],bytes=prefix&3;if(bytes==3) bytes=4;
        assert(i+bytes<=length && prefix!=0xfe);
        unsigned value=0;for(unsigned n=0;n<bytes;++n)value|=unsigned(descriptor[i++])<<(8*n);
        switch(prefix&0xfc) {
        case 0x74:size=value;break;
        case 0x94:count=value;break;
        case 0x84:id=value;break;
        case 0x04:page=value;break;
        case 0x08:usages.push_back(value);break;
        case 0x80:
            inputs[id]+=size*count;
            if(page==0x0d && std::find(usages.begin(),usages.end(),0x30)!=usages.end())
                exposesPressure=true;
            usages.clear();
            break;
        case 0xb0:features[id]+=size*count;break;
        case 0xa0:++depth;if(value==2)++fingers;break;
        case 0xc0:assert(depth);--depth;break;
        }
    }
    assert(!depth && fingers==10);
    assert(inputs[1]==(TouchHID::ReportBytes-1)*8 && features[2]==8);
    assert(!exposesPressure);
    assert(!TouchHID::descriptor(descriptor,10));
}
int main() {
    auto bus=fixture();
    RMI4::Device<Bus> device(bus);
    assert(device.initialize()==RMI4::Result::Success);
    assert(device.maxX==1079 && device.maxY==1919 && device.contactCount()==10 && device.function()==0x12);
    assert(!strcmp(device.product,"S3330TEST0"));
    assert(bus.writes[0].first==0x143 && bus.writes[0].second[0]==0xff);
    assert(bus.writes[1].first==0x31 && bus.writes[1].second==std::vector<uint8_t>({0x81,1})); // IRQ spans bytes
    auto &raw=bus.reads[{0x181,80}];
    raw[0]=1;raw[1]=0x37;raw[2]=4;raw[3]=0x7f;raw[4]=7;raw[5]=90;raw[6]=4;raw[7]=5;
    raw[8]=6;raw[9]=20;raw[11]=30;
    raw[16]=3; // palm
    RMI4::Frame frame;
    assert(device.sample(frame)==RMI4::Result::Success && frame.contacts[0].active && frame.contacts[1].active && !frame.contacts[2].active);
    assert(frame.contacts[0].x==1079 && frame.contacts[0].y==1919);
    uint8_t report[TouchHID::ReportBytes];
    TouchHID::Transform transform;
    assert(TouchHID::report(frame,1079,1919,transform,6553700000ULL,report));
    assert(report[0]==1 && report[1]==7 && report[2]==0 && RMI4::le16(report+3)==32767);
    frame.contacts[0].width=17;frame.contacts[0].height=31;frame.contacts[0].pressure=0x65;
    assert(TouchHID::report(frame,1079,1919,transform,6553700000ULL,report));
    assert(report[7]==17 && report[8]==31 && report[9]==0);
    assert(report[91]==2 && RMI4::le16(report+92)==1);
    transform.rotation=90;
    assert(TouchHID::report(frame,1079,1919,transform,0,report) && RMI4::le16(report+3)==0 && RMI4::le16(report+5)==32767);
    transform.rotation=45;assert(!TouchHID::report(frame,1079,1919,transform,0,report));
    raw[1]=0xff;raw[2]=0xff;
    assert(device.sample(frame)==RMI4::Result::Invalid && frame.contacts[0].x==1079);
    bus.data(0x60,{0x80,0,0});assert(device.sample(frame)==RMI4::Result::Reset);
    bus.data(0x60,{0x40,0,0});assert(device.sample(frame)==RMI4::Result::Bootloader);
    assert(device.restore());
    assert(bus.writes.back().first==0x30 && bus.writes.back().second[0]==4);
    auto crcBus=fixture();crcBus.crcReads=2;RMI4::Device<Bus> crcDevice(crcBus);
    assert(crcDevice.initialize()==RMI4::Result::Success && crcBus.delays==20 && crcBus.pdtReads==20);
    // Do not adopt a failed restore as a new baseline; do not write any new
    // report format until the original controller configuration is restored.
    auto rollbackBus=fixture();RMI4::Device<Bus> rollback(rollbackBus);
    assert(rollback.initialize()==RMI4::Result::Success);
    rollbackBus.failWrite=0x31;
    assert(!rollback.restore());
    unsigned before=rollbackBus.pdtReads;
    assert(rollback.initialize()==RMI4::Result::IO && rollbackBus.pdtReads==before);
    rollbackBus.failWrite=-1;assert(rollback.initialize()==RMI4::Result::Success);
    auto f11=fixture(false);RMI4::Device<Bus> old(f11);assert(old.initialize()==RMI4::Result::Success);
    auto &f11raw=f11.reads[{0x180,53}];f11raw[0]=1;f11raw[3]=0x12;f11raw[4]=0x23;f11raw[5]=0x45;
    assert(old.sample(frame)==RMI4::Result::Success && frame.contacts[0].x==0x125 && frame.contacts[0].y==0x234);
    f11raw[0]=3;assert(old.sample(frame)==RMI4::Result::Invalid);
    f11raw[0]=0;assert(old.sample(frame)==RMI4::Result::Success && !frame.contacts[0].active);
    for (unsigned variant=0;variant<8;++variant) {
        auto invalid=fixture();
        if (variant==0) invalid.data(0x104,{4});
        if (variant==1) invalid.data(0x142,{1,11});
        if (variant==2) invalid.data(0x141,{0,0,0,0});
        if (variant==3) invalid.data(0x108,{0x10,1,0});
        if (variant==4) invalid.failRead=0x105;
        if (variant==5) invalid.data(0x60,{0x40});
        if (variant==6) invalid.crc=true;
        if (variant==7) invalid.failWrite=0x143;
        RMI4::Device<Bus> bad(invalid);
        assert(bad.initialize()!=RMI4::Result::Success);
        if (variant<7) assert(invalid.writes.empty());
        assert(invalid.delays<=100);
    }
    TouchHID::Delivery delivery;
    frame.contacts[2].active=true;
    assert(delivery.send(frame,[](const RMI4::Frame &){return true;}) && delivery.held());
    delivery.release();
    assert(!delivery.send(frame,[](const RMI4::Frame &){return false;}) && delivery.held() && delivery.releasing());
    assert(delivery.send(frame,[](const RMI4::Frame &f){for(auto &c:f.contacts)assert(!c.active);return true;}));
    assert(!delivery.held() && !delivery.releasing());
    // Firmware zeroes departed objects. Untouch must retain the last
    // accepted XY, including releases after a failed HID submission.
    TouchHID::Delivery paths;
    RMI4::Frame down;down.contacts[0]={true,700,900,180,8,10};
    assert(paths.send(down,[](const RMI4::Frame &){return true;}));
    RMI4::Frame moved=down;moved.contacts[0].x=701;
    assert(!paths.send(moved,[](const RMI4::Frame &){return false;}));
    RMI4::Frame up;
    auto verifyUp=[&](const RMI4::Frame &f) {
        assert(!f.contacts[0].active && f.contacts[0].x==700 && f.contacts[0].y==900);
        assert(TouchHID::report(f,1079,1919,{},0,report));
        assert(report[1]==4 && report[7]==8 && report[8]==10 && report[9]==0 && report[91]==0);
        assert(RMI4::le16(report+3)==700U*32767/1079 && RMI4::le16(report+5)==900U*32767/1919);
        return true;
    };
    assert(paths.send(up,verifyUp));
    assert(paths.send(up,verifyUp));
    assert(paths.send(down,[](const RMI4::Frame &){return true;}));
    paths.release();
    assert(!paths.send(down,[](const RMI4::Frame &){return false;}) && paths.releasing());
    assert(paths.send(down,verifyUp) && !paths.releasing() && !paths.held());
    // Each supported transform preserves bounds, including the corners.
    frame={};frame.contacts[0].active=true;frame.contacts[0].x=1079;frame.contacts[0].y=0;
    for (unsigned flags=0;flags<8;++flags) for (unsigned rotation=0;rotation<360;rotation+=90) {
        TouchHID::Transform tr{bool(flags&1),bool(flags&2),bool(flags&4),rotation};
        assert(TouchHID::report(frame,1079,1919,tr,0,report));
        assert((RMI4::le16(report+3)==0 || RMI4::le16(report+3)==32767) &&
               (RMI4::le16(report+5)==0 || RMI4::le16(report+5)==32767));
    }
    checkDescriptor();
    puts("touch: F01/F11/F12 discovery, frames, IRQ masks, pressure-free HID and release recovery passed");
}
