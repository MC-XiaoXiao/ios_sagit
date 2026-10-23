// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMapper.h>
#include "../shared/IOBufferPhysicalMask.h"

namespace IOKitMemory {
// Buffer MD shares bit0x800 with the cache-mode field. MapperNone belongs
// in IOMD/physical-segment operations, never in these allocation options.
constexpr IOOptionBits bufferOptions(bool coherent,bool physical=false) {
    return kIODirectionInOut | (coherent ? kIOMapDefaultCache : kIOMapWriteCombineCache) |
        (physical ? kIOMemoryHostPhysicallyContiguous : 0);
}
inline IOBufferMemoryDescriptor *allocateBuffer(IOOptionBits options,UInt64 bytes,
    UInt64 alignment,UInt64 physicalMask=0) {
    if (!bytes || !alignment || (alignment&(alignment-1)) ||
        (options&kIOMemoryMapperNone) || (reinterpret_cast<uintptr_t>(IOMapper::gSystem)&3)) return nullptr;
    auto *memory=physicalMask ? IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        options|kIOMemoryHostPhysicallyContiguous|MI6IOBuffer::HostPhysicalMask,bytes,physicalMask) :
        IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,options,bytes,alignment);
    if (!memory) return nullptr;
    // Cache settings and physical masks round allocation capacity to the
    // kernel VM page size (16KiB here). DMA commands must still see only the
    // requested logical bytes; retain the whole backing allocation privately.
    if (memory->getLength()<bytes || !memory->getBytesNoCopy()) { memory->release();return nullptr; }
    memory->setLength(bytes);
    if (memory->getLength()!=bytes) { memory->release();return nullptr; }
    return memory;
}
}
