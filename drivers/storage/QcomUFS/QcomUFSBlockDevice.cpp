/* SPDX-License-Identifier: BSD-2-Clause */
#include "QcomUFSBlockDevice.h"
#include "QcomUFS.h"
#include <IOKit/IOKitKeys.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>

OSDefineMetaClassAndStructors(QcomUFSBlockDevice, IOBlockStorageDevice);
bool QcomUFSBlockDevice::initLUN(QcomUFS *host, uint8_t number) {
    if (!IOBlockStorageDevice::init(nullptr)) return false;
    controller = host; lun = number; reportedPresent = true;
    setName("UFS Logical Unit"); setProperty("UFS LUN", lun, 32);
    setProperty("Physical Block Size", controller->blockSize(lun), 64);
    setProperty(kIOMaximumBlockCountReadKey, UFS::MaxTransfer / controller->blockSize(lun), 64);
    setProperty(kIOMaximumBlockCountWriteKey, UFS::MaxTransfer / controller->blockSize(lun), 64);
    setProperty(kIOMaximumByteCountReadKey, UFS::MaxTransfer, 64);
    setProperty(kIOMaximumByteCountWriteKey, UFS::MaxTransfer, 64);
    setProperty(kIOMaximumSegmentCountReadKey, UFS::MaxPRDs, 64);
    setProperty(kIOMaximumSegmentCountWriteKey, UFS::MaxPRDs, 64);
    setProperty(kIOMaximumSegmentByteCountReadKey, UFS::MaxPRDBytes, 64);
    setProperty(kIOMaximumSegmentByteCountWriteKey, UFS::MaxPRDBytes, 64);
    setProperty(kIOMinimumSegmentAlignmentByteCountKey, 64, 64);
    OSSharedPtr<OSDictionary> protocol(OSDictionary::withCapacity(2), OSNoRetain);
    OSSharedPtr<OSDictionary> medium(OSDictionary::withCapacity(1), OSNoRetain);
    OSSharedPtr<OSString> location(OSString::withCString("Internal"), OSNoRetain);
    OSSharedPtr<OSString> transport(OSString::withCString("UFS"), OSNoRetain);
    OSSharedPtr<OSString> solidState(OSString::withCString("Solid State"), OSNoRetain);
    if (!protocol || !medium || !location || !transport || !solidState) return false;
    protocol->setObject("Physical Interconnect", transport.get());
    protocol->setObject("Physical Interconnect Location", location.get());
    medium->setObject("Medium Type", solidState.get());
    return setProperty("Protocol Characteristics", protocol.get()) &&
        setProperty("Device Characteristics", medium.get());
}
IOReturn QcomUFSBlockDevice::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
    IOStorageAttributes *attributes, IOStorageCompletion *completion) {
    return controller->readWrite(lun, buffer, block, count, attributes, completion);
}
IOReturn QcomUFSBlockDevice::doSynchronize(UInt64 block, UInt64 count, IOStorageSynchronizeOptions options) {
    if (options) return kIOReturnUnsupported;
    if (block >= controller->blocks(lun) || count > controller->blocks(lun) - block) return kIOReturnBadArgument;
    return controller->synchronize(lun);
}
IOReturn QcomUFSBlockDevice::doEjectMedia() { return kIOReturnUnsupported; }
IOReturn QcomUFSBlockDevice::doFormatMedia(UInt64) { return kIOReturnUnsupported; }
UInt32 QcomUFSBlockDevice::doGetFormatCapacities(UInt64 *capacities, UInt32 max) const {
    if (capacities && max) capacities[0] = controller->blocks(lun) * controller->blockSize(lun);
    return capacities && !max ? 0 : 1;
}
char *QcomUFSBlockDevice::getVendorString() { return const_cast<char *>("JEDEC"); }
char *QcomUFSBlockDevice::getProductString() { return const_cast<char *>("UFS Storage"); }
char *QcomUFSBlockDevice::getRevisionString() { return const_cast<char *>("1.0"); }
char *QcomUFSBlockDevice::getAdditionalDeviceInfoString() { return const_cast<char *>("UFSHCI 2.0"); }
IOReturn QcomUFSBlockDevice::reportBlockSize(UInt64 *value) {
    if (!value) return kIOReturnBadArgument; *value = controller->blockSize(lun); return kIOReturnSuccess;
}
IOReturn QcomUFSBlockDevice::reportMaxValidBlock(UInt64 *value) {
    if (!value) return kIOReturnBadArgument; *value = controller->blocks(lun) - 1; return kIOReturnSuccess;
}
IOReturn QcomUFSBlockDevice::reportMediaState(bool *present, bool *changed) {
    if (!present) return kIOReturnBadArgument; *present = controller->mediaPresent(lun);
    bool previous = __atomic_exchange_n(&reportedPresent, *present, __ATOMIC_RELAXED);
    if (changed) *changed = previous != *present; return kIOReturnSuccess;
}
IOReturn QcomUFSBlockDevice::reportRemovability(bool *value) {
    if (!value) return kIOReturnBadArgument; *value = false; return kIOReturnSuccess;
}
IOReturn QcomUFSBlockDevice::reportEjectability(bool *value) { return reportRemovability(value); }
#if TARGET_OS_OSX
IOReturn QcomUFSBlockDevice::reportPollRequirements(bool *required, bool *expensive) {
    if (!required || !expensive) return kIOReturnBadArgument;
    *required = *expensive = false; return kIOReturnSuccess;
}
#endif
IOReturn QcomUFSBlockDevice::reportWriteProtection(bool *value) {
    if (!value) return kIOReturnBadArgument; *value = controller->writeProtected(lun); return kIOReturnSuccess;
}
