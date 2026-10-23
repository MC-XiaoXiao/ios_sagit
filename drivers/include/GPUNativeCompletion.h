// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "GPUNativeProtocol.h"

namespace GPU { namespace Native {
// IOKit's vendor-specific error space, code0x100. Only the pre-Family
// capacity check returns this value; no token/fence has been consumed.
constexpr uint32_t AdmissionBackpressureVersion=1,AdmissionDeferredStatus=0xe3ff8100u;
constexpr uint64_t FramebufferSnapshotLimit=64ULL*1024*1024,FramebufferIndirectLimit=64ULL*1024*1024,
    IndexSnapshotLimit=16ULL*1024*1024;
struct AdmissionMemory {uint64_t snapshotBytes=0,indirectBytes=0,indexBytes=0;};
// Indices share one private allocation per job. Each draw needs only a
// naturally aligned address; page padding belongs to the complete allocation.
inline bool appendIndexSnapshot(uint64_t bytes,uint64_t &cursor,uint32_t &offset) {
    if(!bytes || bytes>IndexSnapshotLimit)return false;
    const uint64_t span=(bytes+15)&~uint64_t(15);
    if(span>IndexSnapshotLimit-cursor)return false;
    offset=uint32_t(cursor);cursor+=span;return true;
}
inline uint64_t indexSnapshotAllocation(uint64_t bytes) {
    return (bytes+PageBytes-1)&~uint64_t(PageBytes-1);
}
inline bool admissionFits(uint64_t live,uint64_t requested,uint64_t limit) {
    return live<=limit && requested<=limit-live;
}
inline bool pipelinedAdmissionBlocked(uint32_t pending,bool cpuReadHazard,const AdmissionMemory &request,const AdmissionMemory &live) {
    return pending>=MaxJobs || cpuReadHazard ||
        !admissionFits(live.snapshotBytes,request.snapshotBytes,FramebufferSnapshotLimit) ||
        !admissionFits(live.indirectBytes,request.indirectBytes,FramebufferIndirectLimit) ||
        !admissionFits(live.indexBytes,request.indexBytes,IndexSnapshotLimit);
}
// The caller checks actual retained physical aliases, across every live job.
// A CPU read/flush cannot precede a GPU writer, even through a separate root
// descriptor. Read/read sharing is allowed only when every root pending-use
// reference is accounted for. Unknown/transient uses fail closed.
class CPUReadHazards {
    uint32_t active_,accounted_;
    bool writer_=false,invalid_=false;
public:
    CPUReadHazards(uint32_t active,uint32_t own):active_(active),accounted_(own),invalid_(own>active) {}
    void use(bool sameBacking,bool aliasesWriter) {
        writer_|=aliasesWriter;
        if(sameBacking){if(accounted_==UINT32_MAX)invalid_=true;else ++accounted_;}
    }
    bool blocked() const {return writer_ || invalid_ || active_!=accounted_;}
};
inline bool admissionBlocked(uint32_t pending,bool indexed,bool framebuffer,uint64_t snapshotBytes,uint64_t ibBytes) {
    return pending>=MaxJobs || (indexed && pending) || (framebuffer && (snapshotBytes || ibBytes));
}
// One consumer per actual Family notification queue. Admission and start
// notifications never retire a job; only the matching FIFO end record does.
class CompletionWindow {
    uint64_t ends_[MaxJobs]{};
    uint64_t issued_=0,retired_=0;
    uint32_t head_=0,count_=0;
    bool failed_=false;
public:
    enum class Observation { Start, Retired, Failed, Malformed };
    uint32_t pending() const {return count_;}
    uint64_t issued() const {return issued_;}
    uint64_t retired() const {return retired_;}
    bool failed() const {return failed_;}
    bool full() const {return count_==MaxJobs;}
    bool admit(uint64_t start,uint64_t end) {
        if(failed_ || full() || !start || start<=issued_ || start==UINT64_MAX || end!=start+1 || end==UINT64_MAX)return false;
        ends_[(head_+count_)%MaxJobs]=end;++count_;issued_=end;return true;
    }
    uint32_t abort() {failed_=true;auto count=count_;count_=0;return count;}
    Observation observe(const Notification &event) {
        auto malformed=[&](){failed_=true;return Observation::Malformed;};
        if(failed_ || !event.token || event.reserved)return malformed();
        if(event.status){failed_=true;return Observation::Failed;}
        // The original Family includes the CPU admission interval in start
        // notifications too. Classify opaque tokens from actual admission,
        // never infer GPU completion from the presence of a timestamp.
        for(uint32_t i=0;i<count_;++i)if(ends_[(head_+i)%MaxJobs]-1==event.token)return Observation::Start;
        if(!count_ || event.token!=ends_[head_] || !event.startTime || !event.endTime || event.endTime<event.startTime)return malformed();
        retired_=event.token;ends_[head_]=0;head_=(head_+1)%MaxJobs;--count_;return Observation::Retired;
    }
};
} }
