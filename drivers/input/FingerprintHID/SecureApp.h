// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include "HomeProtocol.h"
#include "SCMTransport.h"

// Private, serialized, kernel-only QSEE session. No arbitrary SMC user client.
// Request, response and modified-command payload use real physical addresses.
class HomeSecureApp {
    IOBufferMemoryDescriptor *fMemory=nullptr;
    uint8_t *fBytes=nullptr;
    uint64_t fPhysical=0;
    uint32_t fApp=0, fSequence=0;
    bool fBlocked=false, fLoaded=false, fWhitelist=false, fTrace=false;
    IOService *fSCM=nullptr,*fClient=nullptr;
    using Reply=FingerprintSCM::Reply;
    struct CallContext {
        HomeSecureApp *self;
        uint32_t id,types;
        const uint64_t *args;
        unsigned count;
        Reply reply;
    };
    static IOReturn runSecureCall(void *);
    Reply call(uint32_t id,uint32_t types,const uint64_t *args,unsigned count);
    bool invoke(unsigned payloadSize,unsigned pointerOffset);
public:
    static constexpr unsigned Capacity=0x400000, PayloadOffset=0x1000, ImageOffset=0x40000;
    bool initialize(IOService *);
    bool load(const char *name,OSData *image);
    bool unload();
    void release();
    bool blocked() const { return fBlocked; }
    uint8_t *payload() { return fBytes ? fBytes+PayloadOffset : nullptr; }
    bool fpc(uint32_t group,uint32_t command,int32_t &status);
    bool goodix(uint32_t command,unsigned size,uint32_t &status);
};
