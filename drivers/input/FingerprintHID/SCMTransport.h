// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>

namespace FingerprintSCM {
struct Frame { uint64_t x[7]; };
struct Reply { int32_t status; uint32_t result,type,data; };
inline bool retainsBuffers(uint32_t id) {
    return id==0x32000101 || id==0x32000102 || id==0x30000001 || id==0x30000006;
}
// Qualcomm SCM64 has four register arguments; longer calls put argument 3
// onward in physical memory and pass its address in x5. An interrupted call
// resumes with original x1..5 and the returned opaque x6 continuation token.
template<class Backend> Reply call(Backend &backend,uint32_t id,uint32_t types,
    const uint64_t *args,unsigned count,uint64_t *extra,uint64_t extraPhysical,bool &quarantined) {
    Reply reply={-1,0xffffffffU,0,0};
    if (quarantined || count>7 || count!=(types&15) || (count && !args) ||
        (count>4 && (!extra || !extraPhysical))) return reply;
    uint64_t fifth=count>3 ? args[3] : 0;
    if (count>4) {
        for (unsigned i=3;i<count;++i) extra[i-3]=args[i];
        fifth=extraPhysical;
    }
    for (unsigned retry=0;retry<4;++retry) {
        const Frame initial={{uint64_t(id)|0x40000000U,types,count>0 ? args[0] : 0,
            count>1 ? args[1] : 0,count>2 ? args[2] : 0,fifth,0}};
        Frame frame=initial;
        for (unsigned interrupted=0;;++interrupted) {
            backend.invoke(frame);
            if (frame.x[0]!=1) break;
            if (interrupted==1023) { quarantined=true; return {-1,0xffffffffU,0,0}; }
            const uint64_t continuation=frame.x[6];
            frame=initial; frame.x[0]=1; frame.x[6]=continuation;
        }
        reply={int32_t(frame.x[0]),uint32_t(frame.x[1]),uint32_t(frame.x[2]),uint32_t(frame.x[3])};
        if (reply.status!=-12) break; // SCM_V2_EBUSY did not accept the command.
        if (retry!=3) backend.busyDelay();
    }
    if (!reply.status && retainsBuffers(id) && (reply.result==1 || reply.result==2)) quarantined=true;
    return reply;
}
}
