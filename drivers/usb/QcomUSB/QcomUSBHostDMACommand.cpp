// SPDX-License-Identifier: BSD-2-Clause
#include "QcomUSBHostDMACommand.h"

OSDefineMetaClassAndStructors(QcomUSBHostDMACommand,AppleUSBHostDMACommand);

QcomUSBHostDMACommand *QcomUSBHostDMACommand::withClientSpecification(UInt64 maxSegment,UInt64 maxTransfer,void *refCon) {
    if (!maxSegment) return nullptr;
    auto *command=new QcomUSBHostDMACommand;
    if (command && !command->initWithSpecification(OutputHost64,64,maxSegment,kUnmapped,maxTransfer,1,nullptr,refCon)) {
        command->release();return nullptr;
    }
    return command;
}
IODMACommand *QcomUSBHostDMACommand::cloneCommand(void *refCon) {
    // Read the actual specification, including changes retained by the base
    // after a failed prepare. A separate success-only cache would be stale.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
    static_assert(__builtin_offsetof(QcomUSBHostDMACommand,fMaxSegmentSize)==40,"native DMA segment limit offset");
    static_assert(__builtin_offsetof(QcomUSBHostDMACommand,fMaxTransferSize)==48,"native DMA transfer limit offset");
#pragma clang diagnostic pop
    return withClientSpecification(fMaxSegmentSize,fMaxTransferSize,refCon);
}
bool QcomUSBHostDMACommand::initWithSpecification(SegmentFunction output,UInt8,UInt64 segment,MappingOptions,
    UInt64 transfer,UInt32,IOMapper *,void *refCon) {
    // Segment layout is part of the caller's output buffer ABI. Reject a
    // 32-bit/custom output function rather than writing 64-bit segments there.
    if (output!=OutputHost64 || !segment || getMemoryDescriptor()) return false;
    return AppleUSBHostDMACommand::initWithSpecification(
        OutputHost64,64,segment,kUnmapped,transfer,1,nullptr,refCon);
}
bool QcomUSBHostDMACommand::initWithSpecification(SegmentFunction output,const SegmentOptions *options,
    UInt32,IOMapper *,void *refCon) {
    if (output!=OutputHost64 || !options || options->fStructSize!=sizeof(SegmentOptions) || !options->fMaxSegmentSize || getMemoryDescriptor())
        return false;
    auto client=*options;client.fNumAddressBits=64;
    client.fAlignment=client.fAlignmentLength=client.fAlignmentInternalSegments=1;
    return IODMACommand::initWithSpecification(OutputHost64,&client,kUnmapped,nullptr,refCon);
}
IOReturn QcomUSBHostDMACommand::setMemoryDescriptor(const IOMemoryDescriptor *memory,bool autoPrepare) {
    return IODMACommand::setMemoryDescriptor(memory,autoPrepare);
}
IOReturn QcomUSBHostDMACommand::clearMemoryDescriptor(bool autoComplete) {
    return IODMACommand::clearMemoryDescriptor(autoComplete);
}
const IOMemoryDescriptor *QcomUSBHostDMACommand::getMemoryDescriptor() const {
    return IODMACommand::getMemoryDescriptor();
}
IOReturn QcomUSBHostDMACommand::nativeSlot31(const IOMemoryDescriptor *memory,bool autoPrepare,UInt32) {
    // Native Host setter can replace an irregular MD by another allocation.
    // Client memory is never submitted to this HCD's hardware, so retain the
    // actual client MD and its ordinary IODMACommand preparation instead.
    return IODMACommand::setMemoryDescriptor(memory,autoPrepare);
}
IOReturn QcomUSBHostDMACommand::prepare(UInt64 offset,UInt64 length,bool,bool) {
    return IODMACommand::prepare(offset,length,false,false);
}
IOReturn QcomUSBHostDMACommand::complete(bool,bool) {
    return IODMACommand::complete(false,false);
}
IOReturn QcomUSBHostDMACommand::synchronize(IOOptionBits options) {
    if ((options&kIODirectionOutIn)==kIODirectionOutIn) return kIOReturnBadArgument;
    UInt64 offset=0,length=0;
    return getPreparedOffsetAndLength(&offset,&length);
}
IOReturn QcomUSBHostDMACommand::prepareWithSpecification(SegmentFunction output,UInt8,UInt64 segment,
    MappingOptions,UInt64 transfer,UInt32,IOMapper *,UInt64 offset,UInt64 length,bool,bool) {
    if (output!=OutputHost64 || !segment) return kIOReturnUnsupported;
    return IODMACommand::prepareWithSpecification(OutputHost64,64,segment,kUnmapped,transfer,1,nullptr,
        offset,length,false,false);
}
IOReturn QcomUSBHostDMACommand::prepareWithSpecification(SegmentFunction output,const SegmentOptions *options,
    UInt32,IOMapper *,UInt64 offset,UInt64 length,bool,bool) {
    if (output!=OutputHost64 || !options || options->fStructSize!=sizeof(SegmentOptions) || !options->fMaxSegmentSize)
        return kIOReturnUnsupported;
    auto client=*options;client.fNumAddressBits=64;
    client.fAlignment=client.fAlignmentLength=client.fAlignmentInternalSegments=1;
    return IODMACommand::prepareWithSpecification(OutputHost64,&client,kUnmapped,nullptr,
        offset,length,false,false);
}
