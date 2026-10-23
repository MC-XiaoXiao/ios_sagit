// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include <IOBufferAllocation.h>
#include "USBDMA.h"
#include "DMACommandState.h"
#include "DMARetirement.h"
#include "QcomUSBMapper.h"

namespace USB {
inline Result memoryResult(IOReturn result) {
    if (result == kIOReturnSuccess) return Result::Success;
    if (result == kIOReturnNoMemory || result == kIOReturnNoResources || result == kIOReturnNotReady) return Result::NotReady;
    if (result == kIOReturnTimeout) return Result::Timeout;
    if (result == kIOReturnBusy) return Result::Busy;
    if (result == kIOReturnBadArgument || result == kIOReturnNotAligned) return Result::Invalid;
    if (result == kIOReturnUnsupported) return Result::Unsupported;
    return Result::HardwareError;
}

// The platform retains the mapper and resource lease until every buffer is
// released, including quarantined requests. This binding borrows that mapper.
// It never silently falls back to the system mapper or to physical addressing.
class IOMemoryAPI {
    IOMapper *mapper;
    PlatformContract contract;
public:
    using Memory = IOMemoryDescriptor;
    // Last preparation operation, sampled only when a worker reports an
    // error. Cleanup deliberately does not replace this diagnostic.
    UInt64 preparationDiagnostic[6]{};
    static constexpr bool CompleteFailurePreservesPreparation=false;
    static bool retirementReady(IOMapper *deviceMapper) {
        if (!OSDynamicCast(QcomUSBMapper,deviceMapper)) return false;
        auto *function=OSSymbol::withCStringNoCopy(DMARetirementFunction);
        if (!function) return false;
        DMARetirement query;
        const auto status=deviceMapper->callPlatformFunction(function,false,&query,nullptr,nullptr,nullptr);
        function->release();return status==kIOReturnSuccess;
    }
    struct Command {
        IODMACommand *object=nullptr;
        DMACommandState completion;
    };
    IOMemoryAPI(IOMapper *deviceMapper, PlatformContract resources) : mapper(deviceMapper), contract(resources) {}
    // DMARegion asks whether this API supplies mutually visible CPU/device
    // storage, not whether the SoC snoops CPU caches. A valid resource lease
    // permits either hardware-coherent allocations or explicit uncached ones.
    bool coherent() const { return contract.valid() && (mapped()!=bypass()); }
    bool mapped() const { return mapper != nullptr; }
    bool bypass() const { return contract.flags & PlatformContract::DMABypass; }
    unsigned addressBits() const { return contract.addressBits; }
    Memory *allocate(uint32_t length, unsigned alignment) {
        if (!coherent() || !length || !alignment || (alignment&(alignment-1))) return nullptr;
        // Without an IOMMU, constrain the actual allocation to the device's
        // physical width. This prevents IODMACommand from secretly bouncing
        // long-lived descriptor/event rings to a second allocation.
        const IOOptionBits options = IOKitMemory::bufferOptions(contract.flags&PlatformContract::Coherent);
        if (mapper) return IOKitMemory::allocateBuffer(options,length,alignment);
        uint64_t mask = ((uint64_t(1) << addressBits()) - 1) & ~uint64_t(alignment - 1);
        return IOKitMemory::allocateBuffer(options,length,alignment,mask);
    }
    void retainMemory(Memory *m) { m->retain(); }
    void releaseMemory(Memory *m) { m->release(); }
    uint64_t length(Memory *m) { return m->getLength(); }
    Result prepare(Memory *m, bool writes, bool reads) {
        IODirection direction = IODirection((writes ? kIODirectionIn : 0) | (reads ? kIODirectionOut : 0));
        const auto actual=m->getDirection();
        preparationDiagnostic[0]=1;preparationDiagnostic[2]=m->getLength();
        preparationDiagnostic[3]=actual;preparationDiagnostic[4]=direction;preparationDiagnostic[5]=0;
        const auto result=(actual & direction)!=direction ? kIOReturnBadArgument:m->prepare(direction);
        preparationDiagnostic[1]=UInt32(result);
        return memoryResult(result);
    }
    Result complete(Memory *m) { return memoryResult(m->complete()); }
    void *bytes(Memory *m) { return static_cast<IOBufferMemoryDescriptor *>(m)->getBytesNoCopy(); }
    uint64_t read(Memory *m, void *dst, unsigned size, uint64_t offset=0) { return m->readBytes(offset, dst, size); }
    uint64_t write(Memory *m, const void *src, unsigned size, uint64_t offset=0) { return m->writeBytes(offset, src, size); }
    Command *createCommand(uint32_t bytes, unsigned alignment) {
        if (!coherent() || !bytes || !alignment || (alignment&(alignment-1))) return nullptr;
        // In bypass mode walk real physical pages at 64 bits and check every
        // resulting address ourselves, avoiding a hidden width bounce buffer.
        auto *command=new Command;
        if (!command) return nullptr;
        command->object=IODMACommand::withSpecification(IODMACommand::OutputHost64,
            mapper ? addressBits() : 64, bytes,
            mapper ? IODMACommand::kMapped : IODMACommand::kUnmapped,
            bytes, alignment, mapper);
        if (!command->object) { delete command; return nullptr; }
        return command;
    }
    Result bind(Command *c, Memory *m) {
        // Avoid the native autoPrepare failure path, which itself calls
        // clearMemoryDescriptor(true) and can discard an unmap error.
        const auto result = c->object->setMemoryDescriptor(m,false);
        preparationDiagnostic[0]=2;preparationDiagnostic[1]=UInt32(result);
        preparationDiagnostic[2]=m->getLength();preparationDiagnostic[3]=mapper!=nullptr;
        preparationDiagnostic[4]=preparationDiagnostic[5]=0;
        if (result != kIOReturnSuccess) return memoryResult(result);
        const auto nativePrepared=c->object->prepare();
        preparationDiagnostic[4]=UInt32(nativePrepared);
        const auto prepared=memoryResult(nativePrepared);
        c->completion.prepared(prepared,mapper!=nullptr);
        if (prepared!=Result::Success) return prepared;
        return c->object->getIOMemoryDescriptor() == m ? Result::Success : Result::Unsupported;
    }
    Result range(Command *c, uint64_t &address, uint64_t &size) {
        IODMACommand::Segment64 segment{}; UInt64 offset = 0; UInt32 count = 1;
        auto result = c->object->gen64IOVMSegments(&offset, &segment, &count);
        preparationDiagnostic[0]=3;preparationDiagnostic[1]=UInt32(result);
        preparationDiagnostic[2]=count;preparationDiagnostic[3]=offset;
        preparationDiagnostic[4]=segment.fIOVMAddr;preparationDiagnostic[5]=segment.fLength;
        if (result != kIOReturnSuccess) return memoryResult(result);
        if (count != 1 || offset != segment.fLength) return Result::Invalid;
        address = segment.fIOVMAddr; size = segment.fLength;
        return Result::Success;
    }
    Result clear(Command *c) {
        if (mapper && c->completion.faulted()) {
            if (!OSDynamicCast(QcomUSBMapper,mapper)) return Result::HardwareError;
            const auto recovered=c->completion.recover([&] {
                auto *function=OSSymbol::withCStringNoCopy(DMARetirementFunction);
                if (!function) return Result::NotReady;
                DMARetirement request;request.memory=reinterpret_cast<uintptr_t>(c->object->getIOMemoryDescriptor());
                request.command=reinterpret_cast<uintptr_t>(c->object);
                const auto status=mapper->callPlatformFunction(function,false,&request,nullptr,nullptr,nullptr);
                function->release();return memoryResult(status);
            });
            if (recovered!=Result::Success) return recovered;
        }
        return c->completion.clear([&] { return memoryResult(c->object->complete()); },
            [&] { return memoryResult(c->object->clearMemoryDescriptor(false)); });
    }
    void releaseCommand(Command *c) { c->object->release(); delete c; }
};

}
