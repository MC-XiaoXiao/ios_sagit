// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include "WLANDMA.h"

namespace WLAN {
static_assert(sizeof(IOMapper) == 208 && sizeof(IODMACommand) == 120,"iOS 18 mapper/DMA ABI");
inline Result ioResult(IOReturn result) {
    switch (result) {
    case kIOReturnSuccess: return Result::Success;
    case kIOReturnBadArgument: case kIOReturnNotAligned: return Result::Invalid;
    case kIOReturnNoMemory: case kIOReturnNoResources: case kIOReturnNotReady: return Result::NotReady;
    case kIOReturnBusy: return Result::Busy;
    case kIOReturnUnsupported: return Result::Unsupported;
    case kIOReturnTimeout: return Result::Timeout;
    default: return Result::Fault;
    }
}
class IOMemoryAPI {
    IOMapper *mapper = nullptr; // retained by the platform owner
    using Isolation = Result (*)(void *,uint64_t);
    Isolation isolation = nullptr;
    void *context = nullptr;
public:
    using Memory = IOBufferMemoryDescriptor;
    struct Command {
        IODMACommand *object = nullptr;
        bool attempted = false, prepared = false, completed = false, poisoned = false;
    };
    IOMemoryAPI(IOMapper *deviceMapper,Isolation check,void *owner) :
        mapper(deviceMapper),isolation(check),context(owner) {}
    bool ready() const { return mapper && isolation && mapper->getPageSize() == 4096; }
    Memory *allocate(uint32_t length,unsigned alignment) {
        // MapperNone is a getPhysicalSegment flag, not an allocation/cache
        // option. Keep CPU storage explicitly uncached on noncoherent SNOC.
        return ready() ? IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
            kIODirectionInOut | kIOMemoryHostPhysicallyContiguous | kIOMapInhibitCache,length,alignment) : nullptr;
    }
    Result prepare(Memory *memory) { return ioResult(memory->prepare(kIODirectionInOut)); }
    Result complete(Memory *memory) { return ioResult(memory->complete(kIODirectionInOut)); }
    void *bytes(Memory *memory) { return memory->getBytesNoCopy(); }
    void releaseMemory(Memory *memory) { memory->release(); }
    Command *createCommand(uint32_t bytes,unsigned alignment) {
        if (!ready()) return nullptr;
        auto *command = new Command;
        if (!command) return nullptr;
        command->object = IODMACommand::withSpecification(IODMACommand::OutputHost64,
            DMAAddressBits,bytes,IODMACommand::kMapped,bytes,alignment,mapper);
        if (!command->object) { delete command; return nullptr; }
        return command;
    }
    Result bind(Command *command,Memory *memory) {
        auto result = ioResult(command->object->setMemoryDescriptor(memory,false));
        if (result != Result::Success) return result;
        command->attempted = true;
        result = ioResult(command->object->prepare());
        command->prepared = result == Result::Success;
        command->poisoned = result == Result::Fault || result == Result::Timeout;
        return result;
    }
    Result range(Command *command,uint64_t &address,uint64_t &length) {
        if (!command->prepared || command->poisoned) return Result::NotReady;
        IODMACommand::Segment64 segment{}; UInt64 offset = 0; UInt32 count = 1;
        const auto result = ioResult(command->object->gen64IOVMSegments(&offset,&segment,&count));
        if (result != Result::Success) return result;
        if (count != 1 || offset != segment.fLength) return Result::Invalid;
        address = segment.fIOVMAddr; length = segment.fLength; return Result::Success;
    }
    Result clear(Command *command) {
        if (command->poisoned) return Result::Fault;
        if (command->attempted && !command->completed) {
            const auto result = ioResult(command->object->complete());
            if (result != Result::Success && !(result == Result::NotReady && !command->prepared)) {
                // Native complete consumes its preparation even on an unmap
                // error. Retrying it cannot prove retirement of that mapping.
                command->poisoned = true; return result;
            }
            command->completed = true;
        }
        return ioResult(command->object->clearMemoryDescriptor(false));
    }
    void releaseCommand(Command *command) { command->object->release(); delete command; }
    Result isolated(uint64_t generation) { return isolation ? isolation(context,generation) : Result::NotReady; }
    void publishBarrier() { __asm__ volatile("dsb oshst" ::: "memory"); }
    void consumeBarrier() { __asm__ volatile("dsb oshld" ::: "memory"); }
};
}
