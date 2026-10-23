/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _QCOM_UFS_BLOCK_DEVICE_H
#define _QCOM_UFS_BLOCK_DEVICE_H
#include <IOKit/storage/IOBlockStorageDevice.h>
static_assert(sizeof(IOBlockStorageDevice) == 144, "iOS 18 IOStorageFamily base ABI");
class QcomUFS;
class QcomUFSBlockDevice : public IOBlockStorageDevice {
    OSDeclareDefaultStructors(QcomUFSBlockDevice);
public:
    bool initLUN(QcomUFS *controller, uint8_t lun);
    IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64,
        IOStorageAttributes *, IOStorageCompletion *) APPLE_KEXT_OVERRIDE;
    IOReturn doSynchronize(UInt64, UInt64, IOStorageSynchronizeOptions) APPLE_KEXT_OVERRIDE;
    IOReturn doEjectMedia() APPLE_KEXT_OVERRIDE;
    IOReturn doFormatMedia(UInt64) APPLE_KEXT_OVERRIDE;
    UInt32 doGetFormatCapacities(UInt64 *, UInt32) const APPLE_KEXT_OVERRIDE;
    char *getVendorString() APPLE_KEXT_OVERRIDE;
    char *getProductString() APPLE_KEXT_OVERRIDE;
    char *getRevisionString() APPLE_KEXT_OVERRIDE;
    char *getAdditionalDeviceInfoString() APPLE_KEXT_OVERRIDE;
    IOReturn reportBlockSize(UInt64 *) APPLE_KEXT_OVERRIDE;
    IOReturn reportMaxValidBlock(UInt64 *) APPLE_KEXT_OVERRIDE;
    IOReturn reportMediaState(bool *, bool *) APPLE_KEXT_OVERRIDE;
    IOReturn reportRemovability(bool *) APPLE_KEXT_OVERRIDE;
    IOReturn reportEjectability(bool *) APPLE_KEXT_OVERRIDE;
#if TARGET_OS_OSX
    IOReturn reportPollRequirements(bool *, bool *) APPLE_KEXT_OVERRIDE;
#endif
    IOReturn reportWriteProtection(bool *) APPLE_KEXT_OVERRIDE;
private:
    QcomUFS *controller;
    uint8_t lun;
    bool reportedPresent;
};
#endif
