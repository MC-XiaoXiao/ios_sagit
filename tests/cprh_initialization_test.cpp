// SPDX-License-Identifier: GPL-2.0-only
#include "../drivers/clock/QcomOSM/CPRhInitialization.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace QcomCPRh;
struct Mock {
    uint32_t regs[2][0x4000/4]={};
    unsigned reads=0,writes=0,barriers=0,failRead=0,failWrite=0,dropWrite=0;
    unsigned elapsed=0;
    struct Write {unsigned cluster;uint32_t offset,value;};
    std::vector<Write> history;
    static bool read(void *ctx,unsigned c,uint32_t o,uint32_t &v) {
        Mock &m=*static_cast<Mock*>(ctx);
        assert(c<2 && o<0x4000 && o%4==0);
        if(++m.reads==m.failRead)return false;
        v=m.regs[c][o/4];return true;
    }
    static bool write(void *ctx,unsigned c,uint32_t o,uint32_t v) {
        Mock &m=*static_cast<Mock*>(ctx);
        assert(c<2 && o<0x4000 && o%4==0);
        if(++m.writes==m.failWrite)return false;
        m.history.push_back({c,o,v});
        if(m.writes!=m.dropWrite)m.regs[c][o/4]=v;
        return true;
    }
    static void barrier(void *ctx){static_cast<Mock*>(ctx)->barriers++;}
    static void delay(void *ctx,uint32_t us) {
        static_cast<Mock*>(ctx)->elapsed+=us;
    }
    RegisterIO io(){return {this,read,write,barrier,delay};}
};
static Plan fixture(unsigned cluster) {
    // Synthetic fuse values, never a replacement for per-device efuse reads.
    FuseRows r={};
    r.row67=(uint64_t(2200)<<46)|(uint64_t(1900&63)<<58);
    r.row68=(uint64_t(1000)<<18)|(uint64_t(1300)<<6)|(1900>>6);
    r.row69=uint64_t(2200&15)<<60;
    r.row70=(uint64_t(1000)<<32)|(uint64_t(1300)<<20)|(uint64_t(1900)<<8)|(2200>>4);
    Plan p;assert(calibrate({292,0x20000,30},r,cluster,p));return p;
}
int main() {
    for(unsigned cluster=0;cluster<2;cluster++) {
        Plan p=fixture(cluster);Mock m;auto io=m.io();
        // Reserved bits survive updates to fields documented by Qualcomm.
        m.regs[cluster][0x3aa0/4]=0xa000f800;
        m.regs[cluster][0x7f8/4]=0x02000008;
        assert(inspect(p,io));assert(m.writes==0);
        assert(apply(p,io));assert(verify(p,io));
        assert(m.regs[cluster][0x004/4]==0x21f);
        assert(m.regs[cluster][0x00c/4]==96000);
        assert(m.regs[cluster][0x0a0/4]==19);
        assert(m.regs[cluster][0x014/4]==(cluster ? 0x389u : 0x30bu));
        assert(m.regs[cluster][0x808/4]==0x4202);
        assert(m.regs[cluster][0x7a8/4]==(33u<<18));
        assert(m.regs[cluster][0x7a4/4]==0x21);
        assert(m.regs[cluster][0x7f8/4]==0x02004098);
        assert(m.regs[cluster][0x3aa0/4]==(0xa000f800|0x080a00b1));
        assert(m.history.back().offset==0x004 && (m.history.back().value&1));
        unsigned writes=m.writes,reads=m.reads;
        assert(apply(p,io));assert(m.writes==writes && m.reads>reads);
        assert(disable(p,io));
        assert(!(m.regs[cluster][0x004/4]&1));assert(!(m.regs[cluster][0x3aa0/4]&1));
        // Every individual register write failure must reject initialization;
        // recovery writes disable the loop and detach OSM.
        for(unsigned fail=1;fail<=writes;fail++) {
            Mock bad;bad.failWrite=fail;assert(!apply(p,bad.io()));
            assert(!(bad.regs[cluster][0x004/4]&1));
            assert(!(bad.regs[cluster][0x3aa0/4]&1));
        }
        // A lost nonzero corner write must be detected by readback.
        Mock lost;lost.dropWrite=1;
        assert(!apply(p,lost.io()));assert(!(lost.regs[cluster][0x004/4]&1));
        for(unsigned fail=1;fail<=reads;fail++) {
            Mock bad;bad.failRead=fail;
            bool ok=apply(p,bad.io());
            if(bad.reads>=fail)assert(!ok);
            assert(ok || !(bad.regs[cluster][0x004/4]&1));
        }
        Mock running;running.regs[cluster][1]=1;
        assert(!inspect(p,running.io()));assert(!apply(p,running.io()));
        assert(!running.writes);
        Mock busy;busy.regs[cluster][2]=1;
        assert(!inspect(p,busy.io()));assert(!busy.writes);
        Mock inaccessible;inaccessible.regs[cluster][1]=0xffffffff;
        assert(!inspect(p,inaccessible.io()));assert(!inaccessible.writes);
        Plan corrupt=p;corrupt.corners[3].word^=1;
        Mock untouched;assert(!apply(corrupt,untouched.io()));assert(!untouched.writes);
        corrupt=p;corrupt.baseQuotient[0]++;
        assert(!apply(corrupt,untouched.io()));assert(!untouched.writes);
        corrupt=p;corrupt.count=41;
        assert(!apply(corrupt,untouched.io()));assert(!untouched.writes);
        corrupt=p;corrupt.cluster=2;
        assert(!apply(corrupt,untouched.io()));assert(!untouched.writes);
        Ownership ownership={true,true,true,true};
        Mock takeover;takeover.regs[cluster][1]=1;takeover.regs[cluster][2]=1;
        assert(inspect(p,takeover.io(),ownership));assert(!takeover.writes);
        assert(apply(p,takeover.io(),ownership));assert(verify(p,takeover.io()));
        assert(takeover.elapsed==0);
        assert(takeover.history.front().offset==0x004 && !(takeover.history.front().value&1));
        Mock firmware;
        firmware.regs[cluster][1]=0xffffffff;
        // Bit 8 is reserved and survives; all implemented repeat bits are set.
        firmware.regs[cluster][1]&=~0xc0u;
        firmware.regs[cluster][2]=1;
        assert(apply(p,firmware.io(),ownership) && verify(p,firmware.io()));
        assert(firmware.elapsed==0 && firmware.history.front().value==0x31e);
        assert(firmware.regs[cluster][1]==0x31f);
        const unsigned takeoverWrites=takeover.writes;
        // Failure of any takeover read or write must propagate; in particular
        // a failed read cannot be mistaken for a harmless state mismatch.
        for(unsigned fail=1;fail<=takeoverWrites;fail++) {
            Mock bad;bad.regs[cluster][1]=1;bad.failWrite=fail;
            assert(!apply(p,bad.io(),ownership));
            assert(!(bad.regs[cluster][1]&1));
        }
        Mock normalTakeover;normalTakeover.regs[cluster][1]=1;
        assert(apply(p,normalTakeover.io(),ownership));
        for(unsigned fail=1;fail<=normalTakeover.reads;fail++) {
            Mock bad;bad.regs[cluster][1]=1;bad.failRead=fail;
            assert(!apply(p,bad.io(),ownership));
            // Preflight failures leave the firmware state untouched.
            if(bad.writes)assert(!(bad.regs[cluster][1]&1));
        }
        Mock stuck;stuck.regs[cluster][1]=1;stuck.regs[cluster][2]=1;
        assert(apply(p,stuck.io(),ownership) && verify(p,stuck.io()));
        assert(stuck.elapsed==0 && stuck.regs[cluster][2]==1);
        // A retained BUSY status does not block vendor initialization, but
        // the sensing loop must stay disabled throughout all corner writes.
        bool loopEnabled=true;
        for(const auto &w:stuck.history) {
            if(w.offset==0x004) loopEnabled=w.value&1;
            if(w.offset>=0x3a00 && w.offset<0x3aa0) assert(!loopEnabled);
        }
        assert(loopEnabled);
        for(unsigned flag=0;flag<4;flag++) {
            Ownership denied=ownership;
            if(flag==0)denied.bootServicesExited=false;
            if(flag==1)denied.bothOSMBanksDisabled=false;
            if(flag==2)denied.tzBootParametersMatched=false;
            if(flag==3)denied.clockVerified=false;
            Mock no;assert(!apply(p,no.io(),denied));assert(!no.writes);
        }
    }
    // Bridge ordering: both read-only inspections precede any writes.
    Plan p[2]={fixture(0),fixture(1)};Mock dual;
    dual.regs[1][2]=1;
    assert(inspect(p[0],dual.io()) && !inspect(p[1],dual.io()));
    assert(!dual.writes);
    std::puts("CPRh initialization: register values, order, readback, idempotence and per-I/O failure checks passed");
}
