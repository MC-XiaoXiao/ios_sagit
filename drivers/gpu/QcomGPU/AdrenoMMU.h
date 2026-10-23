// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#include <QcomPowerHardware.h>
#include "../../bus/QcomSMMU/RetainedPageTables.h"
namespace Adreno {
using MMUStatus = QcomPower::Status;
using namespace QcomPower;
// CP's table update may briefly inhibit TTBR0 walks. Require the exact
// original TCR again before accepting the context; never mask EPD0 or any
// other mismatch. A stuck inhibit bit remains a bounded validation failure.
template<class IO> bool settledTranslationControl(IO &io,uint32_t reg,uint32_t expected,uint32_t &observed) {
    constexpr uint32_t EPD0=1u<<7;
    for(unsigned attempt=0;attempt<8;++attempt) {
        observed=io.read(reg);
        if(observed==expected)return true;
        if((expected&EPD0) || observed!=(expected|EPD0) || attempt==7)return false;
        io.delay(1);
    }
    return false;
}
struct SMMUContract {
    enum : uint32_t { ExclusiveContext=1,BootTablesReserved=2,TablesPublished=4,LifetimeResourcesPinned=8,ProducerStopped=16,Required=31 };
    uint32_t version,flags,bank,asid,inputBits,outputBits,streamCount,reserved;
    uint64_t ttbr,first,end;
    struct Stream { uint32_t slot,id; } streams[8];
    bool valid() const {
        if (version!=1 || flags!=Required || bank!=0 || asid>65535 || inputBits<32 || inputBits>40 ||
            (outputBits!=32 && outputBits!=36 && outputBits!=40) || !streamCount || streamCount>8 || reserved ||
            !ttbr || ((ttbr>>48)!=asid) || (ttbr&4095) || !(ttbr&0x0000fffffffff000ULL) ||
            (ttbr&0x0000fffffffff000ULL)>=(1ULL<<outputBits) ||
            first<0x1000000 || (first&4095) || (end&4095) || end<=first ||
            end>(1ULL<<inputBits) || end-first<0x800000 || end-first>0x20000000) return false;
        for (unsigned i=0;i<8;++i) {
            if (i>=streamCount) { if (streams[i].slot || streams[i].id) return false;continue; }
            if (streams[i].slot>=128 || streams[i].id>32767) return false;
            for (unsigned j=0;j<i;++j) if (streams[i].slot==streams[j].slot || streams[i].id==streams[j].id) return false;
        }
        return true;
    }
};
static_assert(sizeof(SMMUContract)==120,"Adreno retained SMMU handoff ABI");
using MMUTablePage=SMMU::RetainedTablePage;
template<class API,unsigned Capacity=256> using RetainedPageTables=SMMU::RetainedPageTables<API,Capacity>;
}
