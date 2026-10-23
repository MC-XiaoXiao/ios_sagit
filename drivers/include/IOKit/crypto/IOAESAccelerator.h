// SPDX-License-Identifier: BSD-2-Clause
// iOS 18 arm64 ABI, verified by apple-crypto-abi.json. Unnamed slots retain
// their native implementation; this header only names the CBC descriptor API.
#ifndef GENERIC_IOAES_ACCELERATOR_H
#define GENERIC_IOAES_ACCELERATOR_H
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>

class IOCryptoAccelerator : public IOService {
    OSDeclareDefaultStructors(IOCryptoAccelerator)
public:
    virtual const char *getAlgorithmName();
};

struct IOAESKey {
    UInt32 bits;
    UInt8 bytes[32];
    UInt32 selector;
};
static_assert(sizeof(IOAESKey) == 40, "native AES key ABI");

class IOAESAccelerator : public IOCryptoAccelerator {
    OSDeclareDefaultStructors(IOAESAccelerator)
protected:
    UInt8 nativeState[152];
public:
    const char *getAlgorithmName() APPLE_KEXT_OVERRIDE;
    virtual IOReturn nativeSlot169();
    virtual IOReturn performAES(IOMemoryDescriptor *, IOMemoryDescriptor *, IOByteCount,
        void *, UInt32, IOAESKey *, void *, void *, void *, void *);
    virtual UInt32 nativeSlot171();
    virtual UInt32 nativeSlot172();
    virtual IOReturn nativeSlot173();
    virtual IOReturn nativeSlot174();
    virtual IOReturn nativeSlot175();
    virtual IOReturn nativeSlot176();
    virtual IOReturn nativeSlot177();
    virtual IOReturn nativeSlot178();
    virtual IOReturn nativeSlot179();
    virtual IOReturn nativeSlot180();
};
static_assert(sizeof(IOCryptoAccelerator) == 136, "native crypto instance ABI");
static_assert(sizeof(IOAESAccelerator) == 288, "native AES instance ABI");
#endif
