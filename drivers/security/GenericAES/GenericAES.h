// SPDX-License-Identifier: BSD-2-Clause
#include <IOKit/crypto/IOAESAccelerator.h>
class GenericAES : public IOAESAccelerator {
    OSDeclareDefaultStructors(GenericAES)
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn performAES(IOMemoryDescriptor *, IOMemoryDescriptor *, IOByteCount,
        void *, UInt32, IOAESKey *, void *, void *, void *, void *) APPLE_KEXT_OVERRIDE;
};
