// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "PageTables.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOBufferAllocation.h>
namespace SMMU {
// The context owner supplies actual completed ASID invalidation and root
// detachment proofs. These physically contiguous, nonpageable buffers have no
// DMA mapper of their own. Their descriptor retain keeps the pages resident.
template<class Context> class IOTableMemory {
    Context &context;
    unsigned physicalBits;
public:
    IOTableMemory(Context &owner,unsigned bits):context(owner),physicalBits(bits) {}
    Status allocate(TablePage &page) {
        if (physicalBits<32 || physicalBits>40) return Invalid;
        const auto mask=((1ULL<<physicalBits)-1)&~4095ULL;
        auto *memory=IOKitMemory::allocateBuffer(IOKitMemory::bufferOptions(false,true),4096,4096,mask);
        if (!memory) return NotReady;
        IOByteCount span=0;
        const auto physical=memory->getPhysicalSegment(0,&span,kIOMemoryMapperNone);
        auto *bytes=memory->getBytesNoCopy();
        if (!bytes || memory->getLength()!=4096 || !physical || (physical&4095) || span<4096 ||
            physical>mask || (reinterpret_cast<uintptr_t>(bytes)&7)) { memory->release(); return Failed; }
        page={memory,static_cast<volatile uint64_t *>(bytes),physical}; return OK;
    }
    void release(TablePage &page) {
        if (page.owner) static_cast<IOBufferMemoryDescriptor *>(page.owner)->release();
        page={};
    }
    void barrier() { __asm__ volatile("dsb oshst" ::: "memory"); }
    Status sync() { return context.sync(); }
    Status isolated() { return context.rootDetached(); }
};
}
