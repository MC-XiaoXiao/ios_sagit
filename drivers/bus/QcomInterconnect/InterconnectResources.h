// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <QcomPowerHardware.h>
namespace QcomNoC {
using namespace QcomPower;
// The dedicated MSM8998 USB path. Backend serializes actual RPM/GCC requests
// and AP QoS MMIO. No root-clock or bandwidth flag substitutes for an ACK.
template<class Backend> class USBPath {
    Backend &backend;
    struct Vote { uint32_t id; uint64_t value=0; bool held=false; } votes[8] = {
        {0x2000},{0x2001},{0x2002},{0x2100},{0x2101},{0x2102},{0x2103},{0x2104}
    };
    struct Clock { uint32_t id; bool held=false; } clocks[2]={{71},{32}};
    bool uncertain=false, qos=false, granted=false;
    uint32_t priority=0, mode=0;
    Status changed(Status status) {
        if (status==TimedOut || status==Stale || status==Failed) uncertain=true;
        return status;
    }
    Status update(uint32_t offset,uint32_t mask,uint32_t value) {
        const auto before=backend.read(offset);
        if (before==UINT32_MAX) return Failed;
        backend.write(offset,(before&~mask)|(value&mask));
        const auto after=backend.read(offset);
        return after!=UINT32_MAX && (after&mask)==(value&mask) ? OK : Failed;
    }
    Status restoreQoS() {
        auto s=update(Mode,3,0); if (s!=OK) return s;
        s=update(Priority,12,priority); if (s!=OK) return s;
        s=update(Priority,3,priority); if (s!=OK) return s;
        s=update(Mode,3,mode); if (s==OK) qos=false;
        return s;
    }
    static uint64_t up(uint64_t value,uint64_t divisor) { return value/divisor+(value%divisor!=0); }
public:
    // base 0x01660000, fabric offset 0x9000, USB qport2 * stride0x1000.
    static constexpr uint32_t Priority=0xb008, Mode=0xb00c;
    explicit USBPath(Backend &b):backend(b) {}
    bool faulted() const { return uncertain; }
    bool empty() const {
        if (uncertain || qos) return false;
        for (const auto &v:votes) if (v.held) return false;
        for (const auto &c:clocks) if (c.held) return false;
        return true;
    }
    Status supported() {
        for (unsigned i=0;i<8;++i) {
            uint64_t maximum=0;
            const auto s=backend.limit(votes[i].id,i<3,maximum);
            if (s!=OK) return s;
            if (!maximum) return NotReady;
        }
        return OK;
    }
    Status acquire(uint64_t average,uint64_t peak) {
        if (!empty()) return Busy;
        if (!average || average>peak || average>UINT64_MAX/153) return Invalid;
        uint64_t total[5]{};
        for (unsigned i=0;i<5;++i) {
            auto s=backend.floor(votes[i+3].id,false,total[i]); if (s!=OK) return s;
            if (total[i]>UINT64_MAX-average) return Invalid;
            total[i]+=average;
        }
        if (total[3]>UINT64_MAX/153 || total[4]>UINT64_MAX/153) return Invalid;
        // Only average is split over BIMC's two channels. Peak is not split.
        const uint64_t bimcTotal=total[3]>total[4] ? total[3] : total[4];
        const uint64_t bimcAverage=up(up(bimcTotal*153,100),2);
        const uint64_t bimc=up(peak>bimcAverage ? peak : bimcAverage,8);
        uint64_t rates[]={up(peak,16),up(peak,16),bimc};
        if (up(total[0],16)>rates[0]) rates[0]=up(total[0],16);
        if (up(total[1],16)>rates[1]) rates[1]=up(total[1],16);
        if (up(up(total[2],2),32)>rates[1]) rates[1]=up(up(total[2],2),32);
        for (unsigned i=0;i<3;++i) {
            if (rates[i]<19200000) rates[i]=19200000;
            if (rates[i]>UINT32_MAX-999) return Invalid;
            votes[i].value=up(rates[i],1000)*1000;
        }
        for (unsigned i=3;i<8;++i) votes[i].value=average;
        for (unsigned i=0;i<8;++i) {
            uint64_t maximum=0;
            auto s=backend.limit(votes[i].id,i<3,maximum); if (s!=OK) return s;
            if (votes[i].value>maximum) return Invalid;
        }
        auto s=backend.isolated(); if (s!=OK) return s;
        granted=false;
        for (unsigned i=0;i<8;++i) {
            // Establish an explicit sleep baseline before raising active
            // votes; an invalid inherited sleep set may mirror active RPM.
            s=changed(backend.vote(votes[i].id,1,i<3,0)); if (s!=OK) return s;
            s=changed(backend.vote(votes[i].id,0,i<3,votes[i].value)); if (s!=OK) return s;
            votes[i].held=true;
        }
        for (auto &c:clocks) {
            s=changed(backend.clock(c.id,true)); if (s!=OK) return s;
            c.held=true;
        }
        priority=backend.read(Priority); mode=backend.read(Mode);
        if (priority==UINT32_MAX || mode==UINT32_MAX) return Failed;
        qos=true; // a failed write still owns the saved fields
        s=update(Priority,12,4); if (s!=OK) return s; // P1 before P0
        s=update(Priority,3,1); if (s!=OK) return s;
        s=update(Mode,3,0); if (s!=OK) return s;
        granted=true; return check();
    }
    Status check() {
        if (uncertain) return Stale;
        if (!granted || !qos) return NotReady;
        for (unsigned i=0;i<8;++i) {
            uint64_t value=0;
            const auto s=backend.get(votes[i].id,0,i<3,value);
            if (s!=OK) return s;
            if (value<votes[i].value) return NotReady;
        }
        const auto p=backend.read(Priority), m=backend.read(Mode);
        return p!=UINT32_MAX && m!=UINT32_MAX && (p&15)==5 && !(m&3) ? OK : Failed;
    }
    Status release() {
        granted=false;
        if (empty()) return OK;
        // Partial/unknown updates retain all remaining references. Only a
        // real, fresh DWC3 BCR observation permits a lowering operation.
        if (uncertain) return Stale;
        auto s=backend.isolated(); if (s!=OK) return s;
        if (qos) { s=restoreQoS(); if (s!=OK) return s; }
        for (unsigned i=2;i;--i) if (clocks[i-1].held) {
            s=changed(backend.clock(clocks[i-1].id,false)); if (s!=OK) return s;
            clocks[i-1].held=false;
        }
        for (unsigned i=8;i;--i) if (votes[i-1].held) {
            s=changed(backend.vote(votes[i-1].id,0,i-1<3,0)); if (s!=OK) return s;
            votes[i-1].held=false;
        }
        return OK;
    }
};
}
