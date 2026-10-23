// SPDX-License-Identifier: BSD-2-Clause
#pragma once
// Pinned iOS18 arm64 Host Family projection. Native slot194 is the audited
// device allocation helper; the public port implementation attaches/starts its
// returned object. Unknown nativeSlot signatures preserve dispatch only and
// must not be called or overridden by a runtime adapter before further audit.
#include <IOKit/IOService.h>
#include <IOKit/usb/IOUSBHostPipe.h>
class AppleUSBHostRequest;
class IOUSBHostStream;
class IODMACommand;
class IOBufferMemoryDescriptor;
class IOFilterInterruptEventSource;
class IOInterruptEventSource;
class AppleUSBHostPort;
class IOUSBHostDevice;
enum tRegisterHubFlags : UInt32 { kRegisterHubFlagsNone=0,kRegisterHubFlagsMultiTT=1 };
class AppleUSBHostController : public IOService {
    OSDeclareAbstractStructors(AppleUSBHostController);
    UInt8 nativeState[1032];
protected:
    // iOS18 EHCI filter publishes these fields at 1168/1176/1184. Native
    // getFrameNumber/getFrameRollover retry while the two counters differ.
    UInt64 fFrameRolloverTime;
    UInt64 fFrameRolloverBegin;
    UInt64 fFrameRolloverEnd;
private:
    UInt8 nativeFrameTail[24];
protected:
    // Audited iOS18 ownership table. Native createPorts allocates it once;
    // the Native controller teardown owns its started port references.
    AppleUSBHostPort **fPorts; // 1216
    UInt32 fPortCount; // 1224
private:
    UInt8 nativeTail[156];
public:
    void free() APPLE_KEXT_OVERRIDE; // 14
    bool init(OSDictionary * = nullptr) APPLE_KEXT_OVERRIDE; // 21
    IOReturn setProperties(OSObject *) APPLE_KEXT_OVERRIDE; // 44
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
    bool terminate(IOOptionBits = 0) APPLE_KEXT_OVERRIDE; // 94
    bool matchPropertyTable(OSDictionary *, SInt32 *) APPLE_KEXT_OVERRIDE; // 101
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE; // 111
    IOReturn message(UInt32, IOService *, void *) APPLE_KEXT_OVERRIDE; // 132
    void PMstop() APPLE_KEXT_OVERRIDE; // 145
    IOReturn setPowerState(unsigned long, IOService *) APPLE_KEXT_OVERRIDE; // 155
    unsigned long maxCapabilityForDomainState(unsigned long) APPLE_KEXT_OVERRIDE; // 156
    unsigned long initialPowerStateForDomainState(unsigned long) APPLE_KEXT_OVERRIDE; // 157
    IOReturn powerStateWillChangeTo(unsigned long, unsigned long, IOService *) APPLE_KEXT_OVERRIDE; // 159
    IOReturn powerStateDidChangeTo(unsigned long, unsigned long, IOService *) APPLE_KEXT_OVERRIDE; // 160

    virtual UInt64 getSystemWakeTime(); // 168
    virtual uintptr_t nativeSlot169(); // 169
    virtual uintptr_t nativeSlot170(); // 170
    virtual uintptr_t nativeSlot171(); // 171
    virtual uintptr_t nativeSlot172(); // 172
    virtual uintptr_t nativeSlot173(); // 173
    virtual uintptr_t nativeSlot174(); // 174
    virtual uintptr_t nativeSlot175(); // 175
    virtual uintptr_t nativeSlot176(); // 176
    virtual uintptr_t nativeSlot177(); // 177
    virtual uintptr_t nativeSlot178(); // 178
    virtual uintptr_t nativeSlot179(); // 179
    virtual uintptr_t nativeSlot180(); // 180
    virtual uintptr_t nativeSlot181(); // 181
    virtual IOReturn powerDown(unsigned long); // 182; target Native state 0..2
    virtual IOReturn powerUp(unsigned long); // 183; target Native state 1..3
    virtual uintptr_t nativeSlot184(); // 184
    virtual uintptr_t nativeSlot185(); // 185
    virtual uintptr_t nativeSlot186(); // 186
    virtual uintptr_t nativeSlot187(); // 187
    virtual uintptr_t nativeSlot188(); // 188
    virtual uintptr_t nativeSlot189(); // 189
    virtual bool acquireAddressLock(); // 190
    virtual void releaseAddressLock(); // 191
    virtual IOUSBHostDevice *createDevice(UInt32 speed, UInt32 location, UInt32 options) = 0; // 192
    virtual void nativeSlot193(IOUSBHostDevice *) = 0; // 193
    virtual IOUSBHostDevice *nativeSlot194(UInt8 speed, UInt16 address); // 194
    virtual IOReturn registerHub(IOUSBHostDevice *,const StandardUSB::HubDescriptor *,tRegisterHubFlags); // 195
    virtual IOReturn registerHub(IOUSBHostDevice *,const StandardUSB::SuperSpeedHubDescriptor *); // 196
    virtual IOReturn unregisterHub(IOUSBHostDevice *); // 197
    virtual IOReturn registerHubGated(IOUSBHostDevice *,const StandardUSB::HubDescriptor *,tRegisterHubFlags); // 198
    virtual IOReturn registerSuperSpeedHubGated(IOUSBHostDevice *,const StandardUSB::SuperSpeedHubDescriptor *); // 199
    virtual IOReturn unregisterHubGated(IOUSBHostDevice *); // 200
    virtual IOUSBHostPipe *createPipe(IOUSBHostPipe::StandardUSBDescriptors &,IOUSBHostDevice *,IOUSBHostInterface *); // 201
    virtual IOReturn io(AppleUSBHostRequest *); // 202
    virtual IOReturn abortPipe(IOUSBHostPipe *,IOReturn,IOService *); // 203
    virtual IOReturn clearPipeStall(IOUSBHostPipe *); // 204
    virtual IOReturn suspendPipes(IOUSBHostDevice *); // 205
    virtual IOReturn resumePipes(IOUSBHostDevice *); // 206
    virtual IOReturn adjustPipe(IOUSBHostPipe *,IOUSBHostPipe::StandardUSBDescriptors &); // 207
    virtual IOReturn closePipe(IOUSBHostPipe *); // 208
    virtual IOReturn enableStreams(IOUSBHostPipe *); // 209
    virtual IOReturn disableStreams(IOUSBHostPipe *); // 210
    virtual IOUSBHostStream *createStream(IOUSBHostDevice *,IOUSBHostPipe *,UInt32); // 211
    virtual IOReturn abortStream(IOUSBHostStream *,IOReturn); // 212
    virtual IOReturn closeStream(IOUSBHostStream *); // 213
    virtual uintptr_t nativeSlot214(); // 214
    virtual uintptr_t nativeSlot215(); // 215
    // Native packet-filter notification: stage0 records admission/status;
    // stage1 reports completion. Callback return values are not consumed.
    virtual void nativeSlot216(AppleUSBHostRequest *,IOReturn,UInt32 stage); // 216
    virtual IOReturn createPipeGated(IOUSBHostPipe::StandardUSBDescriptors &,IOUSBHostDevice *,IOUSBHostInterface *,IOUSBHostPipe *&) = 0; // 217
    virtual IOReturn ioGated(AppleUSBHostRequest *) = 0; // 218
    virtual IOReturn abortPipeGated(IOUSBHostPipe *,IOReturn,IOService *) = 0; // 219
    virtual IOReturn clearPipeStallGated(IOUSBHostPipe *) = 0; // 220
    virtual IOReturn suspendPipesGated(IOUSBHostDevice *) = 0; // 221
    virtual IOReturn resumePipesGated(IOUSBHostDevice *) = 0; // 222
    virtual IOReturn adjustPipeGated(IOUSBHostPipe *,IOUSBHostPipe::StandardUSBDescriptors &) = 0; // 223
    virtual IOReturn closePipeGated(IOUSBHostPipe *) = 0; // 224
    virtual IOReturn enableStreamsGated(IOUSBHostPipe *) = 0; // 225
    virtual IOReturn disableStreamsGated(IOUSBHostPipe *) = 0; // 226
    virtual IOReturn createStreamGated(IOUSBHostDevice *,IOUSBHostPipe *,UInt32,IOUSBHostStream *&) = 0; // 227
    virtual IOReturn abortStreamGated(IOUSBHostStream *,IOReturn) = 0; // 228
    virtual IOReturn closeStreamGated(IOUSBHostStream *) = 0; // 229
    virtual IOBufferMemoryDescriptor *createIOBuffer(UInt32,UInt64); // 230
    virtual IODMACommand *getDMACommand(UInt32 alignment,UInt32 options) = 0; // 231
    virtual void returnDMACommand(IODMACommand *); // 232
    virtual UInt64 getFrameNumber(UInt64 *absoluteTime = nullptr); // 233
    virtual UInt64 getFrameRollover(UInt64 *absoluteTime = nullptr); // 234
    virtual AppleUSBHostPort *getCompanionPort(AppleUSBHostPort *,UInt32 speed); // 235, borrowed
    virtual uintptr_t nativeSlot236(); // 236
    virtual uintptr_t nativeSlot237(); // 237
    virtual uintptr_t nativeSlot238(); // 238
    virtual uintptr_t nativeSlot239(); // 239
    virtual uintptr_t nativeSlot240(); // 240
    virtual uintptr_t nativeSlot241(); // 241
    virtual uintptr_t nativeSlot242(); // 242
    virtual uintptr_t nativeSlot243(); // 243
    virtual uintptr_t nativeSlot244(); // 244
    virtual uintptr_t nativeSlot245(); // 245
    virtual uintptr_t nativeSlot246(); // 246
    virtual IOReturn createPorts(); // 247
    virtual bool filterOccurred(IOFilterInterruptEventSource *) = 0; // 248
    virtual void interruptOccurred(IOInterruptEventSource *,int) = 0; // 249
    virtual uintptr_t nativeSlot250(); // 250
    virtual uintptr_t nativeSlot251(); // 251
    virtual uintptr_t nativeSlot252(); // 252
    virtual uintptr_t nativeSlot253(); // 253
    virtual uintptr_t nativeSlot254(); // 254
    virtual uintptr_t nativeSlot255(); // 255
    virtual uintptr_t nativeSlot256(); // 256
    virtual uintptr_t nativeSlot257(); // 257
    virtual uintptr_t nativeSlot258(); // 258
    virtual uintptr_t nativeSlot259(); // 259
    virtual uintptr_t nativeSlot260(); // 260
    virtual uintptr_t nativeSlot261(); // 261
    virtual uintptr_t nativeSlot262(); // 262
    virtual uintptr_t nativeSlot263(); // 263
    virtual uintptr_t nativeSlot264(); // 264
    virtual uintptr_t nativeSlot265(); // 265
    virtual UInt64 getRawMicroFrameCount() = 0; // 266
    virtual UInt64 makeFrameNumber(UInt64, UInt64) const = 0; // 267
    virtual uintptr_t nativeSlot268(); // 268
};
static_assert(sizeof(AppleUSBHostController)==1384,"iOS18 host controller instance ABI");
