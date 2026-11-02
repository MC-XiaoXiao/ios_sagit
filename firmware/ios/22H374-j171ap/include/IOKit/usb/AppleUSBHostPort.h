// SPDX-License-Identifier: BSD-2-Clause
#pragma once
// Pinned iOS18 arm64 projection. Old IOUSBHostFamily headers have a different
// object layout and different reset/create-device interfaces. Native slots
// without audited argument types preserve dispatch but must not be called.
#include <IOKit/IOService.h>
class IOUSBHostDevice;
class IOUSBHostPipe;
class AppleUSBHostController;
class AppleUSBHostPort : public IOService {
    OSDeclareAbstractStructors(AppleUSBHostPort);
    UInt8 nativeState[488];
protected:
    UInt32 fMinimumBusCurrent; // 624, Native init/allocateBusCurrent, mA
private:
    UInt8 nativePolicy[140];
protected:
    // HCD start supplies one retain; Native free releases it. Native port
    // policy, location and device creation all consult this field.
    AppleUSBHostController *fHostController; // 768
private:
    UInt8 nativeTail[144];
public:
    void free() APPLE_KEXT_OVERRIDE; // 14
    bool init(OSDictionary * = nullptr) APPLE_KEXT_OVERRIDE; // 21
    IOReturn setProperties(OSObject *) APPLE_KEXT_OVERRIDE; // 44
    bool init(IORegistryEntry *, const IORegistryPlane *) APPLE_KEXT_OVERRIDE; // 74
    bool willTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE; // 76
    bool didTerminate(IOService *, IOOptionBits, bool *) APPLE_KEXT_OVERRIDE; // 77
    void systemWillShutdown(IOOptionBits) APPLE_KEXT_OVERRIDE; // 79
    void registerService(IOOptionBits = 0) APPLE_KEXT_OVERRIDE; // 84
    bool start(IOService *) APPLE_KEXT_OVERRIDE; // 86
    void stop(IOService *) APPLE_KEXT_OVERRIDE; // 87
    bool open(IOService *, IOOptionBits = 0, void * = nullptr) APPLE_KEXT_OVERRIDE; // 88
    void close(IOService *, IOOptionBits = 0) APPLE_KEXT_OVERRIDE; // 89
    bool handleOpen(IOService *, IOOptionBits, void *) APPLE_KEXT_OVERRIDE; // 91
    void handleClose(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE; // 92
    bool handleIsOpen(const IOService *) const APPLE_KEXT_OVERRIDE; // 93
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE; // 111
    IOReturn message(UInt32, IOService *, void *) APPLE_KEXT_OVERRIDE; // 132
    IOReturn setPowerState(unsigned long, IOService *) APPLE_KEXT_OVERRIDE; // 155
    unsigned long initialPowerStateForDomainState(unsigned long) APPLE_KEXT_OVERRIDE; // 157
    IOReturn powerStateWillChangeTo(unsigned long, unsigned long, IOService *) APPLE_KEXT_OVERRIDE; // 159
    IOReturn powerStateDidChangeTo(unsigned long, unsigned long, IOService *) APPLE_KEXT_OVERRIDE; // 160

    virtual IOReturn reset(UInt32 options); // 168
    virtual IOReturn reset(UInt32 milliseconds, UInt32 options); // 169
    virtual IOReturn clearPipeStall(IOUSBHostPipe *); // 170
    virtual uintptr_t nativeSlot171(); // 171
    virtual IOReturn signalInterrupt(UInt32); // 172
    virtual UInt32 getPortStatus(); // 173
    virtual UInt32 getPortType() = 0; // 174
    virtual uintptr_t nativeSlot175(); // 175
    virtual uintptr_t nativeSlot176(); // 176
    virtual uintptr_t nativeSlot177(); // 177
    virtual uintptr_t nativeSlot178(); // 178
    virtual uintptr_t nativeSlot179(); // 179
    virtual uintptr_t nativeSlot180(); // 180
    virtual uintptr_t nativeSlot181(); // 181
    virtual uintptr_t nativeSlot182(); // 182
    virtual uintptr_t nativeSlot183(); // 183
    virtual uintptr_t nativeSlot184(); // 184
    virtual uintptr_t nativeSlot185(); // 185
    virtual uintptr_t nativeSlot186(); // 186
    virtual uintptr_t nativeSlot187(); // 187
    virtual uintptr_t nativeSlot188(); // 188
    virtual uintptr_t nativeSlot189(); // 189
    virtual uintptr_t nativeSlot190(); // 190
    virtual uintptr_t nativeSlot191(); // 191
    virtual uintptr_t nativeSlot192(); // 192
    virtual uintptr_t nativeSlot193(); // 193
    virtual uintptr_t nativeSlot194(); // 194
    virtual uintptr_t nativeSlot195(); // 195
    virtual uintptr_t nativeSlot196(); // 196
    virtual uintptr_t nativeSlot197(); // 197
    virtual uintptr_t nativeSlot198(); // 198
    virtual uintptr_t nativeSlot199(); // 199
    virtual IOReturn powerOff() = 0; // 200
    virtual IOReturn powerOn() = 0; // 201
    virtual IOReturn suspend() = 0; // 202
    virtual IOReturn resume() = 0; // 203
    virtual IOUSBHostDevice *resetAndCreateDevice(UInt32 milliseconds) = 0; // 204
    virtual uintptr_t nativeSlot205(); // 205
    virtual uintptr_t nativeSlot206(); // 206
    virtual uintptr_t nativeSlot207(); // 207
    virtual bool acquireAddressLock(); // 208
    virtual void releaseAddressLock(); // 209
    virtual IOUSBHostDevice *createDevice(UInt32 options); // 210
    virtual uintptr_t nativeSlot211(); // 211
    virtual uintptr_t nativeSlot212(); // 212
    virtual uintptr_t nativeSlot213(); // 213
    virtual IOReturn initBusCurrent(); // 214
    virtual uintptr_t nativeSlot215(); // 215
    virtual uintptr_t nativeSlot216(); // 216
    virtual uintptr_t nativeSlot217(); // 217
    virtual uintptr_t nativeSlot218(); // 218
    virtual IOReturn getCompanionPortGated(UInt32 speed,AppleUSBHostPort *&); // 219, cached/retained by Native
    virtual uintptr_t nativeSlot220(); // 220
    virtual uintptr_t nativeSlot221(); // 221
    virtual uintptr_t nativeSlot222(); // 222
    virtual uintptr_t nativeSlot223(); // 223
    virtual uintptr_t nativeSlot224(); // 224
    virtual uintptr_t nativeSlot225(); // 225
    virtual uintptr_t nativeSlot226(); // 226
    virtual uintptr_t nativeSlot227(); // 227
    virtual uintptr_t nativeSlot228(); // 228
    virtual uintptr_t nativeSlot229(); // 229
    virtual uintptr_t nativeSlot230(); // 230
    virtual uintptr_t nativeSlot231(); // 231
    virtual uintptr_t nativeSlot232(); // 232
    virtual uintptr_t nativeSlot233(); // 233
    virtual IOReturn getPortStatusGated(UInt32 &) = 0; // 234
};
static_assert(sizeof(AppleUSBHostPort)==920,"iOS18 host port instance ABI");
