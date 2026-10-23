// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2013 ARM Limited
// Copyright (c) 2019 The Linux Foundation. All rights reserved.
// MSM8998 GPU bank-0 adaptation, 2026-10-16. Register programming follows the
// pinned Linux arm-smmu{,-qcom}.c/.h in docs/gpu/reference/sources.json.
#pragma once
#include "../../drivers/gpu/QcomGPU/AdrenoMMU.h"
#include "../../drivers/bus/QcomSMMU/ContextTLB.h"

namespace BridgeGPU {
// Configure an owned non-secure context, with the actual GPU SID (0). There
// is no global SMMU reset, bypass probe, identity map or secure-context access.
// IO provides read/write/barrier/delay and a real CP/AXI isolation check.
template<class IO> class SMMUStartup {
    IO &io;
    struct Register { uint32_t offset, value; } saved[280]{};
    unsigned count = 0, groups = 0, target = 0;
    uint32_t context = 0, input = 0, output = 0;
    uint32_t writtenOffset = UINT32_MAX, writtenValue = 0, readbackValue = 0;
    const char *phase = "uninitialized";
    bool probed = false, touched = false, configured = false;
    bool save(uint32_t offset) {
        const auto value = io.read(offset);
        if (value == UINT32_MAX || count == 280) return false;
        saved[count++] = {offset,value}; return true;
    }
    bool set(uint32_t offset, uint32_t value) {
        touched = true; io.write(offset,value); io.barrier();
        writtenOffset=offset;writtenValue=value;readbackValue=io.read(offset);
        if (readbackValue != value) return false;
        for (unsigned i = 0; i < count; ++i) if (saved[i].offset == offset) { saved[i].value = value; return true; }
        return false;
    }
public:
    explicit SMMUStartup(IO &access) : io(access) {}
    uint32_t inputBits() const { return input; }
    uint32_t outputBits() const { return output; }
    uint32_t bankOffset() const { return context; }
    unsigned streamSlot() const { return target; }
    bool ownsHardware() const { return touched; }
    uint32_t lastOffset() const { return writtenOffset; }
    uint32_t lastValue() const { return writtenValue; }
    uint32_t lastReadback() const { return readbackValue; }
    const char *stage() const { return phase; }
    bool unchanged() {
        if (!probed) return false;
        for (unsigned i = 0; i < count; ++i) {
            const auto value=io.read(saved[i].offset);
            const auto mask=saved[i].offset==context+0x58 ? ~SMMU::ContextFormatMask:UINT32_MAX;
            if ((value&mask) != (saved[i].value&mask)) {
                writtenOffset=saved[i].offset;writtenValue=saved[i].value;readbackValue=value;return false;
            }
        }
        return true;
    }
    // replaceStopped is reserved for the cold-power caller: it proved GX OFF
    // before allocating XNU RAM, then reset and isolated CP after EFI exited.
    bool probe(uint64_t first, uint64_t end, bool replaceStopped=false) {
        if (probed || touched) return false;
        count = 0;
        const uint32_t id0 = io.read(0x20), id1 = io.read(0x24), id2 = io.read(0x28);
        const unsigned widths[] = {32,36,40,42,44,48,52};
        const uint32_t globalPages = 1u << (((id1>>28)&7)+1);
        // This producer and native mapper use the MSM8998 64KiB register
        // aperture: 4KiB register pages and context bank 0 at 0x8000.
        if (id0 == UINT32_MAX || id1 == UINT32_MAX || id2 == UINT32_MAX ||
            (id0&((1u<<30)|(1u<<27))) != ((1u<<30)|(1u<<27)) || !(id2&(1u<<12)) ||
            (id1&(1u<<31)) || globalPages != 8 || !(id1&255) || (id1&255)>8 || (id1&0xff0000) ||
            (id2&15)>=7 || ((id2>>4)&15)>=7 || ((id2>>8)&15)>=7) return false;
        context = globalPages*4096; groups = id0&255;
        const unsigned sidBits = (id0>>9)&15;
        if (!groups || groups>128 || !sidBits) return false;
        input = widths[(id2>>8)&15]; if (input>40) input=40;
        output = widths[id2&15]; if (output>widths[(id2>>4)&15]) output=widths[(id2>>4)&15];
        if (output>40) output=40;
        if (first<0x1000000 || ((first|end)&4095) || end<=first || end>(1ULL<<input) ||
            end-first<0x800000 || end-first>0x20000000 ||
            ((io.read(context)&1) && !replaceStopped) || (replaceStopped && io.isolated()!=QcomPower::OK) ||
            SMMU::contextFault(io.read(context+0x58)) || io.read(0x48) || (io.read(0)&8)) return false;
        const unsigned global[] = {0,0x20,0x24,0x28,0x48,0x1000,0x1800};
        for (auto offset : global) if (!save(offset)) return false;
        const unsigned local[] = {0,0x10,0x20,0x24,0x28,0x2c,0x30,0x34,0x38,0x3c,0x58};
        for (auto offset : local) if (!save(context+offset)) return false;
        unsigned free = groups, existing = groups;
        for (unsigned slot = 0; slot < groups; ++slot) {
            const auto smr = io.read(0x800+4*slot), route = io.read(0xc00+4*slot);
            if (!save(0x800+4*slot) || !save(0xc00+4*slot)) return false;
            if (!(smr&(1u<<31))) { if (free==groups) free=slot; continue; }
            // Any masked/duplicate match for SID 0, or a different valid SID
            // translated through bank 0, would share the context we own.
            const uint32_t mask = (smr>>16)&32767, sid = smr&65535;
            const bool matchesZero = !(sid&~mask);
            if (matchesZero) {
                if (sid || mask || existing!=groups) return false;
                if (!(route&0x30000) && (route&255)) return false;
                existing=slot;
            } else if (!(route&0x300ff)) return false;
        }
        target = existing!=groups ? existing : free;
        if (target==groups) return false;
        probed=true; return unchanged();
    }
    bool initialize(uint64_t root, volatile uint64_t *entries) {
        phase="preflight";
        if (!probed || touched || configured || !entries || (reinterpret_cast<uintptr_t>(entries)&7) ||
            !root || (root&4095) || root>=(1ULL<<output) || io.isolated()!=QcomPower::OK || !unchanged()) return false;
        for (unsigned i = 0; i < 512; ++i) if (entries[i]) return false;
        // The root is private UC memory, zeroed and reserved before EFI exits.
        // Fault unmatched streams before temporarily invalidating the GPU SMR.
        const uint32_t global = (io.read(0)&~(1u|(3u<<14)|(1u<<13))) | 0x1c36;
        phase="context-registers";
        if (!set(0,global) || !set(0x800+4*target,0) || !set(context,0) ||
            !set(0x1800,1) || !set(0x1000,0x1f300) ||
            !set(context+0x10,(7u<<15)|(1u<<4)|(output==40?2u:output==36?1u:0u)) ||
            !set(context+0x30,(1u<<23)|(2u<<12)|(64-input)) ||
            !set(context+0x20,uint32_t(root)) || !set(context+0x24,uint32_t(root>>32)) ||
            !set(context+0x28,0) || !set(context+0x2c,0) || !set(context+0x34,0) ||
            !set(context+0x38,0x44) || !set(context+0x3c,0)) return false;
        // New stage-1 VMID/ASID are zero. CBn_TLBIALL removes every stale
        // VMID-0 entry, including globals from an older context format; old
        // nonzero VMIDs cannot match the newly installed context. This is a
        // context-register operation, not a whole-SMMU/secure global flush.
        phase="tlb-retire";
        if (SMMU::synchronizeContext(io,context,0,100000,true)!=QcomPower::OK) return false;
        phase="context-enable";
        if (!set(context,0x1167) || !set(0xc00+4*target,0) || !set(0x800+4*target,1u<<31)) return false;
        phase="tlb-publish";
        if (SMMU::synchronizeContext(io,context,0,100000)!=QcomPower::OK) return false;
        phase="cp-vbif-isolation";
        if (io.isolated()!=QcomPower::OK) return false;
        phase="register-snapshot";
        if (!unchanged()) return false;
        phase="ready";
        configured=true; return true;
    }
};
} // namespace BridgeGPU
