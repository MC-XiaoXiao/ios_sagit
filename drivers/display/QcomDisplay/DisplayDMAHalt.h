// SPDX-License-Identifier: GPL-2.0-only
// DPU3 SSPP/XIN/clock topology and VBIF halt handshake follow Linux msm DPU.
#pragma once
#include <QcomPowerHardware.h>

namespace Display { namespace MMU {
// IO addresses are relative to MDSS (0xc900000). This deliberately accepts
// only one full-frame VIG0/DMA0 on CTL0/LM0 and one command-mode DSI output.
// Every other fetch pipe and control path must be inactive. EFI software
// producers must already have stopped before halt() is called.
template<class IO> class DMAHalt {
    IO &io;
    uint32_t source=0,xin=0,clockBit=0,clockBefore=0,haltBefore=0;
    uint32_t framebuffer=0,size=0,layer=0,top=0,stride=0;
    bool configured=false,touched=false,held=false;
    static constexpr uint32_t Halt=0xb0200,Ack=0xb0204,Clock=0x12ac;
    bool solePipeline() {
        if(io.accessible()!=QcomPower::OK || (io.read(0)&0xffff0000)!=0x30000000)return false;
        const unsigned pipes[]={0x4000,0x6000,0x8000,0xa000,0x24000,0x26000,0x28000,0x2a000};
        for(auto pipe:pipes) {
            const auto at=0x1000+pipe;
            if(pipe!=source){if(io.read(at))return false;continue;}
            if(io.read(at)!=size || io.read(at+0xc)!=size || io.read(at+8) || io.read(at+0x10) ||
                io.read(at+0x14)!=framebuffer || io.read(at+0x24)!=stride)return false;
        }
        const unsigned mixers[]={0,1,2,5};
        for(unsigned ctl=0;ctl<5;++ctl) {
            const auto at=0x2000+ctl*0x200;
            for(auto lm:mixers) {
                const auto mix=io.read(at+(lm==5?0x24:4*lm));
                if(!ctl && !lm){if(mix!=layer)return false;}
                else if(mix&~(1u<<24))return false;
                if(io.read(at+0x40+4*lm) || io.read(at+0x70+4*lm))return false;
            }
            const auto mode=io.read(at+0x14);
            if(!ctl){if(mode!=top || (io.read(at+0x18)&(source==0x4000?1u:1u<<11)))return false;}
            else if((mode && mode!=0x1f00) || io.read(at+0x18))return false;
        }
        const auto output=((top>>4)&7)-1;
        for(unsigned n=0;n<4;++n)if(n!=output && io.read(0x1000+0x6a000+n*0x800))return false;
        return true;
    }
    bool write(uint32_t offset,uint32_t value,uint32_t mask=UINT32_MAX) {
        io.write(offset,value);const auto after=io.read(offset);
        return after!=UINT32_MAX && (after&mask)==(value&mask);
    }
    bool waitAck(uint32_t expected) {
        for(unsigned n=0;n<2000;++n) {
            const auto v=io.read(Ack);if(v==UINT32_MAX)return false;
            if((v&(1u<<xin))==expected)return true;
            io.delay(10);
        }
        return false;
    }
public:
    explicit DMAHalt(IO &access):io(access) {}
    bool prepare(uint64_t address,unsigned width,unsigned height) {
        if(configured || io.accessible()!=QcomPower::OK || !address || address>UINT32_MAX || !width || width>4096 || !height || height>4096)return false;
        framebuffer=uint32_t(address);size=width|(height<<16);
        unsigned matches=0;
        const unsigned sources[]={0x4000,0x24000};
        for(auto pipe:sources)if(io.read(0x1000+pipe)==size && io.read(0x1000+pipe+0x14)==framebuffer){source=pipe;++matches;}
        if(matches!=1)return false;
        xin=source==0x4000?0:1;clockBit=source==0x4000?1u:1u<<8;
        stride=io.read(0x1000+source+0x24);layer=io.read(0x2000);top=io.read(0x2014);
        const auto mask=7u<<(source==0x4000?0:18),output=((top>>4)&7)-1;
        if(!(layer&mask) || (layer&~(mask|(1u<<24))) || !(top&(1u<<17)) ||
            (top&~0x20070u) || output<1 || output>2 || !solePipeline())return false;
        clockBefore=io.read(Clock);haltBefore=io.read(Halt);
        if(clockBefore==UINT32_MAX || haltBefore==UINT32_MAX || (haltBefore&(1u<<xin)))return false;
        configured=true;return true;
    }
    bool halt() {
        if(!configured || touched || !solePipeline())return false;
        const auto clock=io.read(Clock);
        if(clock==UINT32_MAX || (clock&clockBit)!=(clockBefore&clockBit) || io.read(Halt)!=haltBefore)return false;
        touched=true;
        // Only the force-enable bit is owned. Other fields include live clock
        // status, which can change in response to this very write.
        if(!write(Clock,clock|clockBit,clockBit) || !write(Halt,haltBefore|(1u<<xin)) || !waitAck(1u<<xin))return false;
        held=true;return isolated()==QcomPower::OK;
    }
    QcomPower::Status isolated() {
        if(!held || !solePipeline())return QcomPower::Busy;
        const auto ack=io.read(Ack),clock=io.read(Clock);
        return clock!=UINT32_MAX && (clock&clockBit) &&
            io.read(Halt)==(haltBefore|(1u<<xin)) && ack!=UINT32_MAX && (ack&(1u<<xin)) ? QcomPower::OK:QcomPower::Busy;
    }
    // Safe after a failed halt only if no SMMU register has been changed.
    // A failed context transition keeps the bus halted until a real reset.
    bool resume() {
        if(!touched)return configured;
        if(!write(Halt,haltBefore) || !waitAck(0))return false;
        const auto clock=io.read(Clock);
        if(clock==UINT32_MAX || !write(Clock,(clock&~clockBit)|(clockBefore&clockBit),clockBit))return false;
        held=false;touched=false;return solePipeline();
    }
    unsigned client() const {return xin;}
};
} }
