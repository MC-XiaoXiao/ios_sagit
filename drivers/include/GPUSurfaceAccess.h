// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>

namespace GPU { namespace Native {
enum class SurfaceAccessState : uint8_t { Ready, Pending, Failed };
struct SurfaceAccessObservation {
    SurfaceAccessState state=SurfaceAccessState::Pending;
    uint32_t status=0;
};
// A completed error is terminal even if no ready callback arrives. Native
// status0 completion can precede its ready callback, and the native special
// surface policy can also return immediate ready with status0 completion.
// Neither completion0 alone nor callback absence grants access.
inline SurfaceAccessObservation surfaceAccessObservation(bool ready,bool completed,uint32_t status) {
    const bool failed=completed && status!=0;
    return {failed?SurfaceAccessState::Failed:ready?SurfaceAccessState::Ready:SurfaceAccessState::Pending,
        failed?status:0};
}
template<class Read> SurfaceAccessObservation surfaceAccesses(unsigned count,const Read &read) {
    SurfaceAccessObservation result{SurfaceAccessState::Ready,0};
    for(unsigned i=0;i<count;++i) {
        const auto next=read(i);
        if(next.state==SurfaceAccessState::Failed)return next;
        if(next.state==SurfaceAccessState::Pending)result=next;
    }
    return result;
}
} }
