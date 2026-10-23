// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>

namespace MetalSync {
// The caller serializes access. A point belongs to one encoded update,
// rather than a monotonic maximum which could hide an unfinished older update.
class Point {
    bool done_ = false;
    uint32_t error_ = 0;
    uint64_t queue_ = 0, token_ = 0;
public:
    bool pending() const { return !done_; }
    uint32_t error() const { return error_; }
    uint64_t queue() const { return queue_; }
    uint64_t token() const { return token_; }
    bool admit(uint64_t queue, uint64_t token) {
        if(done_ || queue_ || !queue || !token)return false;
        queue_=queue;token_=token;return true;
    }
    bool ordered(uint64_t queue) const { return !done_ && queue_==queue && token_; }
    bool finish(uint32_t error, uint64_t queue = 0, uint64_t token = 0) {
        if (done_ || (!error && (!queue || !token))) return false;
        error_ = error; queue_ = queue; token_ = token; done_ = true;
        return true;
    }
};
struct Observation { bool pending; uint32_t error; };
enum class DependencyState { Ready, Pending, Failed };
struct Dependencies { DependencyState state; uint32_t error; };
template<class Read> Dependencies dependencies(uint64_t count, Read read) {
    bool pending=false;
    for(uint64_t i=0;i<count;++i) {
        Observation point=read(i);
        if(!point.pending && point.error)return {DependencyState::Failed,point.error};
        pending|=point.pending;
    }
    return {pending?DependencyState::Pending:DependencyState::Ready,0};
}
inline bool renderStages(uint64_t stages) { return stages && !(stages & ~uint64_t(3)); }
inline bool barrierScope(uint64_t scope) { return scope && !(scope & ~uint64_t(3)); }
}
