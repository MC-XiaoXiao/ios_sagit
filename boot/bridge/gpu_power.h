// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2015, 2018 The Linux Foundation. All rights reserved.
// Copyright (c) 2019 Jeffrey Hugo
// Copyright (c) 2021, 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
// A540 boot adaptation, 2026-10-16. Pinned Linux GPUCC/GDSC/Fabia and
// MSM8998 OPP sources: docs/power/reference and docs/gpu/reference.
#pragma once
#include "gpu_firmware.h"
#include "../../drivers/clock/QcomGCC/GCCController.h"
#include "../../drivers/clock/QcomGPUCC/ClockTables.h"
#include "../../drivers/power/QcomRPM/RPMRegulators.h"
#include "../../drivers/power/QcomRPM/RPMTransport.h"

namespace BridgeGPU {
inline uint32_t requiredGPUCorner(uint32_t hz) {
    const uint32_t points[][2] = {{257000000,48},{342000000,64},{414000000,128},
        {515000097,192},{596000097,256},{670000048,320},{710000097,384}};
    for (const auto &p : points) if (p[0]==hz) return p[1];
    return 0;
}
// Only private GPU resources are switched. The control bus, XO, shared GPLL0
// and factory PLL analogue configuration remain firmware prerequisites.
class PowerStartup {
    QcomPower::IO io{};
    GCC::Controller &gcc;
    QcomPower::ClockHardware &gpucc;
    QcomPower::RPMRegulators &regulators;
    QcomPower::RPMTransport &transport;
    struct Saved { unsigned bank; uint32_t reg,value; } saved[40]{};
    unsigned count=0;
    uint32_t frequency=0,cx=0,mx=0;
    bool probed=false,touched=false;
    const char *currentStage="preflight";
    uint32_t currentResource=UINT32_MAX,currentResult=0;
    uint32_t read(uint32_t r,unsigned bank=0) {return io.read(io.context,bank,r);}
    void write(uint32_t r,uint32_t v) {touched=true;io.write(io.context,0,r,v);}
    void bits(uint32_t r,uint32_t mask,uint32_t v) {write(r,(read(r)&~mask)|(v&mask));}
    bool poll(uint32_t r,uint32_t mask,uint32_t v,unsigned timeout=1500) {
        for(unsigned us=0;us<=timeout;++us){const auto current=read(r);if(current==UINT32_MAX)return false;if((current&mask)==v)return true;if(us!=timeout)io.wait(io.context,1);}
        return false;
    }
    bool save(uint32_t r,unsigned bank=0) {
        const auto v=read(r,bank);if(v==UINT32_MAX || count==40)return false;
        saved[count++]={bank,r,v};return true;
    }
    static QcomPower::Status supply(void *context,uint32_t controller,uint32_t resource,uint32_t value) {
        auto &s=*static_cast<PowerStartup *>(context);uint32_t actual=0;
        if(controller!=2 || (resource!=0x1000 && resource!=0x1001) || !value)return QcomPower::Invalid;
        auto status=s.transport.check();if(status!=QcomPower::OK)return status;
        status=s.regulators.get(resource,0,actual);
        return status!=QcomPower::OK?status:actual>=value?QcomPower::OK:QcomPower::NotReady;
    }
    static QcomPower::Status parent(void *,int) {return QcomPower::Unsupported;}
    bool preparePLL() {
        // Retain factory CONFIG/TEST values. Program only rate, calibration,
        // output and operating-mode fields described by the Fabia driver.
        const uint64_t hz=uint64_t(frequency)*2;
        if((read(0)&(1u<<20)) || !read(0x14))return false;
        bits(0x1020,1,1);if(!poll(0x1020,0x80000000,0,200))return false;
        bits(0,1,0);write(0x2c,0);
        write(4,uint32_t(hz/19200000));write(0x38,uint32_t(((hz%19200000)*65536+19199999)/19200000));
        bits(0xc,0x300f07,0); // VCO0, even output /1; outputs gated
        if(!(read(0)&4)) {
            // Linux's Fabia prepare uses 54% of the sum of the VCO0 limits.
            constexpr uint64_t calibration=(249600000ULL+2000000000ULL)*54/100;
            write(8,uint32_t(calibration/19200000));
            bits(0,4,4);write(0x2c,1);
            if(!poll(0,0x80000000,0x80000000,500))return false;
            bits(0xc,7,7);bits(0,1,1);
            bits(0,1,0);bits(0xc,7,0);write(0x2c,0);
        }
        return (read(0)&4) && read(4);
    }
public:
    const char *stage() const {return currentStage;}
    uint32_t resource() const {return currentResource;}
    uint32_t result() const {return currentResult;}
    PowerStartup(GCC::Controller &g,QcomPower::ClockHardware &c,QcomPower::RPMRegulators &r,QcomPower::RPMTransport &t)
        :gcc(g),gpucc(c),regulators(r),transport(t) {}
    bool probe(QcomPower::IO access,const FirmwarePower &power,uint32_t cxLevel,uint32_t mxLevel) {
        if(probed || touched || !access.read || !access.write || !access.wait || !power.valid())return false;
        io=access;frequency=power.frequency;cx=cxLevel;mx=mxLevel;
        const auto corner=requiredGPUCorner(frequency);
        const bool cxOn=read(0x1008)&0x80000000;
        if(!corner || cx<corner || mx<corner || (cxOn ? bool(read(0x1004)&1) : !(read(0x1004)&1)) ||
            (read(0x1094)&0x80000000) || !(read(0x1094)&1) ||
            !(read(0x71004,1)&1) || !GCC::sharedPLLEnabled(read(0,1),read(0xc,1)) ||
            !read(4,1) || !read(0x14) || (read(0)&(1u<<20)))return false;
        const uint32_t cc[]={0,4,8,0xc,0x14,0x18,0x1c,0x20,0x2c,0x38,0x130,0x1000,0x1004,0x1008,
            0x1020,0x1030,0x1034,0x1050,0x1054,0x1070,0x1074,0x1090,0x1094,0x1098,0x10b0,0x10b4,0x10d0,
            0x1100,0x1104,0x1120,0x1124,0x10a0,0x10a4};
        for(auto r:cc)if(!save(r))return false;
        const uint32_t shared[]={0,4,0xc,0x38,0x71004};for(auto r:shared)if(!save(r,1))return false;
        probed=true;return unchanged();
    }
    bool unchanged() {
        if(!probed || touched)return false;
        for(unsigned i=0;i<count;++i)if(read(saved[i].reg,saved[i].bank)!=saved[i].value)return false;
        return true;
    }
    bool start() {
        using namespace QcomPower;
        currentStage="baseline";
        if(!unchanged() || supply(this,2,0x1000,cx)!=OK || supply(this,2,0x1001,mx)!=OK)return false;
        touched=true;
        // Acknowledge rails first; then enable only owned GCC GPU gates.
        // Match the GPU/SMMU/GPUCC consumer clocks in msm8998.dtsi.
        // BIMC_GFX_SRC and SNOC_DVM_GFX are not consumer prerequisites:
        // firmware can leave their enable bit set while the GX source is off.
        const uint32_t leaves[]={75,77,176,186};
        currentStage="gcc-gpu-gates";
        for(auto id:leaves) {
            currentResource=id;currentResult=gcc.clock(id,true);
            if(currentResult!=GCC::Success)return false;
        }
        // GX was proven OFF, so its prior CP context cannot execute.
        const uint32_t gates[]={0x1054,0x1098,0x10d0,0x1124};
        currentStage="gpucc-prepare";
        for(auto r:gates)bits(r,1,0);
        if(!gpucc.configure(QcomGPUCCData::description,io,{this,supply,parent}))return false;
        for(const auto &r:QcomGPUCCData::roots) {
            // The Fabia fractional divider rounds upward. Subsequent gate
            // admission checks the observed rate, so cover the same 100ppm
            // tolerance used to validate that rate against the chosen OPP.
            const uint32_t limit=r.id==3?frequency+frequency/10000+1:19200000;
            if(!gpucc.allowRoot(r.id,limit) || !gpucc.addSupply({r.id,limit,2,0x1000,cx}) ||
                !gpucc.addSupply({r.id,limit,2,0x1001,mx}))return false;
        }
        for(const auto &d:QcomGPUCCData::domains)if(!gpucc.allowDomain(d.id) ||
            !gpucc.addSupply({0x80000000u|d.id,0,2,0x1000,cx}) || !gpucc.addSupply({0x80000000u|d.id,0,2,0x1001,mx}))return false;
        for(const auto &b:QcomGPUCCData::branches)if(!gpucc.allowClock(b.id))return false;
        for(const auto &r:QcomGPUCCData::resets)if(!gpucc.allowReset(r.id))return false;
        currentStage="gpucc-initialize";if(!gpucc.initialize())return false;
        auto checked=[&](uint32_t resource,QcomPower::Status status) {
            currentResource=resource;currentResult=status;return status==OK;
        };
        currentStage="cx-domain";if(!checked(1,gpucc.domain(1,true)))return false;
        currentStage="cx-reset";if(!checked(0,gpucc.reset(0,false)))return false;
        currentStage="rbcpr-reset";if(!checked(1,gpucc.reset(1,false)))return false;
        // The vendor CRC initialization first runs at the low OPP. Bring up
        // the same257MHz baseline even when the final pinned OPP is higher.
        const uint32_t targetFrequency=frequency;
        frequency=257000000;
        currentStage="fabia-pll";if(!preparePLL())return false;
        currentStage="xo-roots";
        const uint32_t roots[]={2,4,5};for(auto id:roots)if(!checked(id,gpucc.setRate(id,19200000)))return false;
        currentStage="gx-rate";if(!checked(3,gpucc.setRate(3,frequency)))return false;
        currentStage="cx-clocks";
        const uint32_t cxClocks[]={10,6};
        for(auto id:cxClocks)if(!checked(id,gpucc.clock(id,true)))return false;
        currentStage="gx-domain";if(!checked(2,gpucc.domain(2,true)))return false;
        currentStage="gpucc-gates";
        const uint32_t clocks[]={8,7};
        for(auto id:clocks)if(!checked(id,gpucc.clock(id,true)))return false;
        // The downstream MSM8998 clock driver initializes CRC at a low
        // operating point with GX and gfx3d enabled. A clock-controller
        // branch ACK alone does not initialize the private CRC path.
        currentStage="gfx-crc";
        write(0x10a4,0x00015010);write(0x10a0,0x00800000);io.wait(io.context,1);
        if(read(0x10a4)!=0x00015010 || !(read(0x10a0)&0x00800000))return false;
        if(targetFrequency!=frequency) {
            // CP is not initialized/running yet. Gate the only consumer of
            // this PLL before the existing checked GPUCC retune; its sibling
            // RBBMTIMER/RBCPR roots remain on XO. Keep CRC and rails retained.
            currentStage="opp-gate";if(!checked(7,gpucc.clock(7,false)))return false;
            currentStage="opp-rate";if(!checked(3,gpucc.setRate(3,targetFrequency)))return false;
            currentStage="opp-clock";if(!checked(7,gpucc.clock(7,true)))return false;
        }
        frequency=targetFrequency;
        const uint64_t actual=gpucc.rate(7),error=actual>frequency?actual-frequency:frequency-actual;
        currentStage="rate-and-transport";
        return actual && error<=frequency/10000+1 && transport.check()==OK;
    }
    template<class GPURegisters> bool enableBlocks(GPURegisters &gpu) {
        return Adreno::enableCoreBlocks(gpu) && transport.check()==QcomPower::OK;
    }
};
} // namespace BridgeGPU
