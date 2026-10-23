// SPDX-License-Identifier: BSD-2-Clause
#include "../drivers/gpu/QcomMTL/MetalRetirement.h"
#include <cassert>
#include <cstdio>
#include <stdint.h>
int main() {
    using B=MetalRetirement::Budget;using A=B::Admission;
    B budget;
    for(unsigned i=0;i<B::MaxBatches;++i)assert(budget.acquire(0)==A::Deferred);
    assert(budget.acquire(0)==A::Wait && budget.count()==8);
    assert(budget.acquire(UINT64_MAX)==A::Wait);
    for(unsigned i=0;i<8;++i)assert(budget.release(0,A::Deferred));
    assert(!budget.release(0,A::Deferred));
    assert(budget.acquire(B::MaxBytes-1)==A::Deferred);
    assert(budget.acquire(2)==A::Wait && budget.bytes()==B::MaxBytes-1);
    assert(budget.acquire(1)==A::Deferred);
    assert(budget.acquire(1)==A::Wait && budget.peakBytes()==B::MaxBytes);
    assert(!budget.release(B::MaxBytes+1,A::Deferred));
    assert(budget.release(1,A::Deferred));
    assert(budget.release(B::MaxBytes-1,A::Deferred));
    assert(budget.acquire(UINT64_MAX)==A::Exclusive);
    assert(budget.exclusive() && !budget.bytes() && !budget.count());
    assert(budget.acquire(0)==A::Wait);
    assert(!budget.release(UINT64_MAX,A::Deferred));
    assert(!budget.release(0,A::Exclusive));
    assert(budget.release(UINT64_MAX,A::Exclusive));
    assert(budget.acquire(4096)==A::Deferred && !budget.exclusive());
    assert(budget.release(4096,A::Deferred));
    puts("Metal retirement: count/byte backpressure, overflow-safe charging, exclusive oversized cleanup and recovery passed");
}
