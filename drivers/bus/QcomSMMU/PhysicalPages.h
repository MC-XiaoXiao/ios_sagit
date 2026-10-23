// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <QcomPowerHardware.h>
namespace SMMU {
// Split prepared kernel VM pages (4/16/64 KiB) into the SMMU's 4 KiB
// granule. API supplies either a prepared UPL or real physical MD segments.
template<class API> class PhysicalPages {
    API &api;
    uint64_t offset=0,length=0;
    unsigned shift=0,bias=0;
    bool valid=false;
public:
    explicit PhysicalPages(API &source):api(source) {}
    unsigned pageOffset() const {return bias;}
    QcomPower::Status initialize(uint64_t start,uint64_t bytes,unsigned vmPageShift) {
        using namespace QcomPower;
        if (valid || !bytes || bytes>16*1024*1024 || vmPageShift<12 || vmPageShift>16 || start>UINT64_MAX-bytes) return Invalid;
        offset=start;length=bytes;shift=vmPageShift;uint64_t first=0;
        if (api.hasPageList()) {
            const auto count=api.pageCount(),skip=api.pageOffset();
            if (start || !count || skip>=(1ULL<<shift) || count>(UINT64_MAX>>shift) || bytes>(count<<shift)-skip) return Invalid;
            const auto frame=api.pageFrame(0);
            if (!frame || frame>(UINT64_MAX>>shift)) return Invalid;
            first=(frame<<shift)+skip;
        } else {
            uint64_t span=0;first=api.segment(start,span);
            if (!first || span<(bytes<4096-(first&4095) ? bytes:4096-(first&4095))) return Invalid;
        }
        bias=first&4095;valid=true;return OK;
    }
    QcomPower::Status page(unsigned index,uint64_t &physical) {
        using namespace QcomPower;
        physical=0;
        if (!valid || uint64_t(index)*4096>=((length+bias+4095)&~4095ULL)) return Invalid;
        if (api.hasPageList()) {
            const auto position=api.pageOffset()+uint64_t(index)*4096-bias;
            const auto cpuPage=position>>shift;
            if (cpuPage>=api.pageCount()) return Invalid;
            const auto frame=api.pageFrame(unsigned(cpuPage));
            if (!frame || frame>(UINT64_MAX>>shift)) return Invalid;
            physical=(frame<<shift)+(position&((1ULL<<shift)-1));
            return physical && !(physical&4095) ? OK:Invalid;
        }
        const auto relative=uint64_t(index)*4096;uint64_t span=0;
        if (relative<bias) {
            physical=api.segment(offset,span)&~4095ULL;
            const auto needed=length<4096-bias ? length:4096-bias;
            return physical && span>=needed ? OK:Invalid;
        }
        const auto start=relative-bias;if (start>=length) return Invalid;
        physical=api.segment(offset+start,span);
        return physical && !(physical&4095) && span>=(length-start<4096 ? length-start:4096) ? OK:Invalid;
    }
};
}
