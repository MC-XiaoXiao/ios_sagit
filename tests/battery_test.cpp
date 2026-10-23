// SPDX-License-Identifier: GPL-2.0-only
#include <PMI8998Battery.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <functional>
#include <vector>
using namespace PMICBattery;
struct Bus {
    std::array<uint8_t, 65536> bytes{};
    unsigned calls = 0, failAt = 0;
    std::function<void(unsigned, uint16_t)> before;
    std::vector<uint16_t> reads;
    Bus() {
        bytes[0x103]=2; bytes[0x104]=0x51; bytes[0x105]=0x15;
        bytes[0x4005]=0x10; bytes[0x4105]=0x11;
        bytes[0x4046]=0x80; bytes[0x4010]=2;
        bytes[0x4009]=bytes[0x400a]=128;
        bytes[0x4106]=1;
        bytes[0x4150]=0xa8; bytes[0x4151]=4; // 298 K = 24.85 C
        measurement(32000, 2048); // 3906 mV, -999 mA in Apple's convention
        bytes[0x1006]=7;
    }
    void measurement(uint16_t voltage, int16_t current) {
        bool swapped = bytes[0x103] == 1;
        uint16_t values[] = {voltage, uint16_t(current)};
        for (unsigned i=0;i<2;++i) {
            bytes[0x41a0+i*2] = swapped ? values[i]>>8 : values[i]&255;
            bytes[0x41a1+i*2] = swapped ? values[i]&255 : values[i]>>8;
        }
        for (unsigned i=0;i<4;++i) bytes[0x41a6+i]=bytes[0x41a0+i];
    }
    // No write method: the production reader cannot issue PMIC writes.
    bool read(uint8_t sid, uint16_t address, uint8_t *out, unsigned n) {
        assert(sid==2 && n && n<=8 && unsigned(address)+n<=bytes.size());
        ++calls; reads.push_back(address);
        if (before) before(calls, address);
        if (calls==failAt) return false;
        for (unsigned i=0;i<n;++i) out[i]=bytes[address+i];
        return true;
    }
};
static Sample sample(Bus &b) {
    Sample s; assert(readSample(b, Config{}, s)==Result::Success); return s;
}
int main() {
    Bus normal;
    auto s=sample(normal);
    assert(s.installed && !s.external && !s.charging && !s.full);
    assert(s.percent==50 && s.millivolts==3906 && s.milliamps==-999);
    assert(s.temperatureValid && s.centiCelsius==2485);
    const unsigned successfulReads=normal.calls;
    // Fail every individual I/O in a complete transaction: never commit
    // fabricated zero/half-updated telemetry, and preserve the original error.
    for (unsigned i=1;i<=successfulReads;++i) {
        Bus b; b.failAt=i; Sample out{}; out.percent=77;
        assert(readSample(b, Config{}, out)==Result::IOFailure && out.percent==77);
        assert(b.calls==i);
    }
    for (unsigned rev : {1,2}) {
        Bus b; b.bytes[0x103]=rev; b.measurement(32000,-2048);
        b.bytes[0x160b]=0x11; b.bytes[0x1006]=2; b.bytes[0x100d]=0x20;
        auto v=sample(b); assert(v.milliamps==999 && v.charging && v.external && !v.full);
        b.bytes[0x1007]=2; assert(!sample(b).charging); // thermal inhibit
        b.bytes[0x1007]=0; b.bytes[0x100d]=0; assert(!sample(b).charging);
        b.bytes[0x100d]=0x20; b.bytes[0x160b]=0x10; assert(!sample(b).external);
        b.bytes[0x160b]=0x09; assert(sample(b).external); // DC input
        b.bytes[0x160b]=0x01; assert(!sample(b).external); // no selected input
    }
    for (unsigned raw=0;raw<256;++raw) {
        Bus b;b.bytes[0x4009]=b.bytes[0x400a]=raw;
        auto v=sample(b); assert(v.percent<=100 && v.percent==(raw*100+127)/255);
    }
    for (int current : {-32768,-1,0,1,32767}) {
        Bus b;b.measurement(32000,int16_t(current));
        assert(sample(b).milliamps==-(int64_t(current)*488281/1000000));
    }
    Bus full;full.bytes[0x160b]=0x11;full.bytes[0x1006]=5;
    full.bytes[0x4009]=full.bytes[0x400a]=255; assert(sample(full).full);
    full.bytes[0x1006]=6;assert(!sample(full).full);
    full.bytes[0x1006]=5;full.bytes[0x4009]=full.bytes[0x400a]=128;assert(!sample(full).full);
    Bus missing;missing.bytes[0x4107]=4;missing.bytes[0x4046]=0;
    s=sample(missing);assert(!s.installed && !s.charging && !s.temperatureValid);
    for (auto address : missing.reads) assert(address!=0x41a0 && address!=0x4009);
    for (uint16_t address : {0x4046,0x4010}) {
        Bus b;b.bytes[address]=0;Sample out;
        assert(readSample(b,Config{},out)==Result::NotReady);
    }
    Bus restart;restart.bytes[0x4048]=1;
    assert(readSample(restart,Config{},s)==Result::NotReady);
    for (uint16_t address : {0x103,0x104,0x105,0x4005,0x4105}) {
        Bus b;b.bytes[address]=0xff;
        assert(readSample(b,Config{},s)==Result::Unsupported);
    }
    for (unsigned field=0;field<7;++field) {
        Config c;uint32_t *fields[]={&c.version,&c.sid,&c.soc,&c.info,&c.charger,&c.misc,&c.pollMS};
        *fields[field]=0xffffffff;Bus b;
        assert(readSample(b,c,s)==Result::InvalidConfig && b.calls==0);
    }
    Bus capRace;capRace.bytes[0x400a]=1;
    assert(readSample(capRace,Config{},s)==Result::Incoherent && capRace.calls<20);
    Bus capRecovery;unsigned capReads=0;
    capRecovery.before=[&](unsigned,uint16_t address) {
        if(address==0x4009)capRecovery.bytes[0x400a]=++capReads<3 ? 1:128;
    };
    assert(sample(capRecovery).percent==50 && capReads==4);
    Bus shadow;shadow.bytes[0x41a6]^=1;s.percent=77;
    assert(readSample(shadow,Config{},s)==Result::Incoherent && s.percent==77 && shadow.calls<70);
    Bus transition;unsigned pathReads=0;
    transition.before=[&](unsigned,uint16_t address) {
        if(address==0x160b)transition.bytes[address]=++pathReads==1 ? 0 : 0x11;
    };
    assert(sample(transition).external && pathReads==4);
    Bus removal;unsigned presentReads=0;
    removal.before=[&](unsigned,uint16_t address) {
        if(address==0x4107)removal.bytes[address]=++presentReads==1 ? 0 : 4;
    };
    assert(!sample(removal).installed);
    Bus absentTemp;absentTemp.bytes[0x4106]=0;
    assert(!sample(absentTemp).temperatureValid);
    Bus invalidTemp;invalidTemp.bytes[0x4151]=7;
    assert(!sample(invalidTemp).temperatureValid);
    for (unsigned voltage : {0,65535}) {
        Bus b;b.measurement(voltage,0);s.percent=77;
        assert(readSample(b,Config{},s)==Result::OutOfRange && s.percent==77);
    }
    puts("Battery: real register decoding, revisions, signs, 256 SOC values, bounded coherence, all-read fault injection passed");
}
