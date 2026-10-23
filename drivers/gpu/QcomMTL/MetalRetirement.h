// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>

namespace MetalRetirement {
// Serialized by the command queue's order lock. Completed buffers may transfer
// ownership only within this budget. An oversized buffer keeps its completion
// and capacity permit pending until its exclusive cleanup finishes; it cannot
// accumulate behind completed buffers or run alongside another cleanup.
class Budget {
public:
    static constexpr unsigned MaxBatches=8;
    static constexpr uint64_t MaxBytes=32*1024*1024;
    enum class Admission {Wait, Deferred, Exclusive};
private:
    unsigned count_=0,peakCount_=0;
    uint64_t bytes_=0,peakBytes_=0;
    bool exclusive_=false;
public:
    Admission acquire(uint64_t bytes) {
        if(exclusive_ || count_==MaxBatches)return Admission::Wait;
        if(bytes>MaxBytes) {
            if(count_)return Admission::Wait;
            exclusive_=true;return Admission::Exclusive;
        }
        if(bytes>MaxBytes-bytes_)return Admission::Wait;
        ++count_;bytes_+=bytes;
        if(count_>peakCount_)peakCount_=count_;
        if(bytes_>peakBytes_)peakBytes_=bytes_;
        return Admission::Deferred;
    }
    bool release(uint64_t bytes,Admission admission) {
        if(admission==Admission::Exclusive) {
            if(!exclusive_ || count_ || bytes<=MaxBytes)return false;
            exclusive_=false;return true;
        }
        if(admission!=Admission::Deferred || exclusive_ || !count_ || bytes>bytes_)return false;
        --count_;bytes_-=bytes;return true;
    }
    unsigned count() const{return count_;}
    unsigned peakCount() const{return peakCount_;}
    uint64_t bytes() const{return bytes_;}
    uint64_t peakBytes() const{return peakBytes_;}
    bool exclusive() const{return exclusive_;}
};
}
