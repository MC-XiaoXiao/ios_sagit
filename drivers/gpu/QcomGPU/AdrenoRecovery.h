// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "AdrenoStartup.h"
namespace Adreno {
enum class RecoveryStage : uint32_t { Preflight,Reset,Mapper,Blocks,MEInit,GPMU,ZAP,Timestamp,Complete };
struct RecoveryResult {GPU::Result result;RecoveryStage stage;bool dmaStopped;};

// The caller closes admission, retires failed jobs without replay, and drains
// old Family callbacks before entering. Guard checks real resource leases,
// pinned firmware PTEs and the exact SMMU identity after every reset stage.
template<class Registers,class Guard>
RecoveryResult recover(const PlatformContract &cp,const FirmwareHandoff &firmware,
    Registers &r,Guard &guard,Engine<Registers> &engine,volatile uint32_t *ring,uint64_t ringVA,
    volatile uint32_t *fence,uint64_t fenceVA) {
    Startup<Registers> startup(r);
    auto failed=[&](GPU::Result result,RecoveryStage stage) {
        // Loss of clocks/NoC/SCM ownership forbids further GPU MMIO. The
        // caller pins all DMA and service resources when stopped is false.
        const bool stopped=guard.accessible() && startup.stop() && engine.stop() && guard.isolated();
        return RecoveryResult{result,stage,stopped};
    };
    if(!firmware.valid(cp) || !ring || !fence || engine.ownsHardware() ||
        ((ringVA|fenceVA)&4095) || !deviceRange(ringVA,RingDwords*4) || !deviceRange(fenceVA,4096) ||
        (ringVA<fenceVA+4096 && fenceVA<ringVA+RingDwords*4))return failed(GPU::Result::Invalid,RecoveryStage::Preflight);
    if(!guard.healthy() || !guard.isolated())return failed(GPU::Result::Offline,RecoveryStage::Preflight);
    if(!startup.reset())return failed(GPU::Result::Fault,RecoveryStage::Reset);
    if(!guard.healthy() || !guard.synchronize())return failed(GPU::Result::Fault,RecoveryStage::Mapper);
    if(!enableCoreBlocks(r) || !guard.healthy())return failed(GPU::Result::Fault,RecoveryStage::Blocks);
    if(!startup.start(cp,ring,ringVA) || !guard.healthy())return failed(GPU::Result::Fault,RecoveryStage::MEInit);
    if(!startup.gpmu(firmware.power(),firmware.gpmu,firmware.gpmuWords) || !guard.healthy())
        return failed(GPU::Result::Fault,RecoveryStage::GPMU);
    if(!startup.stop() || !guard.healthy() || !guard.isolated() || !guard.restoreZAP() ||
        !guard.healthy() || !guard.isolated())return failed(GPU::Result::Fault,RecoveryStage::ZAP);
    const auto result=engine.start(cp,ring,ringVA,fence,fenceVA,{true,true});
    if(result!=GPU::Result::Success || !guard.healthy())
        return failed(result==GPU::Result::Success?GPU::Result::Fault:result,RecoveryStage::Timestamp);
    return {GPU::Result::Success,RecoveryStage::Complete,false};
}
} // namespace Adreno
