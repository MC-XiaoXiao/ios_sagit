// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "WLANDeviceDMA.h"
#include "WLANRegisterIO.h"
namespace WLAN {
// Internal hardware object, independent of the Apple IOService inheritance.
// The controller serializes calls on its event worker and owns the IRQs.
class DeviceCopyEngine {
protected:
    virtual ~DeviceCopyEngine() = default;
public:
    static DeviceCopyEngine *create(RegisterIO &,DeviceDMA &,unsigned,bool);
    virtual Result configure(uint64_t) = 0;
    virtual Result post(uint64_t,uint64_t,uint64_t,uint32_t,uint16_t) = 0;
    virtual Result completion(uint64_t,uint64_t &,uint32_t &,uint16_t &) = 0;
    virtual Result shutdown(uint64_t) = 0;
    virtual bool ownsHardware() const = 0;
    Result destroy(); // refuses to discard a live/quarantined ring
};
}
