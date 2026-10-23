// SPDX-License-Identifier: GPL-2.0-only
// A540 producer. EFI owns devices until ExitBootServices completes; the finalizer
// stops CP/AXI, optionally initializes firmware, and publishes native contracts.
#include <Uefi.h>
#include <Protocol/Cpu.h>
extern "C" {
#include "bridge.h"
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
}
#include "../../drivers/gpu/QcomGPU/AdrenoSubmit.h"
#include "../../drivers/gpu/QcomGPU/AdrenoMMU.h"
#include "../../drivers/gpu/QcomGPU/AdrenoInterconnect.h"
#include "../../drivers/clock/QcomGCC/GCCController.h"
#include "../../drivers/clock/QcomGPUCC/ClockTables.h"
#include "../../drivers/bus/QcomSMMU/ContextTLB.h"
#include "../../drivers/power/QcomRPM/RPMClockHandoff.h"
#include "../../drivers/power/QcomRPM/RPMInterconnect.h"
#include "../../drivers/power/QcomRPM/RPMRegulators.h"
#ifdef BRIDGE_GPU_ZAP
#include "../../drivers/gpu/QcomGPU/AdrenoFirmware.h"
#include "../../drivers/security/QcomSCM/SMCCC.h"
#include "gpu_submission.h"
extern "C" const unsigned char gpu_zap_blob[], gpu_zap_end[];
#endif
#ifdef BRIDGE_GPU_FIRMWARE
#ifndef BRIDGE_GPU_ZAP
#error CP/GPMU initialization requires authenticated ZAP support
#endif
#include "gpu_firmware.h"
extern "C" const unsigned char gpu_firmware_blob[], gpu_firmware_end[];
#endif
#ifdef BRIDGE_GPU_SMMU_INIT
#ifndef BRIDGE_GPU_FIRMWARE
#error SMMU initialization requires bridge-owned CP/GPMU firmware
#endif
#include "gpu_smmu.h"
#endif
#ifdef BRIDGE_GPU_COLD_START
#ifndef BRIDGE_GPU_SMMU_INIT
#error Cold GPU power startup requires SMMU initialization and owned firmware
#endif
#include "gpu_power.h"
#include "../../drivers/bus/QcomSPMI/SPMIRegulators.h"
#include <ClockControllerProtocol.h>
#include "../../drivers/power/QcomRPM/RPMResourceHandoff.h"
#endif

inline void *operator new(size_t, void *p) noexcept { return p; }
namespace {
constexpr uint64_t GPUBase = 0x05000000, SMMUBase = 0x05040000;
#ifdef BRIDGE_GPU_ZAP
// iOS18's exported PAGE_MAX_SIZE. Boot-readable RAM must never share a CPU
// mapping granule with PAS-protected metadata/image pages (SMMU pages are 4KiB).
constexpr uint64_t KernelPageBytes = 0x4000;
#endif
uint32_t rd(uint64_t address) {
    auto value = *reinterpret_cast<volatile uint32_t *>(address);
    // Some firmware-mediated global SMMU reads leave upper X-register bits
    // set despite using LDR W. Normalize explicitly before a caller widens
    // a register value into an address or diagnostic; a C cast is optimized
    // out because the architecture normally guarantees this zero extension.
    __asm__ volatile("uxtw %x0,%w0\n\tdmb oshld" : "+r"(value) :: "memory"); return value;
}
void wr(uint64_t address, uint32_t value) {
    __asm__ volatile("dmb oshst" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(address) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
uint64_t pair(uint64_t address) { return uint64_t(rd(address)) | (uint64_t(rd(address + 4)) << 32); }
void wait(unsigned us) {
    uint64_t first, now, hz;
    __asm__ volatile("mrs %0,cntpct_el0" : "=r"(first));
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(hz));
    do { __asm__ volatile("mrs %0,cntpct_el0" : "=r"(now)); } while (now - first < (hz * us + 999999) / 1000000);
}
bool overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn) { return a < b + bn && b < a + an; }
bool prepareMMIO(EFI_BOOT_SERVICES *bs) {
    EFI_GUID guid=EFI_CPU_ARCH_PROTOCOL_GUID;EFI_CPU_ARCH_PROTOCOL *cpu=nullptr;
    uint64_t el;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    if(el!=4 || EFI_ERROR(bs->LocateProtocol(&guid,nullptr,reinterpret_cast<void **>(&cpu))) ||
        !cpu || !cpu->SetMemoryAttributes)return false;
    // Firmware maps GPUCC/GPMU only. Map the exact native resource windows
    // while CPU protocol allocation and translation-table services still run.
    // This adds CPU Device mappings; it neither powers blocks nor grants DMA.
    const uint64_t regions[][2]={{GPUBase,0x40000},{SMMUBase,0x10000},{0x01000000,0x80000}};
    for(const auto &region:regions) {
        const auto status=cpu->SetMemoryAttributes(cpu,region[0],region[1],EFI_MEMORY_UC|EFI_MEMORY_XP);
        bridge_puts("[GPU MMIO] address=");bridge_hex(region[0]);bridge_puts(" bytes=");bridge_hex(region[1]);
        bridge_puts(" mapping-status=");bridge_hex(status);bridge_puts("\n");
        if(EFI_ERROR(status))return false;
        for(uint64_t address=region[0];address<region[0]+region[1];address+=4096) {
            uint64_t read,write;
            __asm__ volatile("at s1e1r,%1\n\tisb\n\tmrs %0,par_el1":"=r"(read):"r"(address):"memory");
            __asm__ volatile("at s1e1w,%1\n\tisb\n\tmrs %0,par_el1":"=r"(write):"r"(address):"memory");
            if((read&1) || (write&1) || (read&0x0000fffffffff000ULL)!=address ||
                (write&0x0000fffffffff000ULL)!=address)return false;
        }
    }
    return true;
}
void *property(void *tree, const char *node, const char *key, unsigned size) {
    uint32_t bytes = 0; auto *p = dt_property(tree, node, key, &bytes); return bytes == size ? p : nullptr;
}
struct State {
    struct Page { uint64_t pa, va; unsigned level; uint64_t entries[512]; } pages[256]{};
    unsigned pageCount = 0;
    unsigned char memoryMap[65536]{};
    UINTN mapBytes = sizeof(memoryMap), descriptorBytes = 0;
    Adreno::PlatformContract cp{};
    Adreno::SMMUContract smmu{};
    Adreno::NoCContract noc{};
    GCC::Controller gcc;
    QcomPower::ClockHardware gpucc;
    QcomPower::RPMRegulators regulators;
    QcomPower::RPMClockHandoff clocks;
    QcomPower::RPMInterconnect bus;
    uint32_t rpmBootLevels[2]{};
    uint32_t usbCoreHz=0;
    uint64_t gccToken = 0, gpuccToken = 0;
    uint32_t context = 0;
    struct Register { uint32_t offset, value; } saved[280]{};
    unsigned savedCount = 0;
    bool prepared = false;
    SMMU::ContextTLB<State> tlb{*this};
#ifdef BRIDGE_GPU_COLD_START
    QcomPower::RPMTransport bootRPM;
    QcomPower::SPMIRegulators gfxRegulator;
    QcomPower::GPURegulatorHandoff gfxHandoff{};
    uint64_t gfxToken=0;
    BridgeGPU::PowerStartup coldPower{gcc,gpucc,regulators,bootRPM};
    bool powerReady=false,channelYielded=false;
    struct BootNoC {
        State *owner;
        uint32_t read(uint32_t r) {return rd(0x01000000+r);}
        void write(uint32_t r,uint32_t v) {wr(0x01000000+r,v);}
        QcomPower::Status isolated() {return owner->isolated();}
        QcomPower::Status limit(uint32_t id,bool clock,uint64_t &v) {return owner->bus.limit(id,clock,v);}
        QcomPower::Status vote(uint32_t id,unsigned set,bool clock,uint64_t v) {
            return owner->bus.vote(id,set,12,clock,v,[&](uint32_t type,uint32_t resource,unsigned state,const uint8_t *p,uint32_t n) {
                return owner->bootRPM.write(type,resource,state,p,n);
            });
        }
        QcomPower::Status get(uint32_t id,bool clock,uint64_t &v) {
            const auto s=owner->bootRPM.check();return s==QcomPower::OK?owner->bus.get(id,0,clock,v):s;
        }
    } bootNoCIO{this};
    Adreno::GPUInterconnect<BootNoC> bootNoC{bootNoCIO};
    bool startPower(void *tree);
#endif
#ifdef BRIDGE_GPU_SMMU_INIT
    BridgeGPU::SMMUStartup<State> coldSMMU{*this};
    bool smmuReady = false;
#endif
#ifdef BRIDGE_GPU_ZAP
    struct Allocation {
        uint64_t raw = 0, bytes = 0, address = 0;
        bool borrowed = false;
        uint64_t reservationBytes = 0;
    } allocations[9];
    unsigned allocated = 0, poolUsed = 0;
    uint64_t bootVA = 0;
    Adreno::ZapArchive archive;
    Adreno::MDTImage image;
    struct Monitor {
        unsigned calls=0;
        void smc(QcomSecure::Registers &r) {
            const auto function=r.x[0];QcomSecure::smc(r);
            if(calls++<16) {
                bridge_puts("[GPU SCM] function=");bridge_hex(function);
                bridge_puts(" status=");bridge_hex(r.x[0]);bridge_puts(" value=");bridge_hex(r.x[1]);bridge_puts("\n");
            }
        }
        void sleep(unsigned ms) { wait(ms * 1000); }
    } monitor;
    QcomSecure::Transport<Monitor> transport{monitor};
    QcomSecure::PAS<QcomSecure::Transport<Monitor>> pas{transport};
    struct BootMapIO {
        State *owner;
        QcomPower::Status isolated();
        QcomPower::Status borrow(uint64_t, Adreno::MMUTablePage &);
        QcomPower::Status allocate(Adreno::MMUTablePage &);
        void release(Adreno::MMUTablePage &p, bool) { p = {}; } // pool remains boot-reserved
        void barrier() { __asm__ volatile("dsb sy" ::: "memory"); }
        QcomPower::Status sync();
    } bootIO{this};
    Adreno::RetainedPageTables<BootMapIO> bootTables{bootIO};
    struct Commands {
        uint32_t read(uint32_t offset) { return rd(GPUBase + offset); }
        void write(uint32_t offset, uint32_t value) {
            wr(GPUBase+offset,value);
#ifdef BRIDGE_GPU_PROBE
            if(offset==Adreno::RBControl || (offset==Adreno::MEControl && !value)) {
                bridge_puts("[GPU ring] register=");bridge_hex(offset);bridge_puts(" read=");
                bridge_hex(rd(GPUBase+Adreno::RBRead));bridge_puts(" write=");
                bridge_hex(rd(GPUBase+Adreno::RBWrite));bridge_puts("\n");
            }
#endif
        }
        void publish() { __asm__ volatile("dsb sy" ::: "memory"); }
        void acquire() { __asm__ volatile("dmb oshld" ::: "memory"); }
        void delay(unsigned us) { wait(us); }
    } commands;
    BridgeGPU::SubmissionIO<Commands> submissionIO{commands};
    Adreno::Engine<BridgeGPU::SubmissionIO<Commands>> bootEngine{submissionIO};
#ifdef BRIDGE_GPU_FIRMWARE
    BridgeGPU::Firmware firmware;
    BridgeGPU::FirmwarePower power{};
    uint64_t firmwareVA[3]{};
    BridgeGPU::Startup<Commands> startup{commands};
#endif
    bool stageZap(EFI_BOOT_SERVICES *, void *);
    bool loadZap();
    uint64_t physicalConflict(uint64_t address, uint64_t bytes) const;
#endif
    uint32_t read(uint32_t offset) { return rd(SMMUBase + offset); }
    void write(uint32_t offset, uint32_t value) { wr(SMMUBase + offset, value); }
    void barrier() { __asm__ volatile("dsb sy" ::: "memory"); }
    void delay(unsigned us) { wait(us); }
    QcomPower::Status isolated() {
#ifdef BRIDGE_GPU_COLD_START
        if(gfxRegulator.validateGPUState(gfxToken)!=QcomPower::OK)return QcomPower::Stale;
#endif
        if (gcc.validateGPUState(gccToken) != GCC::Success || gpucc.validateGPUState(gpuccToken) != QcomPower::OK)
            return QcomPower::Stale;
        return (rd(GPUBase+Adreno::MEControl)&Adreno::Halt)==Adreno::Halt &&
            (rd(GPUBase+Adreno::VBIFHalted)&15)==15 ? QcomPower::OK : QcomPower::Busy;
    }
    bool ram(uint64_t address, uint64_t length) const {
        if (!address || address >= (1ULL << 40) || !length || length > (1ULL << 40) - address) return false;
        for (UINTN offset = 0; offset < mapBytes; offset += descriptorBytes) {
            auto *d = reinterpret_cast<const EFI_MEMORY_DESCRIPTOR *>(memoryMap + offset);
            const bool ordinary = d->Type == EfiLoaderCode || d->Type == EfiLoaderData ||
                d->Type == EfiBootServicesCode || d->Type == EfiBootServicesData || d->Type == EfiConventionalMemory;
            if (!ordinary || (d->Attribute & (EFI_MEMORY_RP | EFI_MEMORY_RUNTIME)) || d->NumberOfPages > (1ULL << 28)) continue;
            const auto bytes = d->NumberOfPages << 12;
            if (address >= d->PhysicalStart && address - d->PhysicalStart < bytes && length <= bytes - (address - d->PhysicalStart)) return true;
        }
        return false;
    }
    bool vacant() const {
        for (unsigned i = 0; i < pageCount; ++i) {
            const auto &page = pages[i]; const uint64_t span = 1ULL << (39 - 9 * page.level);
            for (unsigned j = 0; j < 512; ++j) {
                const auto pte = page.entries[j];
                if ((pte & 1) && (page.level == 3 || (pte & 3) == 1) &&
                    overlap(page.va + uint64_t(j) * span, span, smmu.first, smmu.end - smmu.first)) return false;
            }
        }
        return true;
    }
    bool translate(uint64_t va, uint64_t &pa) const {
        if (va >= (1ULL << smmu.inputBits)) return false;
        uint64_t table = smmu.ttbr & 0x0000fffffffff000ULL;
        for (unsigned level = smmu.inputBits > 39 ? 0 : 1; level < 4; ++level) {
            const Page *page = nullptr;
            for (unsigned i = 0; i < pageCount; ++i) if (pages[i].pa == table && pages[i].level == level) { page = &pages[i]; break; }
            if (!page) return false;
            const unsigned shift = 39 - 9 * level;
            const auto pte = page->entries[(va >> shift) & 511];
            if (!pte) return false;
            table = pte & 0x0000fffffffff000ULL;
            if (level == 3 || (pte & 3) == 1) { pa = table | (va & ((1ULL << shift) - 1)); return true; }
        }
        return false;
    }
    bool save(uint32_t offset) {
        if (savedCount == 280) return false;
        auto value = read(offset); if (value == UINT32_MAX) return false;
        saved[savedCount++] = {offset, value}; return true;
    }
    bool unchanged() {
#ifdef BRIDGE_GPU_COLD_START
        if(!powerReady)return coldPower.unchanged();
        if(!channelYielded && (bootRPM.check()!=QcomPower::OK || bootNoC.check()!=QcomPower::OK))return false;
#endif
        if (gcc.validateGPUState(gccToken) != GCC::Success || gpucc.validateGPUState(gpuccToken) != QcomPower::OK) return false;
#ifdef BRIDGE_GPU_SMMU_INIT
        if (!smmuReady) return coldSMMU.unchanged();
#endif
        for (unsigned i = 0; i < savedCount; ++i) {
            const auto value=read(saved[i].offset);
            if (value != saved[i].value) {
                bridge_puts("[GPU retained] SMMU offset=");bridge_hex(saved[i].offset);
                bridge_puts(" expected=");bridge_hex(saved[i].value);
                bridge_puts(" observed=");bridge_hex(value);bridge_puts("\n");return false;
            }
        }
        for (unsigned i = 0; i < pageCount; ++i) {
            auto *p = reinterpret_cast<volatile uint64_t *>(pages[i].pa);
            for (unsigned j = 0; j < 512; ++j) if (p[j] != pages[i].entries[j]) {
                bridge_puts("[GPU retained] page=");bridge_hex(pages[i].pa);
                bridge_puts(" entry=");bridge_hex(j);bridge_puts("\n");return false;
            }
        }
        return true;
    }
    bool walk(uint64_t physical, unsigned level, uint64_t va) {
        if (pageCount == 256 || (physical & 4095) || !ram(physical, 4096)) return false;
        for (unsigned i = 0; i < pageCount; ++i) if (pages[i].pa == physical) return false; // cycles and aliases
        auto &page = pages[pageCount++]; page.pa = physical; page.level = level; page.va = va;
        auto *live = reinterpret_cast<volatile uint64_t *>(physical);
        const uint64_t span = 1ULL << (39 - 9 * level), mask = 0x0000fffffffff000ULL;
        for (unsigned i = 0; i < 512; ++i) {
            const uint64_t entry = live[i]; page.entries[i] = entry;
            if (!entry) continue;
            const auto address = va + uint64_t(i) * span;
            if (address >= (1ULL << smmu.inputBits) || (entry & 3) == 2 || !(entry & 1)) return false;
            if (level < 3 && (entry & 3) == 3) {
                if (!walk(entry & mask, level + 1, address)) return false;
            } else {
                if (!level || (level == 3 && (entry & 3) != 3) || ((entry & mask) & (span - 1))) return false;
                const uint64_t pa = entry & mask;
                if (pa >= (1ULL << smmu.outputBits) || span > (1ULL << smmu.outputBits) - pa) return false;
            }
        }
        return true;
    }
    bool captureSMMU() {
        const uint32_t id0 = read(0x20), id1 = read(0x24), id2 = read(0x28);
        const unsigned pageShift = id1 & (1u << 31) ? 16 : 12;
        const uint64_t globals = 1ULL << (((id1 >> 28) & 7) + 1);
        if (2 * globals * (1ULL << pageShift) != 0x10000) return false;
        context = unsigned(globals << pageShift);
        const uint32_t tcr = read(context + 0x30), tcr2 = read(context + 0x10);
        const unsigned bits[] = {32, 36, 40};
        smmu.version = 1; smmu.flags = Adreno::SMMUContract::Required; smmu.bank = 0;
        smmu.inputBits = 64 - (tcr & 63); smmu.outputBits = (tcr2 & 7) < 3 ? bits[tcr2 & 7] : 0;
        smmu.ttbr = pair(SMMUBase + context + 0x20); smmu.asid = unsigned(smmu.ttbr >> 48);
        if (tlb.configure(0x10000, 0, smmu.asid) != QcomPower::OK || tlb.bankOffset() != context ||
            (read(0) & 9) || (read(context) & ~0x1167u) || (read(context) & 0x1067) != 0x1067 ||
            read(0x1800) != 1 || ((read(0x1000) >> 16) & 3) != 1 || pair(SMMUBase + context + 0x28) ||
            (read(context + 0x38) & 255) != 0x44 || SMMU::contextFault(read(context + 0x58)) ||
            tcr != ((1u << 23) | (2u << 12) | (64 - smmu.inputBits)) ||
            tcr2 != ((7u << 15) | (1u << 4) | (smmu.outputBits == 40 ? 2u : smmu.outputBits == 36 ? 1u : 0u))) return false;
        const unsigned widths[] = {32,36,40,42,44,48,52};
        if (!(id0 & (1u << 27)) || (id2 & 15) >= 7 || ((id2 >> 4) & 15) >= 7 || ((id2 >> 8) & 15) >= 7 ||
            smmu.outputBits > widths[id2 & 15] || smmu.outputBits > widths[(id2 >> 4) & 15] ||
            smmu.inputBits > widths[(id2 >> 8) & 15]) return false;
        const unsigned groups = id0 & 255, sidBits = (id0 >> 9) & 15;
        if (!groups || groups > 128) return false;
        for (unsigned slot = 0; slot < groups; ++slot) {
            const auto smr = read(0x800 + 4 * slot), route = read(0xc00 + 4 * slot);
            if (!save(0x800 + 4 * slot) || !save(0xc00 + 4 * slot)) return false;
            if (!(smr & (1u << 31)) || (route & 0x300ff)) continue;
            if (smmu.streamCount == 8 || (smr & 0x7fff0000) || (smr & 65535) >= (1u << sidBits)) return false;
            smmu.streams[smmu.streamCount++] = {slot, smr & 65535};
        }
        for (unsigned slot = 0; slot < groups; ++slot) {
            const auto smr = read(0x800 + 4 * slot), route = read(0xc00 + 4 * slot);
            if (!(smr & (1u << 31)) || !(route & 0x300ff)) continue;
            for (unsigned i = 0; i < smmu.streamCount; ++i)
                if (!((smr ^ smmu.streams[i].id) & 65535 & ~((smr >> 16) & 32767))) return false;
        }
        const unsigned offsets[] = {0,0x1000,0x1800};
        for (auto offset : offsets) if (!save(offset)) return false;
        const unsigned local[] = {0,0x10,0x20,0x24,0x28,0x2c,0x30,0x38,0x3c,0x58};
        for (auto offset : local) if (!save(context + offset)) return false;
        return smmu.valid() && walk(smmu.ttbr & 0x0000fffffffff000ULL, smmu.inputBits > 39 ? 0 : 1, 0) &&
            vacant();
    }
    bool rpm(void *tree) {
        auto *domains = static_cast<uint32_t *>(property(tree, "power-controller@778000", "qcom,rpm-domain-handoff", 64));
        auto *clock = static_cast<uint32_t *>(property(tree, "power-controller@778000", "qcom,rpm-clock-handoff", 16 * QcomPower::RPMHandoffClockCount));
        auto *limits=static_cast<uint32_t *>(property(tree,"power-controller@778000","qcom,rpm-clock-limit-handoff",16*QcomPower::RPMHandoffClockLimitCount));
        auto *interconnect = static_cast<unsigned char *>(property(tree, "power-controller@778000", "qcom,rpm-interconnect-handoff", 256));
        if (!domains || !clock || !limits || !interconnect) return false;
        uint32_t usbBytes=0;
        const auto *usbClock=dt_property(tree,"power-controller@778000","qcom,rpm-usb-core-clock-hz",&usbBytes);
        if(usbClock) {
            if(usbBytes!=sizeof(usbCoreHz))return false;
            memcpy(&usbCoreHz,usbClock,sizeof(usbCoreHz));
            if(!QcomPower::usbCoreCXLevel(usbCoreHz))return false;
        }
        bool cx = false, mx = false;
        for (unsigned i = 0; i < 4; ++i) {
            auto *p = domains + 4 * i;
            if (p[3] || !regulators.configureDomain(p[0], p[1], p[2])) return false;
            if(p[0]>=0x1000 && p[0]<0x1002)rpmBootLevels[p[0]-0x1000]=p[1];
            cx |= p[0] == 0x1000 && p[1]; mx |= p[0] == 0x1001 && p[1];
        }
        for (unsigned i = 0; i < QcomPower::RPMHandoffClockCount; ++i) { auto *p = clock + 4 * i; if (p[3] || !clocks.configure(p[0], p[1], p[2])) return false; }
        for(unsigned i=0;i<QcomPower::RPMHandoffClockLimitCount;++i) {
            auto *p=limits+4*i;if(p[3] || !clocks.configureLimit(p[0],p[1],p[2]))return false;
        }
        if(!clocks.configured())return false;
        for (unsigned i = 0; i < 8; ++i) {
            uint32_t id, reserved; uint64_t values[3];
            memcpy(&id, interconnect + 32 * i, 4); memcpy(&reserved, interconnect + 32 * i + 4, 4);
            memcpy(values, interconnect + 32 * i + 8, 24);
            if (reserved || !bus.configure(id, values[0], values[1], values[2])) return false;
        }
        for (unsigned i = 0; i < 3; ++i) for (unsigned set = 0; set < 2; ++set) {
            const auto &r = QcomPower::rpmBusResources[i]; uint32_t hz = 0,cap=0; uint64_t floor = 0,maximum=0;
            if (clocks.floor(r.type, r.resource, 0x007a484b, set, hz) != QcomPower::OK ||
                clocks.limit(r.type,r.resource,set,cap)!=QcomPower::OK ||
                bus.floor(r.id, set, floor) != QcomPower::OK || floor != uint64_t(hz) * 1000 ||
                bus.limit(r.id,true,maximum)!=QcomPower::OK || (cap && maximum>uint64_t(cap)*1000))return false;
        }
        uint64_t bimc = 0, ebi = 0, maximum = 0, maxClock = 0;
        uint64_t peakClock = ((noc.peak + 7) / 8 + 999) / 1000 * 1000;
        if (peakClock < 19200000) peakClock = 19200000;
        return cx && mx && bus.floor(0x2002, 0, bimc) == QcomPower::OK && bimc &&
            bus.floor(0x2104, 0, ebi) == QcomPower::OK && ebi &&
            bus.limit(0x2104, false, maximum) == QcomPower::OK && noc.average <= maximum &&
            bus.limit(0x2002, true, maxClock) == QcomPower::OK && peakClock <= maxClock;
    }
};
#ifdef BRIDGE_GPU_ZAP
uint64_t State::physicalConflict(uint64_t address, uint64_t length) const {
    uint64_t first = UINT64_MAX;
    for (unsigned i = 0; i < pageCount; ++i) {
        const auto &p = pages[i];
        if (overlap(address, length, p.pa, 4096) && p.pa < first) first = p.pa;
        const uint64_t span = 1ULL << (39 - 9 * p.level);
        for (auto entry : p.entries) if ((entry & 1) && (p.level == 3 || (entry & 3) == 1)) {
            const auto pa = entry & 0x0000fffffffff000ULL;
            if (overlap(address, length, pa, span) && pa < first) first = pa;
        }
    }
    return first;
}
bool State::stageZap(EFI_BOOT_SERVICES *bs, void *tree) {
    if (!archive.open(gpu_zap_blob, size_t(gpu_zap_end - gpu_zap_blob)) || !image.parse(archive)) return false;
#ifdef BRIDGE_GPU_FIRMWARE
    auto *point = property(tree,"gpu@5000000","qcom,gpu-firmware-power",sizeof(power));
    if (!point || !firmware.open(gpu_firmware_blob,size_t(gpu_firmware_end-gpu_firmware_blob))) return false;
    memcpy(&power,point,sizeof(power));
#ifndef BRIDGE_GPU_COLD_START
    const uint64_t hz = gpucc.rate(7), error = hz > power.frequency ? hz-power.frequency : power.frequency-hz;
    if (!power.valid() || !hz || error > power.frequency/10000+1) return false;
#else
    if(!power.valid())return false;
#endif
#endif
    EFI_GUID guid = EFI_CPU_ARCH_PROTOCOL_GUID;
    EFI_CPU_ARCH_PROTOCOL *cpu = nullptr;
    if (EFI_ERROR(bs->LocateProtocol(&guid, nullptr, reinterpret_cast<void **>(&cpu))) || !cpu ||
        !cpu->SetMemoryAttributes || !cpu->FlushDataCache) return false;
    // All pages stay outside the advertised XNU arena. Low host PA permits
    // either probed SMCCC convention; metadata/image are never shared pages.
    auto allocate = [&](unsigned index, uint64_t bytes, uint64_t alignment, uint64_t fixed = 0) {
        if (index>=2) {
            bytes=(bytes+KernelPageBytes-1)&~(KernelPageBytes-1);
            if (alignment<KernelPageBytes) alignment=KernelPageBytes;
        }
        const uint64_t usable = (bytes+4095)&~4095ULL;
        const uint64_t total = usable+(fixed?0:alignment-4096);
        uint64_t limit = UINT32_MAX;
        for (unsigned attempt = 0; attempt < 256; ++attempt) {
            EFI_PHYSICAL_ADDRESS raw = fixed ? fixed : limit;
            if (EFI_ERROR(bs->AllocatePages(fixed ? AllocateAddress : AllocateMaxAddress, EfiLoaderData, total / 4096, &raw))) return false;
            const auto conflict = physicalConflict(raw, total);
            if (conflict != UINT64_MAX) {
                bs->FreePages(raw, total / 4096);
                if (fixed || !conflict) return false;
                limit = conflict - 1; continue;
            }
            const auto address = fixed ? fixed : (raw+alignment-1)&~(alignment-1);
            if (address<raw || address-raw>total || usable>total-(address-raw)) {
                bs->FreePages(raw,total/4096); return false;
            }
            if (EFI_ERROR(cpu->FlushDataCache(cpu, raw, total, EfiCpuFlushTypeWriteBackInvalidate))) {
                bs->FreePages(raw, total / 4096); return false;
            }
            if (EFI_ERROR(cpu->SetMemoryAttributes(cpu, raw, total, EFI_MEMORY_UC | EFI_MEMORY_XP))) {
                // Restore cacheability before returning pages to firmware.
                if (!EFI_ERROR(cpu->SetMemoryAttributes(cpu, raw, total, EFI_MEMORY_WB))) bs->FreePages(raw, total / 4096);
                return false;
            }
            // Track the full EFI reservation separately from usable bytes
            // after alignment; copying/mapping must not include the prefix.
            allocations[index] = {raw,usable,address,false,total};
            if (index>=2) memset(reinterpret_cast<void *>(address),0,size_t(usable));
            ++allocated; return true;
        }
        return false;
    };
    auto imageRegion = [&] {
        uint32_t reservedBytes = 0;
        auto *reserved = dt_property(tree,"gpu@5000000","qcom,gpu-zap-memory",&reservedBytes);
        if (!reserved) return allocate(1,image.imageBytes(),image.imageAlignment(),image.relocatable() ? 0 : image.physicalBase());
        if (reservedBytes != 16) return false;
        uint64_t region[2]; memcpy(region,reserved,sizeof(region));
        if (region[0] < 0x80000000 || !region[1] || (region[0] | region[1]) & 4095 ||
            region[1] > Adreno::MDTImage::MaximumImage || region[0] >= (1ULL<<32) || region[1] > (1ULL<<32)-region[0] ||
            physicalConflict(region[0],region[1]) != UINT64_MAX) return false;
        const auto address = image.relocatable() ? (region[0]+image.imageAlignment()-1)&~uint64_t(image.imageAlignment()-1) : image.physicalBase();
        if (address < region[0] || address-region[0] > region[1] || image.imageBytes() > region[1]-(address-region[0])) return false;
        bool found = false;
        for (UINTN offset=0; offset<mapBytes; offset+=descriptorBytes) {
            auto *d=reinterpret_cast<const EFI_MEMORY_DESCRIPTOR *>(memoryMap+offset);
            if (d->Type!=EfiReservedMemoryType || !(d->Attribute&EFI_MEMORY_UC) ||
                (d->Attribute&(EFI_MEMORY_RP|EFI_MEMORY_WP|EFI_MEMORY_RO|EFI_MEMORY_RUNTIME)) || d->NumberOfPages>(1ULL<<28)) continue;
            const auto bytes=d->NumberOfPages<<12;
            if (region[0]>=d->PhysicalStart && region[0]-d->PhysicalStart<bytes && region[1]<=bytes-(region[0]-d->PhysicalStart)) {found=true;break;}
        }
        // A platform-declared reserved carveout is not free EFI RAM. Require
        // an uncached CPU mapping and never flush/read it before PAS setup.
        if (!found || EFI_ERROR(cpu->SetMemoryAttributes(cpu,address,image.imageBytes(),EFI_MEMORY_UC|EFI_MEMORY_XP))) return false;
        allocations[1]={region[0],region[1]-(address-region[0]),address,true,region[1]};++allocated;return true;
    };
    bool complete = allocate(0, image.metadataBytes(), 4096) &&
        imageRegion() &&
        allocate(2, Adreno::RingDwords * 4, 4096) &&
        allocate(3, BridgeGPU::SubmissionProbe::Bytes, KernelPageBytes) && allocate(4, 8 * 4096, 4096);
#ifdef BRIDGE_GPU_FIRMWARE
    complete = complete && allocate(5,firmware.cpBytes(0),4096) && allocate(6,firmware.cpBytes(1),4096) &&
        allocate(7,uint64_t(firmware.gpmuWords())*4,4096);
#endif
#ifdef BRIDGE_GPU_SMMU_INIT
    complete = complete && allocate(8,4096,4096);
#endif
    auto unwind = [&] {
        for (unsigned i = 0; i < allocated; ++i) {
            const auto &a = allocations[i];
            if (a.borrowed) continue;
            if (!EFI_ERROR(cpu->SetMemoryAttributes(cpu, a.raw, a.reservationBytes, EFI_MEMORY_WB))) bs->FreePages(a.raw, a.reservationBytes/4096);
        }
        allocated = 0;
    };
    if (!complete) { unwind(); return false; }
    if (!image.metadataInto(reinterpret_cast<void *>(allocations[0].address), uint32_t(allocations[0].bytes))) { unwind(); return false; }
#ifdef BRIDGE_GPU_FIRMWARE
    if (!firmware.copyCP(0,reinterpret_cast<void *>(allocations[5].address),uint32_t(allocations[5].bytes)) ||
        !firmware.copyCP(1,reinterpret_cast<void *>(allocations[6].address),uint32_t(allocations[6].bytes)) ||
        !firmware.gpmuCommands(reinterpret_cast<uint32_t *>(allocations[7].address),uint32_t(allocations[7].bytes/4))) {
        unwind(); return false;
    }
#endif
    // Select an independent vacant 8MiB bootstrap aperture. The kernel's
    // allocator window remains empty; its mapper preserves these boot leaves.
    uint64_t candidate = 0x1000000;
    const uint64_t maximum = smmu.inputBits < 32 ? 1ULL << smmu.inputBits : 1ULL << 32;
    while (candidate < maximum && 0x800000 <= maximum - candidate) {
        uint64_t next = candidate;
        if (overlap(candidate, 0x800000, smmu.first, smmu.end - smmu.first)) next = smmu.end;
        for (unsigned i = 0; i < pageCount; ++i) {
            const auto &p = pages[i]; const uint64_t span = 1ULL << (39 - 9 * p.level);
            for (unsigned j = 0; j < 512; ++j) if ((p.entries[j] & 1) && (p.level == 3 || (p.entries[j] & 3) == 1)) {
                const auto va = p.va + uint64_t(j) * span;
                if (overlap(candidate, 0x800000, va, span) && va + span > next) next = va + span;
            }
        }
        if (next == candidate) { bootVA = candidate; return true; }
        candidate = (next + 0x7fffff) & ~0x7fffffULL;
    }
    unwind(); return false;
}
QcomPower::Status State::BootMapIO::isolated() {
    return owner->isolated();
}
QcomPower::Status State::BootMapIO::borrow(uint64_t pa, Adreno::MMUTablePage &page) {
    if (!owner->ram(pa, 4096)) return QcomPower::Failed;
    page = {owner,reinterpret_cast<volatile uint64_t *>(pa),pa}; return QcomPower::OK;
}
QcomPower::Status State::BootMapIO::allocate(Adreno::MMUTablePage &page) {
    if (owner->poolUsed == 8) return QcomPower::NotReady;
    const auto pa = owner->allocations[4].address + uint64_t(owner->poolUsed++) * 4096;
    page = {owner,reinterpret_cast<volatile uint64_t *>(pa),pa}; return QcomPower::OK;
}
QcomPower::Status State::BootMapIO::sync() {
    for (unsigned i = 0; i < owner->pageCount; ++i) bridge_clean(owner->pages[i].pa, owner->pages[i].pa + 4096);
    barrier(); return owner->tlb.sync(); // newly allocated table pages are uncached
}
bool State::loadZap() {
    if (!bootVA || transport.probe() != QcomSecure::Status::OK) return false;
    const unsigned pasCommands[] = {1,5,6,7};
    for (const auto command : pasCommands) {
        const auto r = transport.available(2, command);
        if (r.status != QcomSecure::Status::OK || r.value != 1) return false;
    }
    if (image.relocatable()) {
        const auto r = transport.available(2, 2);
        if (r.status != QcomSecure::Status::OK || r.value != 1) return false;
    }
    const auto support = transport.supported();
    bridge_puts("[GPU PAS] support-query status=");bridge_hex(uint32_t(support.status));
    bridge_puts(" value=");bridge_hex(support.value);bridge_puts("\n");
    // The Adreno MDT loader uses the advertised INIT/MEM_SETUP/AUTH APIs
    // directly; IS_SUPPORTED is not an authorization result. Some GPU PAS
    // firmware reports zero here. Every actual signed-image operation below
    // must still return success, before a non-secure transition is attempted.
    if (support.status != QcomSecure::Status::OK) return false;
    bridge_puts("[GPU firmware] preparing private bootstrap mappings\n");
    auto bootstrap = smmu; bootstrap.first = bootVA; bootstrap.end = bootVA + 0x800000;
    if (bootTables.adopt(bootstrap) != QcomPower::OK) return false;
    struct Source {
        uint64_t physical;
        QcomPower::Status page(unsigned n, uint64_t &pa) { pa = physical + uint64_t(n) * 4096; return QcomPower::OK; }
    } ring{allocations[2].address}, fence{allocations[3].address};
    using Probe=BridgeGPU::SubmissionProbe;
    Source ib{fence.physical+Probe::IBOffset},source{fence.physical+Probe::SourceOffset},destination{fence.physical+Probe::DestinationOffset};
    const uint64_t fenceVA=bootVA+0x8000;
    if (bootTables.map(bootVA, 8, ring, 3) != QcomPower::OK ||
        bootTables.map(fenceVA,1,fence,3)!=QcomPower::OK ||
        bootTables.map(fenceVA+Probe::IBOffset,1,ib,1)!=QcomPower::OK ||
        bootTables.map(fenceVA+Probe::SourceOffset,1,source,3)!=QcomPower::OK ||
        bootTables.map(fenceVA+Probe::DestinationOffset,1,destination,3)!=QcomPower::OK)return false;
#ifdef BRIDGE_GPU_FIRMWARE
    uint64_t next = bootVA+0x10000;
    for (unsigned i = 0; i < 3; ++i) {
        auto &a = allocations[5+i]; Source source{a.address}; firmwareVA[i] = next;
        if (a.bytes > bootVA+0x800000-next || bootTables.map(next,unsigned(a.bytes/4096),source,1) != QcomPower::OK) return false;
        next += a.bytes;
    }
#endif
    // Refresh only the page snapshot after our own additions. Context/stream
    // registers must retain the original identities across authentication.
    pageCount = 0;
    if (!walk(smmu.ttbr & 0x0000fffffffff000ULL, smmu.inputBits > 39 ? 0 : 1, 0) || !vacant() || !unchanged()) return false;
    bridge_puts("[GPU firmware] bootstrap mappings and retained identities verified\n");
#ifdef BRIDGE_GPU_FIRMWARE
#ifndef BRIDGE_GPU_SMMU_INIT
    // Retained tables must survive the actual GPU reset. A fresh context was
    // instead installed after reset by the SMMU-init takeover path above.
    if (!startup.reset()) return false;
#endif
    // Recheck the entire context/stream identity and page snapshot before any
    // CP DMA, for both retained and newly initialized contexts.
    if (bootIO.isolated() != QcomPower::OK || tlb.sync() != QcomPower::OK || !unchanged()) return false;
    cp.pm4 = firmwareVA[0]; cp.pfp = firmwareVA[1];
    const bool cpStarted = startup.start(cp,reinterpret_cast<volatile uint32_t *>(allocations[2].address),bootVA);
    bridge_puts(cpStarted?"[GPU firmware] CP ME_INIT complete\n":"[GPU firmware] CP ME_INIT failed\n");
    const bool initialized = cpStarted && startup.gpmu(power,firmwareVA[2],firmware.gpmuWords());
    bridge_puts(initialized?"[GPU firmware] GPMU response complete\n":"[GPU firmware] GPMU response unavailable\n");
    const bool drained = startup.stop();
    if (!initialized || !drained || !unchanged()) return false;
    bridge_puts("[GPU handoff] PM4/PFP loaded; reset/ME_INIT and GPMU firmware response complete\n");
#endif
    if (pas.init(allocations[0].address) != QcomSecure::Status::OK ||
        pas.setup(allocations[1].address, image.imageBytes(), image.relocatable()) != QcomSecure::Status::OK ||
        !image.imageInto(reinterpret_cast<void *>(allocations[1].address), image.imageBytes(), allocations[1].address)) return false;
    // PAS may access the image after it has protected it. No subsequent CPU
    // read, clear, free or cache operation touches metadata or image pages.
    __asm__ volatile("dsb sy" ::: "memory");
    const auto authenticated=pas.authenticate();
    const auto authIsolation=authenticated==QcomSecure::Status::OK ? bootIO.isolated():QcomPower::NotReady;
    const bool retained=authIsolation==QcomPower::OK && unchanged();
    bridge_puts("[GPU PAS] authenticate=");bridge_hex(uint32_t(authenticated));
    bridge_puts(" isolation=");bridge_hex(uint32_t(authIsolation));
    bridge_puts(" retained=");bridge_hex(retained);bridge_puts("\n");
    if (authenticated != QcomSecure::Status::OK || authIsolation != QcomPower::OK || !retained) return false;
    auto healthy=[&] {
        return gcc.validateGPUState(gccToken)==GCC::Success && gpucc.validateGPUState(gpuccToken)==QcomPower::OK;
    };
    auto result = bootEngine.start(cp, reinterpret_cast<volatile uint32_t *>(allocations[2].address), bootVA,
        reinterpret_cast<volatile uint32_t *>(allocations[3].address), fenceVA, {true,false});
    bridge_puts("[GPU submit] secure-transition/fence result=");bridge_hex(uint32_t(result));
    bridge_puts(" fence=");bridge_hex(*reinterpret_cast<volatile uint32_t *>(allocations[3].address));bridge_puts("\n");
    if(result==GPU::Result::Success)result=Probe::run(bootEngine,submissionIO,
        reinterpret_cast<uint32_t *>(allocations[3].address),uint32_t(allocations[3].bytes),fenceVA,healthy);
    bridge_puts("[GPU submit] DMA-probe result=");bridge_hex(uint32_t(result));bridge_puts("\n");
    if(healthy()) {
        const uint32_t offsets[]={Adreno::MEControl,Adreno::RBRead,Adreno::RBWrite,Adreno::RBControl,
            Adreno::InterruptStatus,Adreno::Status,Adreno::Status3,Adreno::TrustControl,Adreno::GPMUReady,
            Adreno::GPMUError,0x0b18*4,0x0b1a*4,0x0b1b*4,0x0b1c*4};
        for(auto offset:offsets) {
            bridge_puts("[GPU submit probe] offset=");bridge_hex(offset);bridge_puts(" value=");
            bridge_hex(commands.read(offset));bridge_puts("\n");
        }
        bridge_puts("[GPU submit probe] SMMU-FSR=");bridge_hex(read(context+0x58));
        bridge_puts(" FAR=");bridge_hex(pair(SMMUBase+context+0x60));bridge_puts("\n");
    }
    // Also halt/drain after a failed IB or readback. If clock ownership was
    // lost, avoid unsafe MMIO and retain every reservation until reset.
    const bool stopped = healthy() && bootEngine.stop();
    if (result != GPU::Result::Success || !stopped || submissionIO.faulted()) return false;
    bridge_puts("[GPU handoff] CP Fill32/Copy32/barriers, timestamp and guarded DMA readback complete\n");
    // Only non-secure bootstrap ring/fence/tables are CPU-readable here. The
    // authenticated image and metadata stay allocated until platform reset.
    return !rd(GPUBase + Adreno::TrustControl) && unchanged();
}
#endif
alignas(State) unsigned char storage[sizeof(State)];
State *state;
uint32_t gccRead(void *, uint32_t offset) { return rd(0x00100000 + offset); }
uint32_t ccRead(void *, unsigned bank, uint32_t offset) { return rd((bank ? 0x00100000 : 0x05065000) + offset); }
#ifdef BRIDGE_GPU_COLD_START
void gccWrite(void *,uint32_t offset,uint32_t value) {wr(0x00100000+offset,value);}
void ccWrite(void *,unsigned bank,uint32_t offset,uint32_t value) {wr((bank?0x00100000:0x05065000)+offset,value);}
uint32_t rpmRead(void *,unsigned bank,uint32_t offset) {return rd((bank?0x17911000:0x00778000)+offset);}
void rpmWrite(void *,unsigned bank,uint32_t offset,uint32_t value) {wr((bank?0x17911000:0x00778000)+offset,value);}
void rpmKick(void *) {wr(0x17911008,1);}
#endif
#ifndef BRIDGE_GPU_COLD_START
void unexpectedWrite(void *, uint32_t, uint32_t) { for (;;) __asm__ volatile("wfe"); }
void unexpectedCCWrite(void *, unsigned, uint32_t, uint32_t) { for (;;) __asm__ volatile("wfe"); }
#endif
void ioWait(void *, unsigned us) { wait(us); }
#ifdef BRIDGE_GPU_COLD_START
bool State::startPower(void *tree) {
    auto report = [](const char *stage, QcomPower::Status result) {
        bridge_puts("[GPU power] stage="); bridge_puts(stage);
        bridge_puts(" result="); bridge_hex(uint32_t(result)); bridge_puts("\n");
        return result == QcomPower::OK;
    };
    const auto access=QcomPower::IO{nullptr,rpmRead,rpmWrite,ioWait,rpmKick};
    auto status=QcomPower::NotReady;
    auto *cold=property(tree,"power-controller@778000","qcom,rpm-transport-cold-handoff",sizeof(QcomPower::RPMTransportCold));
#ifdef BRIDGE_RPM_COLD_HANDOFF
    if(!cold)return false; // a selected reset fence cannot fall back to legacy ownership
#endif
    if(cold) {
        QcomPower::RPMTransportCold h{};memcpy(&h,cold,sizeof(h));status=bootRPM.initializeCold(access,0x7000,h);
    } else status=bootRPM.initialize(access,0x7000);
    auto preserveUSB=[&] {
        return usbCoreHz ? QcomPower::validateUSBClockBaseline(regulators,usbCoreHz,gcc.rate(163)):QcomPower::OK;
    };
    if(!report("transport", status))return false;
    status=QcomPower::initializeRPMResources(bootRPM,regulators,clocks,bus,true,preserveUSB);
    uint32_t openAckWord = 0;
    if (bootRPM.observedOpenAck(openAckWord)) {
        bridge_puts("[GPU RPM] observed OPEN_ACK local-channel=1 trailing-word=");
        bridge_hex(openAckWord); bridge_puts("\n");
    }
    if(!report("resource-acks", status)) {
        const auto d=bootRPM.debug();
        const auto *words=reinterpret_cast<const uint32_t *>(&d);
        for (unsigned i=0;i<sizeof(d)/sizeof(*words);++i) {
            bridge_puts("[GPU RPM debug] word="); bridge_hex(i);
            bridge_puts(" value="); bridge_hex(words[i]); bridge_puts("\n");
        }
        return false;
    }
    const auto spmiRead=[](void *,unsigned bank,uint32_t offset) {
        const uint64_t bases[]={0x0800f000,0x08400000,0x09400000,0x0a400000,0x0800a000};
        const uint32_t sizes[]={0x1000,0x1000000,0x1000000,0x220000,0x3000};
        return bank<5 && !(offset&3) && offset<=sizes[bank]-4 ? rd(bases[bank]+offset):UINT32_MAX;
    };
    const auto spmiWrite=[](void *,unsigned bank,uint32_t offset,uint32_t value) {
        const uint64_t bases[]={0x0800f000,0x08400000,0x09400000,0x0a400000,0x0800a000};
        const uint32_t sizes[]={0x1000,0x1000000,0x1000000,0x220000,0x3000};
        if(bank<5 && !(offset&3) && offset<=sizes[bank]-4)wr(bases[bank]+offset,value);
    };
    uint32_t rail[5]{};
    const auto *config=property(tree,"power-controller@800f000","qcom,pm8005-regulators",sizeof(rail));
    if(!config)return false;
    memcpy(rail,config,sizeof(rail));
    if(!report("gfx-spmi",gfxRegulator.initialize({nullptr,spmiRead,spmiWrite,ioWait,nullptr},0)) ||
        !gfxRegulator.configure(rail[0],rail[1],rail[2],rail[3],rail[4]))return false;
    const uint16_t observations[]={0x1404,0x1408,0x1440,0x1445,0x1446,0x1461,0x1d04,0x1d46};
    for(auto address:observations) {
        uint8_t data[2]{};const auto observed=gfxRegulator.bus().transfer(rail[1],address,data,2,false);
        bridge_puts("[GPU GFX probe] address=");bridge_hex(address);
        bridge_puts(" result=");bridge_hex(observed);bridge_puts(" value=");
        bridge_hex(uint32_t(data[0])|(uint32_t(data[1])<<8));bridge_puts("\n");
    }
    uint32_t actual=0;
    if(!report("gfx-observe",gfxRegulator.get(rail[0],actual)))return false;
    bridge_puts("[GPU power] GFX inherited-microvolts=");bridge_hex(actual);bridge_puts("\n");
    if(!report("gfx-enable",gfxRegulator.set(rail[0],ClockController::GPUConsumer,power.microvolts)) ||
        !report("gfx-readback",gfxRegulator.get(rail[0],actual)))return false;
    gfxHandoff={1,rail[0],power.microvolts,0};
    if(actual<power.microvolts || !report("gfx-contract",gfxRegulator.configureGPU(gfxHandoff)) ||
        !report("gfx-retain",gfxRegulator.retainGPUState(gfxToken)))return false;
    bridge_puts("[GPU power] GFX verified-microvolts=");bridge_hex(actual);bridge_puts("\n");
    if(!coldPower.start()) {
        report(coldPower.stage(), QcomPower::NotReady);
        bridge_puts("[GPU power] resource=");bridge_hex(coldPower.resource());
        bridge_puts(" raw-status=");bridge_hex(coldPower.result());bridge_puts("\n");
        return false;
    }
    report("private-clock-start", QcomPower::OK);
    if(gcc.retainGPUState(gccToken)!=GCC::Success || gpucc.retainGPUState(gpuccToken)!=QcomPower::OK) {
        report("clock-ownership", QcomPower::NotReady); return false;
    }
    report("clock-ownership",QcomPower::OK);
    // RBCCU/SP are separate GPMU-controlled islands. Power them before the
    // first CP/RBBM/VBIF access, as a5xx_pm_resume does before hw_init.
    bridge_puts("[GPU power] starting RBCCU/SP islands\n");
    if(!report("core-islands",coldPower.enableBlocks(commands)?QcomPower::OK:QcomPower::Failed))return false;
#ifdef BRIDGE_GPU_PROBE
    const uint32_t coreRegisters[]={0x0000,0x0043,0x0067,0x0093,0x0094};
    for(auto index:coreRegisters) {
        bridge_puts("[GPU core probe] index=");bridge_hex(index);bridge_puts(" value=");
        bridge_hex(rd(GPUBase+index*4));bridge_puts("\n");
    }
#endif
    // Only the cold path has proven GX OFF before takeover and has not yet
    // installed any CP instruction/ring address. Reset RBBM before touching
    // CP's internal register bus, which can still be parked after GX reset.
    bridge_puts("[GPU power] cold RBBM reset\n");
    wr(GPUBase+Adreno::RBBM_SW_RESET_CMD*4,1);
    (void)rd(GPUBase+Adreno::RBBM_SW_RESET_CMD*4);
    wr(GPUBase+Adreno::RBBM_SW_RESET_CMD*4,0);wait(1);
    if(!coldPower.enableBlocks(commands))return false;
    wr(GPUBase+Adreno::RBBM_AHB_CNTL0*4,1);
    wr(GPUBase+Adreno::RBBM_INTERFACE_HANG_INT_CNTL*4,0x4000ffff);
#ifdef BRIDGE_GPU_PROBE
    const uint64_t observationAddresses[]={
        0x05066004,0x05066008,0x05066094,0x05066098,0x05066070,
        GPUBase+0xa88d*4,GPUBase+0xa88b*4,SMMUBase+0x20,SMMUBase+0x24,SMMUBase+0x28,
        SMMUBase,SMMUBase+0x8000,SMMUBase+0x8058,SMMUBase+0x1000,SMMUBase+0x1800,
        GPUBase+Adreno::VBIFHalted,GPUBase+Adreno::TrustControl};
    for(auto address:observationAddresses) {
        bridge_puts("[GPU core probe] address=");bridge_hex(address);bridge_puts(" value=");
        bridge_hex(rd(address));bridge_puts("\n");
    }
#endif
    // Collapsed GX was proven before EFI exit: no prior GPU job can be
    // replayed. Isolate the newly powered CP before resetting its state.
    bridge_puts("[GPU power] masking interrupts\n");wr(GPUBase+Adreno::InterruptMask,0);
    bridge_puts("[GPU power] reading CP halt state\n");
    const auto initialME=rd(GPUBase+Adreno::MEControl);
    bridge_puts("[GPU power] initial-ME-control=");bridge_hex(initialME);bridge_puts("\n");
#ifdef BRIDGE_GPU_PROBE
    const uint32_t cpRegisters[]={Adreno::RBBase,Adreno::RBControl,Adreno::RBRead,Adreno::RBWrite,
        Adreno::PFPBase,Adreno::PFPBase+4,Adreno::MEBase,Adreno::MEBase+4,Adreno::ContextSwitch,
        Adreno::InterruptStatus,Adreno::Status,Adreno::Status3};
    for(auto offset:cpRegisters) {
        bridge_puts("[GPU CP probe] offset=");bridge_hex(offset);bridge_puts(" value=");
        bridge_hex(rd(GPUBase+offset));bridge_puts("\n");
    }
#endif
    // Restore the complete A540 stopped control state, with no ring or
    // instruction address installed. Do not reuse the A530v1 high-bit mask.
    bridge_puts("[GPU power] requesting A540 CP halt\n");
    wr(GPUBase+Adreno::MEControl,initialME|Adreno::Halt);
    bridge_puts("[GPU power] A540-halt-readback=");bridge_hex(rd(GPUBase+Adreno::MEControl));bridge_puts("\n");
    bridge_puts("[GPU power] draining VBIF\n");wr(GPUBase+Adreno::VBIFHalt,15);
    for(unsigned elapsed=0;elapsed<100000 && (rd(GPUBase+Adreno::VBIFHalted)&15)!=15;elapsed+=10)wait(10);
    const uint32_t isolationRegisters[]={Adreno::MEControl,Adreno::VBIFHalt,Adreno::VBIFHalted,
        Adreno::InterruptStatus,Adreno::Status,Adreno::Status3};
    for(auto offset:isolationRegisters) {
        bridge_puts("[GPU isolation] offset=");bridge_hex(offset);bridge_puts(" value=");
        bridge_hex(rd(GPUBase+offset));bridge_puts("\n");
    }
    auto step=[&](const char *name,bool complete) {return report(name,complete?QcomPower::OK:QcomPower::Failed);};
    auto initializeSMMU=[&] {
        const auto result=coldSMMU.initialize(allocations[8].address,
            reinterpret_cast<volatile uint64_t *>(allocations[8].address));
        bridge_puts("[GPU SMMU] stage=");bridge_puts(coldSMMU.stage());
        bridge_puts(" offset=");bridge_hex(coldSMMU.lastOffset());
        bridge_puts(" expected=");bridge_hex(coldSMMU.lastValue());
        bridge_puts(" observed=");bridge_hex(coldSMMU.lastReadback());bridge_puts("\n");
        if(!result) {
            const uint32_t offsets[]={0,0x48,0x800,0xc00,0x1000,0x1800,0x8000,0x8010,
                0x8020,0x8024,0x8030,0x8034,0x8058,0x8060,0x8064,0x8068,0x87f4};
            for(auto offset:offsets) {
                bridge_puts("[GPU SMMU probe] offset=");bridge_hex(offset);bridge_puts(" value=");
                bridge_hex(read(offset));bridge_puts("\n");
            }
            bridge_puts("[GPU SMMU] isolation=");bridge_hex(isolated());bridge_puts("\n");
        }
        return result;
    };
    if(!report("cp-isolate",isolated()) || !step("core-reset",startup.reset()) ||
        !step("core-blocks",coldPower.enableBlocks(commands)) ||
        !step("smmu-probe",coldSMMU.probe(smmu.first,smmu.end,true)) ||
        !step("smmu-initialize",initializeSMMU()) ||
        !step("smmu-capture",captureSMMU()))return false;
    smmuReady=true;
    if(!report("noc-acquire",bootNoC.acquire(noc)))return false;
    cp={1,Adreno::PlatformContract::Required,GPU::ChipID,1,pair(GPUBase+Adreno::MEBase),pair(GPUBase+Adreno::PFPBase),0};
    powerReady=true;return unchanged();
}
#endif
} // namespace

extern "C" int bridge_gpu_prepare(EFI_BOOT_SERVICES *bs, void *tree) {
    state = new(storage) State;
    auto &s = *state;
    if(!prepareMMIO(bs))return -15;
    auto *contract = property(tree, "gpu@5000000", "qcom,adreno-smmu-handoff", sizeof(s.smmu));
    auto *noc = property(tree, "gpu@5000000", "qcom,gpu-noc-handoff", sizeof(s.noc));
    if (!contract || !noc || !property(tree, "gpu@5000000", "qcom,gpu-handoff", sizeof(s.cp))) return -1;
    memcpy(&s.smmu, contract, sizeof(s.smmu)); memcpy(&s.noc, noc, sizeof(s.noc));
    s.noc.version = 1; s.noc.flags = Adreno::NoCContract::Required;
    if (!s.noc.valid() || !s.rpm(tree)) return -2;
#ifdef BRIDGE_GPU_COLD_START
    const uint32_t leaves[]={75,77,176,186};
    if(!s.gcc.setIO({nullptr,gccRead,gccWrite,ioWait}))return -3;
    for(auto id:leaves)if(!s.gcc.allowClock(id))return -3;
    auto *point=property(tree,"gpu@5000000","qcom,gpu-firmware-power",sizeof(s.power));
    auto *rpmVersion=static_cast<const uint32_t *>(property(tree,"power-controller@778000","qcom,rpm-handoff-version",4));
    if(!point || !rpmVersion || *rpmVersion!=2 ||
        !property(tree,"power-controller@778000","qcom,rpm-transport-handoff",sizeof(QcomPower::RPMTransportHandoff)))return -11;
    memcpy(&s.power,point,sizeof(s.power));
    if(!s.gcc.initialize() || !s.coldPower.probe({nullptr,ccRead,ccWrite,ioWait,nullptr},s.power,s.rpmBootLevels[0],s.rpmBootLevels[1]))return -11;
    s.cp={1,Adreno::PlatformContract::Required,GPU::ChipID,1,0,0,0};
#else
    if (!s.gcc.setIO({nullptr,gccRead,unexpectedWrite,ioWait}) || !s.gcc.initialize() ||
        s.gcc.retainGPUState(s.gccToken) != GCC::Success ||
        !s.gpucc.configure(QcomGPUCCData::description,{nullptr,ccRead,unexpectedCCWrite,ioWait,nullptr},{}) ||
        !s.gpucc.initialize() || s.gpucc.retainGPUState(s.gpuccToken) != QcomPower::OK) return -3;
    const auto trust = rd(GPUBase + Adreno::TrustControl);
#ifdef BRIDGE_GPU_ZAP
    const bool invalidTrust = trust > 1;
#else
    const bool invalidTrust = trust != 0;
#endif
    if (invalidTrust || rd(GPUBase + Adreno::ContextSwitch)) return -4;
#ifndef BRIDGE_GPU_FIRMWARE
    if (rd(GPUBase + Adreno::GPMUReady) != 0xbabeface || rd(GPUBase + Adreno::GPMUError) ||
        (rd(GPUBase + Adreno::RBControl)&63) != 12) return -4;
#endif
    s.cp = {1, Adreno::PlatformContract::Required, GPU::ChipID, 1,
        pair(GPUBase + Adreno::MEBase), pair(GPUBase + Adreno::PFPBase), 0};
#ifndef BRIDGE_GPU_FIRMWARE
    if (!s.cp.valid()) return -5;
#endif
#endif
    UINTN key = 0; UINT32 version = 0;
    if (EFI_ERROR(bs->GetMemoryMap(&s.mapBytes, reinterpret_cast<EFI_MEMORY_DESCRIPTOR *>(s.memoryMap),
        &key, &s.descriptorBytes, &version)) || s.descriptorBytes < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        s.mapBytes % s.descriptorBytes) return -6;
#ifdef BRIDGE_GPU_COLD_START
    // Hardware geometry is read only after the domains have been powered.
    // The static staging aperture is bounded by the matching MSM8998 ABI.
    s.smmu.inputBits=40;s.smmu.outputBits=40;
#elif defined(BRIDGE_GPU_SMMU_INIT)
    // No active bank-0 translation is replaced in this path. Before reserving
    // XNU RAM, require a quiet CP: halted micro-engines, or zero instruction
    // bases and a reset empty ring. Final ownership/isolation follows EFI exit.
    if ((rd(GPUBase+Adreno::Status)&~1u) || (rd(GPUBase+Adreno::Status3)&(1u<<24)) ||
        rd(GPUBase+Adreno::RBRead)!=rd(GPUBase+Adreno::RBWrite) ||
        (rd(GPUBase+Adreno::InterruptStatus)&Adreno::ErrorIRQs) ||
        ((rd(GPUBase+Adreno::MEControl)&Adreno::Halt)!=Adreno::Halt &&
         (s.cp.pm4 || s.cp.pfp || rd(GPUBase+Adreno::RBRead))) ||
        !s.coldSMMU.probe(s.smmu.first,s.smmu.end)) return -9;
    s.smmu.inputBits=s.coldSMMU.inputBits(); s.smmu.outputBits=s.coldSMMU.outputBits();
#else
    if (!s.captureSMMU()) return -6;
#endif
#ifndef BRIDGE_GPU_FIRMWARE
    uint64_t pa = 0;
    if (!s.translate(s.cp.pm4, pa) || !s.ram(pa, 4) || !s.translate(s.cp.pfp, pa) || !s.ram(pa, 4)) return -7;
#endif
#ifdef BRIDGE_GPU_ZAP
    if (!s.stageZap(bs,tree)) return -8;
#endif
    s.prepared = true;
#ifdef BRIDGE_GPU_COLD_START
    bridge_puts("[GPU handoff] GX OFF and shared clock prerequisites captured; private startup memory reserved\n");
#else
    bridge_puts("[GPU handoff] GPU/SMMU captured; firmware mappings excluded from XNU arena\n");
#endif
    return 0;
}

extern "C" uint64_t bridge_gpu_next_arena(uint64_t start, uint64_t bytes) {
    if (!state || !state->prepared || !bytes || bytes > UINT64_MAX - start) return UINT64_MAX;
    uint64_t next = start;
    const uint64_t mask = 0x0000fffffffff000ULL;
    for (unsigned i = 0; i < state->pageCount; ++i) {
        const auto &p = state->pages[i];
        if (overlap(start, bytes, p.pa, 4096) && p.pa + 4096 > next) next = p.pa + 4096;
        const auto span = 1ULL << (39 - 9 * p.level);
        for (auto entry : p.entries) if ((entry & 1) && (p.level == 3 || (entry & 3) == 1)) {
            const uint64_t pa = entry & mask;
            if (overlap(start, bytes, pa, span) && pa + span > next) next = pa + span;
        }
    }
#ifdef BRIDGE_GPU_ZAP
    // Recheck the full EFI reservations, including alignment prefixes and
    // PAS metadata/image pages which intentionally have no GPU mapping.
    for(const auto &a:state->allocations)if(a.reservationBytes &&
        overlap(start,bytes,a.raw,a.reservationBytes) && a.raw+a.reservationBytes>next)next=a.raw+a.reservationBytes;
#endif
    return next > UINT64_MAX - 0x1fffff ? UINT64_MAX : (next + 0x1fffff) & ~0x1fffffULL;
}

#ifdef BRIDGE_GPU_COLD_START
extern "C" int bridge_gpu_ram_safe(uint64_t start,uint64_t bytes) {
    if(!state || !state->prepared || !bytes || bytes>UINT64_MAX-start ||
        state->physicalConflict(start,bytes)!=UINT64_MAX)return 0;
    for(const auto &a:state->allocations)if(a.reservationBytes &&
        overlap(start,bytes,a.raw,a.reservationBytes))return 0;
    return 1;
}
#endif

extern "C" int bridge_gpu_takeover(void *tree, uint64_t arena, uint64_t bytes) {
    if (!state || !state->prepared || !state->unchanged() || bridge_gpu_next_arena(arena, bytes) != arena) return -1;
    auto &s = *state;
#ifdef BRIDGE_GPU_COLD_START
    if(!s.startPower(tree))return -11;
    bridge_puts("[GPU handoff] RPM ACKs, CX/GX, Fabia/GCC/GPUCC and RBCCU/SP startup complete\n");
#endif
    const auto trust = rd(GPUBase + Adreno::TrustControl);
#ifdef BRIDGE_GPU_ZAP
    const bool invalidTrust = trust > 1;
#else
    const bool invalidTrust = trust != 0;
#endif
    if (pair(GPUBase + Adreno::MEBase) != s.cp.pm4 || pair(GPUBase + Adreno::PFPBase) != s.cp.pfp ||
        invalidTrust || rd(GPUBase + Adreno::ContextSwitch)) return -2;
#ifndef BRIDGE_GPU_FIRMWARE
    if (rd(GPUBase + Adreno::GPMUReady) != 0xbabeface || rd(GPUBase + Adreno::GPMUError)) return -2;
#endif
    bool idle = false;
    for (unsigned elapsed = 0; elapsed <= 100000; elapsed += 10) {
        if (!(rd(GPUBase + Adreno::Status) & ~1u) && rd(GPUBase + Adreno::RBRead) == rd(GPUBase + Adreno::RBWrite) &&
            !(rd(GPUBase + Adreno::Status3) & (1u << 24))) { idle = true; break; }
        wait(10);
    }
    if (!idle || (rd(GPUBase + Adreno::InterruptStatus) & Adreno::ErrorIRQs)) return -3;
    wr(GPUBase + Adreno::InterruptMask, 0);
    wr(GPUBase + Adreno::MEControl, rd(GPUBase + Adreno::MEControl) | Adreno::Halt);
    if ((rd(GPUBase + Adreno::MEControl) & Adreno::Halt) != Adreno::Halt) return -4;
    wr(GPUBase + Adreno::VBIFHalt, 15);
    unsigned elapsed = 0;
    while ((rd(GPUBase + Adreno::VBIFHalted) & 15) != 15 && elapsed < 100000) { wait(10); elapsed += 10; }
    if ((rd(GPUBase + Adreno::VBIFHalted) & 15) != 15) return -5;
#if defined(BRIDGE_GPU_SMMU_INIT) && !defined(BRIDGE_GPU_COLD_START)
    // Reset CP before installing the new context, so its tables never depend
    // on retaining an uninitialized SMMU across the GPU reset.
    if (!s.startup.reset() || !s.coldSMMU.initialize(s.allocations[8].address,
            reinterpret_cast<volatile uint64_t *>(s.allocations[8].address)) || !s.captureSMMU()) return -10;
    s.smmuReady=true;
    bridge_puts("[GPU handoff] SMMU bank 0 initialized; SID 0 / ASID 0 readback and TLB sync verified\n");
#endif
    for (unsigned i = 0; i < s.pageCount; ++i) bridge_clean(s.pages[i].pa, s.pages[i].pa + 4096);
    if (s.tlb.sync() != QcomPower::OK || !s.unchanged()) return -5;
#ifdef BRIDGE_GPU_ZAP
    if (!s.loadZap() || !s.unchanged()) {
        bridge_puts("[GPU handoff] ZAP PAS authentication/bootstrap failed; reserved resources retained until reset\n");
        return -7;
    }
    bridge_puts("[GPU handoff] signed ZAP loaded/authenticated; CP non-secure timestamp verified\n");
#endif
    // DT properties are 4-byte aligned; memcpy avoids unaligned 64-bit stores
    // with EDK2's SCTLR.A setting. Publish versions only after all proofs pass.
    auto *cp = property(tree,"gpu@5000000","qcom,gpu-handoff",sizeof(s.cp));
    auto *smmu = property(tree,"gpu@5000000","qcom,adreno-smmu-handoff",sizeof(s.smmu));
    auto *noc = property(tree,"gpu@5000000","qcom,gpu-noc-handoff",sizeof(s.noc));
    if (!cp || !smmu || !noc) return -6;
#ifdef BRIDGE_GPU_FIRMWARE
    auto *firmware=property(tree,"gpu@5000000","qcom,gpu-firmware-handoff",sizeof(Adreno::FirmwareHandoff));
    const Adreno::FirmwareHandoff firmwareState{1,Adreno::FirmwareHandoff::Required,s.cp.pm4,s.cp.pfp,s.firmwareVA[2],
        uint32_t(s.allocations[5].bytes),uint32_t(s.allocations[6].bytes),s.firmware.gpmuWords(),s.power.frequency,s.power.microvolts,{0,0,0}};
    if(!firmware || !firmwareState.valid(s.cp))return -13;
#endif
#ifdef BRIDGE_GPU_COLD_START
    auto *rpm=property(tree,"power-controller@778000","qcom,rpm-transport-handoff",sizeof(QcomPower::RPMTransportHandoff));
    QcomPower::RPMTransportHandoff channel{};
    if(!rpm || !s.unchanged() || s.bootRPM.handoff(channel)!=QcomPower::OK)return -12;
    auto *gfx=property(tree,"gpu@5000000","qcom,gpu-gfx-handoff",sizeof(s.gfxHandoff));
    auto *spmi=property(tree,"power-controller@800f000","qcom,gpu-gfx-handoff",sizeof(s.gfxHandoff));
    if(!gfx || !spmi || s.gfxRegulator.validateGPUState(s.gfxToken)!=QcomPower::OK)return -14;
    memcpy(gfx,&s.gfxHandoff,sizeof(s.gfxHandoff));memcpy(spmi,&s.gfxHandoff,sizeof(s.gfxHandoff));
    s.channelYielded=true;memcpy(rpm,&channel,sizeof(channel));
#endif
    memcpy(smmu,&s.smmu,sizeof(s.smmu)); memcpy(noc,&s.noc,sizeof(s.noc)); memcpy(cp,&s.cp,sizeof(s.cp));
#ifdef BRIDGE_GPU_FIRMWARE
    memcpy(firmware,&firmwareState,sizeof(firmwareState));
#endif
    bridge_puts("[GPU handoff] post-EFI CP halt/AXI drain/TLB sync complete; native handoffs published\n");
    return 0;
}
