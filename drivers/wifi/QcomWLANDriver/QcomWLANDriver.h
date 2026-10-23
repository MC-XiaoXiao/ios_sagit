// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <DriverKit/IOService.h>
static_assert(sizeof(OSObject) == 40 && sizeof(IOService) == 48,"DriverKit 24.6 public object ABI");
static_assert(sizeof(OSClassLoadInformation) == 160 && sizeof(OSClassDescription) == 312,
    "Apple public class-load ABI");

// Only inherited Apple RPCs are overridden. The registration in RPC.cpp uses
// the public OSClassLoadInformation format; it introduces no private RPC ID.
class QcomWLANDriver : public IOService {
public:
    using super = IOService;
    kern_return_t Dispatch(IORPC) override;
    static kern_return_t _Dispatch(QcomWLANDriver *, IORPC);
    kern_return_t Start_Impl(IOService *);
    kern_return_t Stop_Impl(IOService *);
};

class QcomWLANDriverMetaClass : public OSMetaClass {
public:
    kern_return_t New(OSObject *) override;
};
