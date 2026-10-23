// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2016-2017 The Linux Foundation. All rights reserved.
// A540-only adaptation of Linux v6.12 a5xx_gpu.c/a5xx_power.c, 2026-10-15.
// Register definitions and original sources: docs/gpu/reference/sources.json.
#pragma once
#include "AdrenoSubmit.h"

namespace Adreno {

struct FirmwarePower {
    uint32_t version, frequency, microvolts, reserved;
    bool valid() const {
        return version == 1 && !reserved && frequency >= 19200000 && frequency <= 1000000000 &&
            microvolts >= 400000 && microvolts <= 2000000 && !(microvolts % 1000);
    }
};
static_assert(sizeof(FirmwarePower) == 16, "GPMU operating point layout");

// Optional loader-owned firmware needed for a native reset. All three GPU
// ranges stay read-only and outside the native allocation aperture until
// platform reset; the native mapper pins and rechecks every leaf descriptor.
struct FirmwareHandoff {
    uint32_t version,flags;
    uint64_t pm4,pfp,gpmu;
    uint32_t pm4Bytes,pfpBytes,gpmuWords,frequency,microvolts,reserved[3];
    enum : uint32_t { LoaderOwned=1,GPUReadOnly=2,Retained=4,Initialized=8,ResetAllowed=16,Required=31 };
    FirmwarePower power() const {return {1,frequency,microvolts,0};}
    bool valid(const PlatformContract &cp) const {
        if(version!=1 || flags!=Required || !cp.valid() || pm4!=cp.pm4 || pfp!=cp.pfp ||
            reserved[0] || reserved[1] || reserved[2] || !power().valid() ||
            ((pm4|pfp|gpmu|pm4Bytes|pfpBytes)&4095) || !pm4Bytes || !pfpBytes ||
            pm4Bytes>0x100000 || pfpBytes>0x100000 || !gpmuWords || gpmuWords>0x1021 ||
            !deviceRange(pm4,pm4Bytes) || !deviceRange(pfp,pfpBytes) || !deviceRange(gpmu,uint64_t(gpmuWords)*4))return false;
        const uint64_t ranges[3][2]={{pm4,pm4Bytes},{pfp,pfpBytes},{gpmu,uint64_t(gpmuWords)*4}};
        for(unsigned i=0;i<3;++i)for(unsigned j=0;j<i;++j)
            if(ranges[i][0]<ranges[j][0]+ranges[j][1] && ranges[j][0]<ranges[i][0]+ranges[i][1])return false;
        return true;
    }
};
static_assert(sizeof(FirmwareHandoff)==64,"loader firmware reset ABI");

template<class Registers> bool enableCoreBlocks(Registers &r) {
    // a5xx_pm_resume: RBCCU first limits power droop, then SP. Completion is
    // observed in real hardware; delay alone is not a power-ready assertion.
    r.write(0xa887*4,0x778000);r.delay(3);
    for(unsigned n=0;;++n){const auto v=r.read(0xa88d*4);if(v==UINT32_MAX)return false;if(v&(1u<<20))break;if(n==100)return false;r.delay(1);}
    r.write(0xa881*4,0x778000);
    for(unsigned n=0;;++n){const auto v=r.read(0xa88b*4);if(v==UINT32_MAX)return false;if(v&(1u<<20))break;if(n==100)return false;r.delay(1);}
    return true;
}

// Original firmware files, without rewriting their headers or contents. This
// container is packaging only; it does not provide firmware authentication.
class Firmware {
    struct File { const uint8_t *data = nullptr; uint32_t bytes = 0; } files[3];
    uint32_t first = 0, instructions = 0;
    static uint32_t word(const uint8_t *p) {
        return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }
public:
    bool open(const void *source, size_t bytes) {
        for (auto &f : files) f = {};
        first = instructions = 0;
        if (!source || bytes < 80 || bytes > 3 * 1024 * 1024 + 128) return false;
        auto *p = static_cast<const uint8_t *>(source);
        if (word(p) != 0x57464751 || word(p+4) != 1 || word(p+8) != 3 || word(p+12) != bytes ||
            word(p+16) != 16 || word(p+20) || word(p+24) || word(p+28)) return false;
        uint32_t end = 80;
        for (unsigned i = 0; i < 3; ++i) {
            const auto *entry = p + 32 + 16 * i;
            const uint32_t offset = word(entry+4), length = word(entry+8);
            if (word(entry) != i || word(entry+12) || (offset & 15) || offset < end || length < 8 ||
                (length & 3) || length > 1024 * 1024 || offset > bytes || length > bytes-offset) return false;
            files[i] = {p+offset,length}; end = offset+length;
        }
        if (end != bytes || files[2].bytes < 16) return false;
        const auto *g = files[2].data;
        const uint32_t remaining = word(g), metadata = word(g+8);
        if (word(g+4) != 2 || remaining < 3 || remaining >= files[2].bytes/4 || metadata > remaining-3) return false;
        first = metadata+3; instructions = remaining-metadata-2;
        // Instruction RAM is 0x8800..0x97ff; data RAM begins at 0x9800.
        if (instructions > 0x1000) { instructions = 0; return false; }
        return true;
    }
    uint32_t cpBytes(unsigned index) const { return index < 2 && instructions ? files[index].bytes-4 : 0; }
    uint32_t gpmuWords() const { return instructions ? instructions + (instructions+126)/127 : 0; }
    bool copyCP(unsigned index, void *destination, uint32_t capacity) const {
        const auto bytes = cpBytes(index);
        if (!destination || !bytes || capacity < bytes) return false;
        auto *out = static_cast<uint8_t *>(destination);
        // adreno_fw_create_bo(): the first file word is not CP microcode.
        for (unsigned i = 0; i < bytes; ++i) out[i] = files[index].data[4+i];
        for (unsigned i = bytes; i < capacity; ++i) out[i] = 0;
        return true;
    }
    bool gpmuCommands(uint32_t *out, uint32_t capacity) const {
        if (!out || !instructions || capacity < gpmuWords()) return false;
        uint32_t offset = 0, count = 0;
        while (offset < instructions) {
            const uint32_t n = instructions-offset > 127 ? 127 : instructions-offset;
            const uint32_t reg = 0x8800+offset;
            out[count++] = 0x40000000 | n | (parity(n)<<7) | (reg<<8) | (parity(reg)<<27);
            for (unsigned i = 0; i < n; ++i) out[count++] = word(files[2].data+4*(first+offset+i));
            offset += n;
        }
        return true;
    }
};

// DWORD indices from the pinned A5xx register XML.
enum Register : uint32_t {
    CP_ADDR_MODE_CNTL = 0x0860,
    CP_CHICKEN_DBG = 0x0833,
    CP_MEQ_THRESHOLDS = 0x0825,
    CP_MERCIU_SIZE = 0x0826,
    CP_PERFCTR_CP_SEL_0 = 0x0bb0,
    CP_PROTECT = 0x0880,
    CP_PROTECT_CNTL = 0x08a0,
    CP_ROQ_THRESHOLDS_1 = 0x081f,
    CP_ROQ_THRESHOLDS_2 = 0x0820,
    GPMU_CM3_SYSRESET = 0xa8d8,
    GPMU_GPMU_PWR_THRESHOLD = 0xac80,
    GPMU_GPMU_VOLTAGE = 0xac60,
    GPMU_PWR_COL_BINNING_CTRL = 0xa894,
    GPMU_PWR_COL_INTER_FRAME_CTRL = 0xa892,
    GPMU_PWR_COL_INTER_FRAME_HYST = 0xa893,
    GPMU_PWR_COL_STAGGER_DELAY = 0xa891,
    GRAS_ADDR_MODE_CNTL = 0x0c81,
    HLSQ_ADDR_MODE_CNTL = 0x0e05,
    HLSQ_DBG_ECO_CNTL = 0x0e04,
    PC_ADDR_MODE_CNTL = 0x0d01,
    PC_DBG_ECO_CNTL = 0x0d00,
    RBBM_AHB_CNTL0 = 0x0093,
    RBBM_AHB_CNTL1 = 0x0094,
    RBBM_AHB_CNTL2 = 0x0095,
    RBBM_CLOCK_CNTL = 0x0067,
    RBBM_CLOCK_CNTL2_RAC = 0x008b,
    RBBM_CLOCK_CNTL2_RB0 = 0x007c,
    RBBM_CLOCK_CNTL2_RB1 = 0x007d,
    RBBM_CLOCK_CNTL2_RB2 = 0x007e,
    RBBM_CLOCK_CNTL2_RB3 = 0x007f,
    RBBM_CLOCK_CNTL2_SP0 = 0x006c,
    RBBM_CLOCK_CNTL2_SP1 = 0x006d,
    RBBM_CLOCK_CNTL2_SP2 = 0x006e,
    RBBM_CLOCK_CNTL2_SP3 = 0x006f,
    RBBM_CLOCK_CNTL2_TP0 = 0x004e,
    RBBM_CLOCK_CNTL2_TP1 = 0x004f,
    RBBM_CLOCK_CNTL2_TP2 = 0x0050,
    RBBM_CLOCK_CNTL2_TP3 = 0x0051,
    RBBM_CLOCK_CNTL2_UCHE = 0x005b,
    RBBM_CLOCK_CNTL3_TP0 = 0x0052,
    RBBM_CLOCK_CNTL3_TP1 = 0x0053,
    RBBM_CLOCK_CNTL3_TP2 = 0x0054,
    RBBM_CLOCK_CNTL3_TP3 = 0x0055,
    RBBM_CLOCK_CNTL3_UCHE = 0x005c,
    RBBM_CLOCK_CNTL4_UCHE = 0x005d,
    RBBM_CLOCK_CNTL_CCU0 = 0x0082,
    RBBM_CLOCK_CNTL_CCU1 = 0x0083,
    RBBM_CLOCK_CNTL_CCU2 = 0x0084,
    RBBM_CLOCK_CNTL_CCU3 = 0x0085,
    RBBM_CLOCK_CNTL_RAC = 0x008a,
    RBBM_CLOCK_CNTL_RB0 = 0x0078,
    RBBM_CLOCK_CNTL_RB1 = 0x0079,
    RBBM_CLOCK_CNTL_RB2 = 0x007a,
    RBBM_CLOCK_CNTL_RB3 = 0x007b,
    RBBM_CLOCK_CNTL_SP0 = 0x0068,
    RBBM_CLOCK_CNTL_SP1 = 0x0069,
    RBBM_CLOCK_CNTL_SP2 = 0x006a,
    RBBM_CLOCK_CNTL_SP3 = 0x006b,
    RBBM_CLOCK_CNTL_TP0 = 0x004a,
    RBBM_CLOCK_CNTL_TP1 = 0x004b,
    RBBM_CLOCK_CNTL_TP2 = 0x004c,
    RBBM_CLOCK_CNTL_TP3 = 0x004d,
    RBBM_CLOCK_CNTL_TSE_RAS_RBBM = 0x0063,
    RBBM_CLOCK_CNTL_UCHE = 0x005a,
    RBBM_CLOCK_DELAY2_TP0 = 0x00a8,
    RBBM_CLOCK_DELAY2_TP1 = 0x00a9,
    RBBM_CLOCK_DELAY2_TP2 = 0x00aa,
    RBBM_CLOCK_DELAY2_TP3 = 0x00ab,
    RBBM_CLOCK_DELAY3_TP0 = 0x00ac,
    RBBM_CLOCK_DELAY3_TP1 = 0x00ad,
    RBBM_CLOCK_DELAY3_TP2 = 0x00ae,
    RBBM_CLOCK_DELAY3_TP3 = 0x00af,
    RBBM_CLOCK_DELAY_GPC = 0x0061,
    RBBM_CLOCK_DELAY_GPMU = 0x00c9,
    RBBM_CLOCK_DELAY_HLSQ = 0x0066,
    RBBM_CLOCK_DELAY_RAC = 0x0081,
    RBBM_CLOCK_DELAY_RB_CCU_L1_0 = 0x008c,
    RBBM_CLOCK_DELAY_RB_CCU_L1_1 = 0x008d,
    RBBM_CLOCK_DELAY_RB_CCU_L1_2 = 0x008e,
    RBBM_CLOCK_DELAY_RB_CCU_L1_3 = 0x008f,
    RBBM_CLOCK_DELAY_SP0 = 0x0074,
    RBBM_CLOCK_DELAY_SP1 = 0x0075,
    RBBM_CLOCK_DELAY_SP2 = 0x0076,
    RBBM_CLOCK_DELAY_SP3 = 0x0077,
    RBBM_CLOCK_DELAY_TP0 = 0x00a4,
    RBBM_CLOCK_DELAY_TP1 = 0x00a5,
    RBBM_CLOCK_DELAY_TP2 = 0x00a6,
    RBBM_CLOCK_DELAY_TP3 = 0x00a7,
    RBBM_CLOCK_DELAY_TSE_RAS_RBBM = 0x0065,
    RBBM_CLOCK_DELAY_UCHE = 0x005f,
    RBBM_CLOCK_DELAY_VFD = 0x0092,
    RBBM_CLOCK_HYST2_TP0 = 0x00b4,
    RBBM_CLOCK_HYST2_TP1 = 0x00b5,
    RBBM_CLOCK_HYST2_TP2 = 0x00b6,
    RBBM_CLOCK_HYST2_TP3 = 0x00b7,
    RBBM_CLOCK_HYST3_TP0 = 0x00b8,
    RBBM_CLOCK_HYST3_TP1 = 0x00b9,
    RBBM_CLOCK_HYST3_TP2 = 0x00ba,
    RBBM_CLOCK_HYST3_TP3 = 0x00bb,
    RBBM_CLOCK_HYST_GPC = 0x0062,
    RBBM_CLOCK_HYST_GPMU = 0x00ca,
    RBBM_CLOCK_HYST_RAC = 0x0080,
    RBBM_CLOCK_HYST_RB_CCU0 = 0x0086,
    RBBM_CLOCK_HYST_RB_CCU1 = 0x0087,
    RBBM_CLOCK_HYST_RB_CCU2 = 0x0088,
    RBBM_CLOCK_HYST_RB_CCU3 = 0x0089,
    RBBM_CLOCK_HYST_SP0 = 0x0070,
    RBBM_CLOCK_HYST_SP1 = 0x0071,
    RBBM_CLOCK_HYST_SP2 = 0x0072,
    RBBM_CLOCK_HYST_SP3 = 0x0073,
    RBBM_CLOCK_HYST_TP0 = 0x00b0,
    RBBM_CLOCK_HYST_TP1 = 0x00b1,
    RBBM_CLOCK_HYST_TP2 = 0x00b2,
    RBBM_CLOCK_HYST_TP3 = 0x00b3,
    RBBM_CLOCK_HYST_TSE_RAS_RBBM = 0x0064,
    RBBM_CLOCK_HYST_UCHE = 0x005e,
    RBBM_CLOCK_HYST_VFD = 0x0090,
    RBBM_CLOCK_MODE_GPC = 0x0060,
    RBBM_CLOCK_MODE_VFD = 0x0091,
    RBBM_INTERFACE_HANG_INT_CNTL = 0x002f,
    RBBM_ISDB_CNT = 0x0533,
    RBBM_PERFCTR_CNTL = 0x0464,
    RBBM_PERFCTR_GPU_BUSY_MASKED = 0x046f,
    RBBM_PERFCTR_RBBM_SEL_0 = 0x046b,
    RBBM_SECVID_TSB_ADDR_MODE_CNTL = 0xf810,
    RBBM_SECVID_TSB_CNTL = 0xf803,
    RBBM_SECVID_TSB_TRUSTED_BASE_HI = 0xf801,
    RBBM_SECVID_TSB_TRUSTED_BASE_LO = 0xf800,
    RBBM_SECVID_TSB_TRUSTED_SIZE = 0xf802,
    RBBM_SW_RESET_CMD = 0x0043,
    RB_ADDR_MODE_CNTL = 0x0cc5,
    RB_MODE_CNTL = 0x0cc6,
    SP_ADDR_MODE_CNTL = 0x0ec1,
    TPL1_ADDR_MODE_CNTL = 0x0f01,
    TPL1_MODE_CNTL = 0x0f02,
    UCHE_ADDR_MODE_CNTL = 0x0e80,
    UCHE_CACHE_WAYS = 0x0e96,
    UCHE_DBG_ECO_CNTL_2 = 0x0e8f,
    UCHE_GMEM_RANGE_MAX_HI = 0x0e8e,
    UCHE_GMEM_RANGE_MAX_LO = 0x0e8d,
    UCHE_GMEM_RANGE_MIN_HI = 0x0e8c,
    UCHE_GMEM_RANGE_MIN_LO = 0x0e8b,
    UCHE_MODE_CNTL = 0x0e81,
    UCHE_TRAP_BASE_HI = 0x0e8a,
    UCHE_TRAP_BASE_LO = 0x0e89,
    UCHE_WRITE_THRU_BASE_HI = 0x0e88,
    UCHE_WRITE_THRU_BASE_LO = 0x0e87,
    VBIF_GATE_OFF_WRREQ_EN = 0x302a,
    VBIF_ROUND_ROBIN_QOS_ARB = 0x3049,
    VFD_ADDR_MODE_CNTL = 0x0e41,
    VPC_ADDR_MODE_CNTL = 0x0e61,
    VPC_DBG_ECO_CNTL = 0x0e60,
    VSC_ADDR_MODE_CNTL = 0x0bc1,
};

// The bridge calls this only after EFI exits and all DMA mappings are proven.
// reset() and start() are separate so the caller can recheck the retained SMMU
// after the real GPU reset, before any CP DMA is enabled.
template<class Registers> class Startup {
    Registers &r;
    volatile uint32_t *ring = nullptr;
    uint32_t cursor = 0;
    bool resetDone = false, owned = false;
    void set(uint32_t reg, uint32_t value) { r.write(reg*4,value); }
    uint32_t get(uint32_t reg) { return r.read(reg*4); }
    uint64_t readAddress(uint32_t reg) { return r.read(reg)|(uint64_t(r.read(reg+4))<<32); }
    void address(uint32_t reg, uint64_t value) { r.write(reg,uint32_t(value)); r.write(reg+4,uint32_t(value>>32)); }
    void put(uint32_t value) { ring[cursor++] = value; }
    bool idle() {
        for (unsigned elapsed = 0; elapsed < 100000; elapsed += 10) {
            if ((r.read(InterruptStatus)&ErrorIRQs) || (r.read(CPInterruptStatus)&0x3f)) return false;
            if (r.read(RBRead) == cursor && !(r.read(Adreno::Status) & ~1u) && !(r.read(Status3)&(1u<<24))) return true;
            r.delay(10);
        }
        return false;
    }
    bool kick() { r.publish(); r.write(RBWrite,cursor); return idle(); }
    static uint32_t protection(uint32_t base, unsigned log2Length) { return 0x60000000 | (log2Length<<24) | (base<<2); }
    void configure() {
        // A540 has 1 MiB GMEM, highest bank bit 15, and LMLOADKILL_DISABLE.
        set(VBIF_ROUND_ROBIN_QOS_ARB,3);
        set(VBIF_GATE_OFF_WRREQ_EN,9);
        set(RBBM_PERFCTR_GPU_BUSY_MASKED,0xffffffff);
        set(RBBM_AHB_CNTL0,1);
        set(RBBM_INTERFACE_HANG_INT_CNTL,0x4000ffff);
        set(RBBM_PERFCTR_CNTL,1);
        set(CP_PERFCTR_CP_SEL_0,0);
        set(RBBM_PERFCTR_RBBM_SEL_0,6);
        set(UCHE_CACHE_WAYS,2);
        set(UCHE_TRAP_BASE_LO,0xffff0000);
        set(UCHE_TRAP_BASE_HI,0x1ffff);
        set(UCHE_WRITE_THRU_BASE_LO,0xffff0000);
        set(UCHE_WRITE_THRU_BASE_HI,0x1ffff);
        set(UCHE_GMEM_RANGE_MIN_LO,0x100000);
        set(UCHE_GMEM_RANGE_MIN_HI,0);
        set(UCHE_GMEM_RANGE_MAX_LO,0x1fffff);
        set(UCHE_GMEM_RANGE_MAX_HI,0);
        set(CP_MEQ_THRESHOLDS,0x40);
        set(CP_MERCIU_SIZE,0x400);
        set(CP_ROQ_THRESHOLDS_2,0x80000060);
        set(CP_ROQ_THRESHOLDS_1,0x40201b16);
        set(PC_DBG_ECO_CNTL,(0x400u<<11)|(0x300u<<22));
        set(UCHE_MODE_CNTL,1u<<29);
        set(CP_CHICKEN_DBG,0x02000000);
        set(RBBM_AHB_CNTL1,0xa6ffffff);
        hardwareClockGating(true);
        set(RBBM_AHB_CNTL2,0x3f);
        set(TPL1_MODE_CNTL,2u<<7);
        set(RB_MODE_CNTL,2u<<1);
        set(UCHE_DBG_ECO_CNTL_2,2);
        set(CP_PROTECT_CNTL,7);
        set(VPC_DBG_ECO_CNTL,get(VPC_DBG_ECO_CNTL)|(1u<<10)|(1u<<23));
        set(HLSQ_DBG_ECO_CNTL,get(HLSQ_DBG_ECO_CNTL)&~(1u<<18));
        set(CP_PROTECT+0,protection(0x4,2));
        set(CP_PROTECT+1,protection(0x8,3));
        set(CP_PROTECT+2,protection(0x10,4));
        set(CP_PROTECT+3,protection(0x20,5));
        set(CP_PROTECT+4,protection(0x40,6));
        set(CP_PROTECT+5,protection(0x80,6));
        set(CP_PROTECT+6,protection(0xf800,4));
        set(CP_PROTECT+7,protection(0xf400,1));
        set(CP_PROTECT+8,protection(0x800,6));
        set(CP_PROTECT+9,protection(0x840,3));
        set(CP_PROTECT+10,protection(0x880,5));
        set(CP_PROTECT+11,protection(0xaa0,0));
        set(CP_PROTECT+12,protection(0xcc0,0));
        set(CP_PROTECT+13,protection(0xcf0,1));
        set(CP_PROTECT+14,protection(0xe68,3));
        set(CP_PROTECT+15,protection(0xe70,4));
        set(CP_PROTECT+16,protection(0xe80,4));
        set(CP_PROTECT+17,protection(0x10000,15));
        set(RBBM_SECVID_TSB_CNTL,0);
        set(RBBM_SECVID_TSB_TRUSTED_BASE_LO,0);
        set(RBBM_SECVID_TSB_TRUSTED_BASE_HI,0);
        set(RBBM_SECVID_TSB_TRUSTED_SIZE,0);
        set(CP_ADDR_MODE_CNTL,1);
        set(VSC_ADDR_MODE_CNTL,1);
        set(GRAS_ADDR_MODE_CNTL,1);
        set(RB_ADDR_MODE_CNTL,1);
        set(PC_ADDR_MODE_CNTL,1);
        set(HLSQ_ADDR_MODE_CNTL,1);
        set(VFD_ADDR_MODE_CNTL,1);
        set(VPC_ADDR_MODE_CNTL,1);
        set(UCHE_ADDR_MODE_CNTL,1);
        set(SP_ADDR_MODE_CNTL,1);
        set(TPL1_ADDR_MODE_CNTL,1);
        set(RBBM_SECVID_TSB_ADDR_MODE_CNTL,1);
    }
public:
    explicit Startup(Registers &registers) : r(registers) {}
    void hardwareClockGating(bool enabled) {
        static constexpr struct { uint32_t reg, value; } hwcg[] = {
            {RBBM_CLOCK_CNTL_SP0,0x02222222},
            {RBBM_CLOCK_CNTL_SP1,0x02222222},
            {RBBM_CLOCK_CNTL_SP2,0x02222222},
            {RBBM_CLOCK_CNTL_SP3,0x02222222},
            {RBBM_CLOCK_CNTL2_SP0,0x02222220},
            {RBBM_CLOCK_CNTL2_SP1,0x02222220},
            {RBBM_CLOCK_CNTL2_SP2,0x02222220},
            {RBBM_CLOCK_CNTL2_SP3,0x02222220},
            {RBBM_CLOCK_HYST_SP0,0x0000F3CF},
            {RBBM_CLOCK_HYST_SP1,0x0000F3CF},
            {RBBM_CLOCK_HYST_SP2,0x0000F3CF},
            {RBBM_CLOCK_HYST_SP3,0x0000F3CF},
            {RBBM_CLOCK_DELAY_SP0,0x00000080},
            {RBBM_CLOCK_DELAY_SP1,0x00000080},
            {RBBM_CLOCK_DELAY_SP2,0x00000080},
            {RBBM_CLOCK_DELAY_SP3,0x00000080},
            {RBBM_CLOCK_CNTL_TP0,0x22222222},
            {RBBM_CLOCK_CNTL_TP1,0x22222222},
            {RBBM_CLOCK_CNTL_TP2,0x22222222},
            {RBBM_CLOCK_CNTL_TP3,0x22222222},
            {RBBM_CLOCK_CNTL2_TP0,0x22222222},
            {RBBM_CLOCK_CNTL2_TP1,0x22222222},
            {RBBM_CLOCK_CNTL2_TP2,0x22222222},
            {RBBM_CLOCK_CNTL2_TP3,0x22222222},
            {RBBM_CLOCK_CNTL3_TP0,0x00002222},
            {RBBM_CLOCK_CNTL3_TP1,0x00002222},
            {RBBM_CLOCK_CNTL3_TP2,0x00002222},
            {RBBM_CLOCK_CNTL3_TP3,0x00002222},
            {RBBM_CLOCK_HYST_TP0,0x77777777},
            {RBBM_CLOCK_HYST_TP1,0x77777777},
            {RBBM_CLOCK_HYST_TP2,0x77777777},
            {RBBM_CLOCK_HYST_TP3,0x77777777},
            {RBBM_CLOCK_HYST2_TP0,0x77777777},
            {RBBM_CLOCK_HYST2_TP1,0x77777777},
            {RBBM_CLOCK_HYST2_TP2,0x77777777},
            {RBBM_CLOCK_HYST2_TP3,0x77777777},
            {RBBM_CLOCK_HYST3_TP0,0x00007777},
            {RBBM_CLOCK_HYST3_TP1,0x00007777},
            {RBBM_CLOCK_HYST3_TP2,0x00007777},
            {RBBM_CLOCK_HYST3_TP3,0x00007777},
            {RBBM_CLOCK_DELAY_TP0,0x11111111},
            {RBBM_CLOCK_DELAY_TP1,0x11111111},
            {RBBM_CLOCK_DELAY_TP2,0x11111111},
            {RBBM_CLOCK_DELAY_TP3,0x11111111},
            {RBBM_CLOCK_DELAY2_TP0,0x11111111},
            {RBBM_CLOCK_DELAY2_TP1,0x11111111},
            {RBBM_CLOCK_DELAY2_TP2,0x11111111},
            {RBBM_CLOCK_DELAY2_TP3,0x11111111},
            {RBBM_CLOCK_DELAY3_TP0,0x00001111},
            {RBBM_CLOCK_DELAY3_TP1,0x00001111},
            {RBBM_CLOCK_DELAY3_TP2,0x00001111},
            {RBBM_CLOCK_DELAY3_TP3,0x00001111},
            {RBBM_CLOCK_CNTL_UCHE,0x22222222},
            {RBBM_CLOCK_CNTL2_UCHE,0x22222222},
            {RBBM_CLOCK_CNTL3_UCHE,0x22222222},
            {RBBM_CLOCK_CNTL4_UCHE,0x00222222},
            {RBBM_CLOCK_HYST_UCHE,0x00444444},
            {RBBM_CLOCK_DELAY_UCHE,0x00000002},
            {RBBM_CLOCK_CNTL_RB0,0x22222222},
            {RBBM_CLOCK_CNTL_RB1,0x22222222},
            {RBBM_CLOCK_CNTL_RB2,0x22222222},
            {RBBM_CLOCK_CNTL_RB3,0x22222222},
            {RBBM_CLOCK_CNTL2_RB0,0x00222222},
            {RBBM_CLOCK_CNTL2_RB1,0x00222222},
            {RBBM_CLOCK_CNTL2_RB2,0x00222222},
            {RBBM_CLOCK_CNTL2_RB3,0x00222222},
            {RBBM_CLOCK_CNTL_CCU0,0x00022220},
            {RBBM_CLOCK_CNTL_CCU1,0x00022220},
            {RBBM_CLOCK_CNTL_CCU2,0x00022220},
            {RBBM_CLOCK_CNTL_CCU3,0x00022220},
            {RBBM_CLOCK_CNTL_RAC,0x05522222},
            {RBBM_CLOCK_CNTL2_RAC,0x00505555},
            {RBBM_CLOCK_HYST_RB_CCU0,0x04040404},
            {RBBM_CLOCK_HYST_RB_CCU1,0x04040404},
            {RBBM_CLOCK_HYST_RB_CCU2,0x04040404},
            {RBBM_CLOCK_HYST_RB_CCU3,0x04040404},
            {RBBM_CLOCK_HYST_RAC,0x07444044},
            {RBBM_CLOCK_DELAY_RB_CCU_L1_0,0x00000002},
            {RBBM_CLOCK_DELAY_RB_CCU_L1_1,0x00000002},
            {RBBM_CLOCK_DELAY_RB_CCU_L1_2,0x00000002},
            {RBBM_CLOCK_DELAY_RB_CCU_L1_3,0x00000002},
            {RBBM_CLOCK_DELAY_RAC,0x00010011},
            {RBBM_CLOCK_CNTL_TSE_RAS_RBBM,0x04222222},
            {RBBM_CLOCK_MODE_GPC,0x02222222},
            {RBBM_CLOCK_MODE_VFD,0x00002222},
            {RBBM_CLOCK_HYST_TSE_RAS_RBBM,0x00000000},
            {RBBM_CLOCK_HYST_GPC,0x04104004},
            {RBBM_CLOCK_HYST_VFD,0x00000000},
            {RBBM_CLOCK_DELAY_HLSQ,0x00000000},
            {RBBM_CLOCK_DELAY_TSE_RAS_RBBM,0x00004000},
            {RBBM_CLOCK_DELAY_GPC,0x00000200},
            {RBBM_CLOCK_DELAY_VFD,0x00002222},
            {RBBM_CLOCK_DELAY_GPMU,0x770},
            {RBBM_CLOCK_HYST_GPMU,4},
            {RBBM_CLOCK_CNTL,0xaaa8aa00},
            {RBBM_ISDB_CNT,0x182},
        };
        for (const auto &entry : hwcg) set(entry.reg,enabled?entry.value:entry.reg==RBBM_ISDB_CNT?0x180:0);
    }
    bool reset() {
        if (owned || resetDone || (r.read(MEControl)&Halt) != Halt || (r.read(VBIFHalted)&15) != 15) return false;
        owned = true;
        set(RBBM_SW_RESET_CMD,1); (void)get(RBBM_SW_RESET_CMD); set(RBBM_SW_RESET_CMD,0);
        r.write(InterruptMask,0); r.write(MEControl,Halt); r.write(VBIFHalt,15);
        if (!stop() || get(RBBM_SW_RESET_CMD) || r.read(RBRead) || r.read(RBWrite) ||
            r.read(ContextSwitch) || r.read(TrustControl)>1) return false;
        // A retained BABEFACE cannot count as a new firmware response.
        set(GPMU_CM3_SYSRESET,1); r.write(GPMUReady,0); r.write(GPMUError,0);
        if (get(GPMU_CM3_SYSRESET) != 1 || r.read(GPMUReady) || r.read(GPMUError)) return false;
        resetDone = true; return true;
    }
    bool start(const PlatformContract &p, volatile uint32_t *cpu, uint64_t va) {
        if (!resetDone || owned || !p.valid() || !cpu || (va&4095) || !deviceRange(va,RingDwords*4) ||
            r.read(RBRead) || r.read(RBWrite) || (r.read(MEControl)&Halt)!=Halt || (r.read(VBIFHalted)&15)!=15) return false;
        resetDone = false; owned = true; ring = cpu; cursor = 0;
        for (unsigned i = 0; i < RingDwords; ++i) ring[i] = 0;
        configure();
        address(MEBase,p.pm4); address(PFPBase,p.pfp); address(RBBase,va);
        r.write(ContextSwitch,0); r.write(RBControl,RingControl); r.write(RBWrite,0);
        r.write(InterruptMask,0); r.write(InterruptClear,OwnedIRQs);
        if (readAddress(MEBase)!=p.pm4 || readAddress(PFPBase)!=p.pfp || readAddress(RBBase)!=va ||
            r.read(RBControl)!=RingControl || r.read(RBRead) || r.read(RBWrite) ||
            r.read(ContextSwitch) || get(CP_ADDR_MODE_CNTL)!=1 || get(CP_PROTECT_CNTL)!=7) return false;
        r.publish(); r.write(VBIFHalt,0); r.write(MEControl,0);
        put(packet7(0x48,8)); // CP_ME_INIT, A540 payload (no A530 workaround)
        const uint32_t init[] = {0x2f,3,0x20000000,0,0,0,0,0};
        for (auto value : init) put(value);
        return kick();
    }
    bool gpmu(const FirmwarePower &power, uint64_t commandsVA, uint32_t words) {
        if (!owned || !ring || cursor != 9 || !power.valid() || !words || words > 0x1021 ||
            (commandsVA&3) || !deviceRange(commandsVA,uint64_t(words)*4) ||
            get(GPMU_CM3_SYSRESET) != 1 || r.read(GPMUReady) || r.read(GPMUError)) return false;
        set(GPMU_GPMU_VOLTAGE,0x80000000); set(GPMU_GPMU_PWR_THRESHOLD,0x80000000|6000);
        // AGC shared-register protocol, not an inferred operating voltage.
        constexpr uint32_t msg = 0x9800+7, payload = msg+5;
        for (unsigned i = 0; i < 36; ++i) set(payload+i,0);
        set(msg,0x80000001); set(msg+1,1);
        set(payload,5448); set(payload+1,1);
        set(payload+2,power.microvolts/1000); set(payload+3,power.frequency/1000000);
        set(payload+34,(1u<<24)|(1u<<17)|(2u<<8)); // A540 patch 1, BCL/throttling disabled
        set(payload+35,~0x303u); set(msg+3,36*4); set(0x9800+5,0xbabeface);
        set(GPMU_PWR_COL_INTER_FRAME_CTRL,0x7f); set(GPMU_PWR_COL_BINNING_CTRL,0);
        set(GPMU_PWR_COL_INTER_FRAME_HYST,0xa0080); set(GPMU_PWR_COL_STAGGER_DELAY,0x600040);
        put(packet7(0x5f,1)); put(0); // CP_SET_PROTECTED_MODE
        put(packet7(0x3f,3)); put(uint32_t(commandsVA)); put(uint32_t(commandsVA>>32)); put(words);
        put(packet7(0x5f,1)); put(1);
        if (!kick()) return false;
        set(GPMU_CM3_SYSRESET,0);
        for (unsigned elapsed = 0; elapsed < 100000; elapsed += 10) {
            if ((r.read(InterruptStatus)&ErrorIRQs) || (r.read(CPInterruptStatus)&0x3f) || r.read(GPMUError)) return false;
            if (r.read(GPMUReady)==0xbabeface) return idle();
            r.delay(10);
        }
        return false;
    }
    bool stop() {
        if (!owned) return true;
        r.write(InterruptMask,0); r.write(MEControl,Halt); r.write(VBIFHalt,15);
        for (unsigned elapsed = 0; elapsed < 100000; elapsed += 10) {
            if ((r.read(MEControl)&Halt)==Halt && (r.read(VBIFHalted)&15)==15) { owned=false; return true; }
            r.delay(10);
        }
        return false;
    }
};
} // namespace Adreno
