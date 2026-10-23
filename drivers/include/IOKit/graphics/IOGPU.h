// SPDX-License-Identifier: BSD-2-Clause
#pragma once
// Private arm64 ABI projection for the pinned IOGPUFamily image. Unknown
// methods are relocation-only aliases: never call them through these types.
// See apple-gpu-abi.json and docs/gpu/native-iogpu.md.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"

class IOAcceleratorES : public IOService {
    OSDeclareAbstractStructors(IOAcceleratorES);
};
class IOGPU;
class IOBufferMemoryDescriptor;
class IOGPUDevice;
class IOGPUTask;
class IOGPUResource;
class IOGPUCommandQueue;
class IOGPUWorkQueue;
class IOGPUCommandDescriptor;
class IOGPUScheduler;
class IOGPUMemoryMap;
class IOGPUSysMemory;
class IOSurface;
class IOGPUEventMachine;
class IOGPUFenceMachine;
class IOGPUEventFence;
struct IOGPUEventSet {
    UInt32 mask, values[15];
    void clear() { mask=0;for (auto &value:values)value=255; }
};
static_assert(sizeof(IOGPUEventSet)==64,"IOGPU event-set ABI");
class IOGPUObject : public OSObject {
    OSDeclareAbstractStructors(IOGPUObject);
    // Native IOGPUObject occupies OSObject's tail padding with its own count.
    // Naming it prevents a derived C++ class from reusing these four bytes.
    UInt32 nativeReferenceCount;
    static void verifyLayout() {
        static_assert(__builtin_offsetof(IOGPUObject, nativeReferenceCount) == 12, "IOGPU reference counter ABI");
    }
protected:
    virtual void nativeSlot15();
public:
    int getRetainCount() const APPLE_KEXT_OVERRIDE;
    void retain() const APPLE_KEXT_OVERRIDE;
    void release() const APPLE_KEXT_OVERRIDE;
    void release(int) const APPLE_KEXT_OVERRIDE;
    void taggedRetain(const void *) const APPLE_KEXT_OVERRIDE;
    void taggedRelease(const void *) const APPLE_KEXT_OVERRIDE;
    void taggedRelease(const void *, int) const APPLE_KEXT_OVERRIDE;
    bool init() APPLE_KEXT_OVERRIDE;
    bool initNativeObject(); // audited nonvirtual IOGPUObject initializer
};
class IOGPUMemory : public IOGPUObject {
    OSDeclareAbstractStructors(IOGPUMemory);
    UInt8 nativeMemoryState[112 - sizeof(IOGPUObject)];
public:
    void free() APPLE_KEXT_OVERRIDE;
protected:
    void nativeSlot15() APPLE_KEXT_OVERRIDE;
    virtual void nativeSlot16();
    virtual void nativeSlot17();
    virtual void nativeSlot18();
    virtual void nativeSlot19();
    virtual void nativeSlot20();
    virtual void nativeSlot21() = 0;
    virtual void nativeSlot22();
    virtual void nativeSlot23();
    virtual void nativeSlot24();
    virtual void nativeSlot25() = 0;
    virtual void nativeSlot26() = 0;
    virtual void nativeSlot27();
    virtual void nativeSlot28();
};
class IOGPUSysMemory : public IOGPUMemory {
    OSDeclareAbstractStructors(IOGPUSysMemory);
    UInt8 nativeSysMemoryState[168 - sizeof(IOGPUMemory)];
public:
    void free() APPLE_KEXT_OVERRIDE;
    IOMemoryDescriptor *memoryDescriptor(); // borrowed, native getter
protected:
    void nativeSlot16() APPLE_KEXT_OVERRIDE;
    void nativeSlot17() APPLE_KEXT_OVERRIDE;
    void nativeSlot21() APPLE_KEXT_OVERRIDE;
    void nativeSlot22() APPLE_KEXT_OVERRIDE;
    void nativeSlot23() APPLE_KEXT_OVERRIDE;
    void nativeSlot24() APPLE_KEXT_OVERRIDE;
    void nativeSlot25() APPLE_KEXT_OVERRIDE;
    void nativeSlot26() APPLE_KEXT_OVERRIDE;
    void nativeSlot27() APPLE_KEXT_OVERRIDE;
};
class IOGPUMemoryMap : public IOGPUObject {
    OSDeclareAbstractStructors(IOGPUMemoryMap);
    UInt8 nativeMapState[72 - sizeof(IOGPUObject)];
public:
    void free() APPLE_KEXT_OVERRIDE;
protected:
    void nativeSlot15() APPLE_KEXT_OVERRIDE;
    virtual void nativeSlot16();
    virtual void nativeSlot17();
    virtual void nativeSlot18();
    virtual UInt64 address(); // 19
    virtual UInt64 gpuAddress(); // 20
    virtual UInt64 length(); // 21
    virtual void nativeSlot22();
    virtual void nativeSlot23();
    virtual void nativeSlot24();
    virtual UInt64 mappedLength(); // 25
    virtual bool updateGPUPageTable() = 0; // 26
    virtual void nativeSlot27() = 0; // relocation-only, not used by this bridge
    virtual void nativeSlot28();
};
class IOGPUResource : public IOGPUObject {
    OSDeclareAbstractStructors(IOGPUResource);
protected:
    // Audited fields of IOGPUResource, relative to the 16-byte base object.
    UInt32 nativeFlags, nativeProtection;
    void *nativeSharedInfo;
    IOGPUMemoryMap *nativeMap;
    IOGPUMemory *nativeMemory;
    OSObject *nativeParent; // +48: resource parent, or IOSurfaceDeviceCache for 0x80
    UInt8 native56[16];
    UInt32 nativePlane, nativeSurfaceOffset; // +72/+76 for 0x82; together the 64-bit buffer base offset for 0x80
    UInt8 native80[40];
    UInt64 nativeLength; // object +120
    UInt8 native128[16];
    IOMemoryMap *nativeClientMap; // object +144
    UInt8 native152[8];
    static void verifyLayout() {
        static_assert(__builtin_offsetof(IOGPUResource, nativeFlags) == 16, "IOGPU resource flags ABI");
        static_assert(__builtin_offsetof(IOGPUResource, nativeMap) == 32, "IOGPU resource map ABI");
        static_assert(__builtin_offsetof(IOGPUResource, nativeMemory) == 40, "IOGPU resource memory ABI");
        static_assert(__builtin_offsetof(IOGPUResource, nativeParent) == 48, "IOGPU resource parent ABI");
        static_assert(__builtin_offsetof(IOGPUResource, nativePlane) == 72, "IOGPU surface plane ABI");
        static_assert(__builtin_offsetof(IOGPUResource, nativeSurfaceOffset) == 76, "IOGPU surface offset ABI");
        static_assert(__builtin_offsetof(IOGPUResource, nativeLength) == 120, "IOGPU resource length ABI");
        static_assert(__builtin_offsetof(IOGPUResource, nativeClientMap) == 144, "IOGPU client map ABI");
    }
public:
    void free() APPLE_KEXT_OVERRIDE;
    virtual bool initWithGPU(IOGPU *, IOGPUDevice *, UInt32); // 16
    virtual bool initialize(IOGPUDevice *, const void *, UInt64); // 17
    virtual UInt64 mappingOptions(const void *, UInt64); // 18
protected:
    virtual void nativeSlot19();
    virtual void nativeSlot20();
    virtual void nativeSlot21();
    virtual void nativeSlot22();
    virtual void nativeSlot23();
    virtual bool map(UInt64, UInt32); // 24
    virtual bool mapAtAddressLength(UInt64, UInt64, UInt64); // 25
    virtual void nativeSlot26();
    virtual void nativeSlot27() = 0; // unsupported native resource-group path
    virtual UInt64 nativeSlot28() = 0; // no resource-group size
    virtual void nativeSlot29();
    virtual void nativeSlot30();
    virtual void nativeSlot31();
    virtual void nativeSlot32();
};
class IOGPUDeviceShmem : public IOGPUObject {
    OSDeclareAbstractStructors(IOGPUDeviceShmem);
    UInt8 nativeShmemState[104 - sizeof(IOGPUObject)];
public:
    void free() APPLE_KEXT_OVERRIDE;
    virtual void *mapKernel(); // 16
    virtual void unmapKernel(); // 17
protected:
    virtual void nativeSlot18();
public:
    virtual UInt64 length(); // 19
protected:
    virtual void nativeSlot20();
    virtual void nativeSlot21();
};
class IOGPUDevice : public IOGPUObject {
    OSDeclareAbstractStructors(IOGPUDevice);
    UInt8 nativeDeviceState[80 - sizeof(IOGPUObject)];
    task_t nativeOwningTask;
    UInt8 nativeDevice88[256 - 88];
    static void verifyLayout() {
        static_assert(__builtin_offsetof(IOGPUDevice, nativeOwningTask) == 80, "IOGPU device task ABI");
    }
protected:
    void nativeSlot15() APPLE_KEXT_OVERRIDE;
    virtual void nativeSlot16();
    virtual void nativeSlot17();
    virtual void nativeSlot18();
    virtual void nativeSlot19();
    virtual void nativeSlot20();
    virtual void nativeSlot21();
    virtual void nativeSlot22();
    virtual void nativeSlot23();
    virtual void nativeSlot24();
    virtual void nativeSlot25();
    virtual void nativeSlot26();
    virtual void nativeSlot27();
    virtual void nativeSlot28();
public:
    task_t owningTask() const { return nativeOwningTask; }
    // Nonvirtual retained namespace lookups. Handle resolution stays native.
    IOGPUResource *copyResource(UInt32);
    IOGPUDeviceShmem *copyShmem(UInt32);
    IOGPUCommandQueue *copyCommandQueue(UInt32);
};
class IOGPUCommandQueue : public IOGPUObject {
    OSDeclareAbstractStructors(IOGPUCommandQueue);
protected:
    UInt8 nativeQueue16[1092 - sizeof(IOGPUObject)];
    UInt32 nativeForegroundPriority,nativeBackgroundPriority,nativeEffectivePriority;
    UInt8 nativeQueue1104[1176-1104];
    IOGPUEventSet nativeCurrentEvents, nativePriorEvents;
    IOGPUEventFence *nativeCurrentFence; // +1304, borrowed during hook 35
    UInt32 nativeSubmissionError; // audited per-command-buffer error, +1312
    UInt8 nativeQueue1316[1408 - 1316];
    static void verifyLayout() {
        static_assert(__builtin_offsetof(IOGPUCommandQueue,nativeForegroundPriority)==1092 &&
            __builtin_offsetof(IOGPUCommandQueue,nativeBackgroundPriority)==1096 &&
            __builtin_offsetof(IOGPUCommandQueue,nativeEffectivePriority)==1100,"IOGPU queue priority ABI");
        static_assert(__builtin_offsetof(IOGPUCommandQueue, nativeSubmissionError) == 1312, "IOGPU queue error ABI");
        static_assert(__builtin_offsetof(IOGPUCommandQueue,nativeCurrentEvents)==1176,"IOGPU queue current events ABI");
        static_assert(__builtin_offsetof(IOGPUCommandQueue,nativePriorEvents)==1240,"IOGPU queue prior events ABI");
        static_assert(__builtin_offsetof(IOGPUCommandQueue,nativeCurrentFence)==1304,"IOGPU queue current fence ABI");
    }
public:
    void free() APPLE_KEXT_OVERRIDE;
protected:
    virtual void nativeSlot16();
    virtual void nativeSlot17();
    virtual void nativeSlot18();
    virtual void nativeSlot19();
    virtual void nativeSlot20();
    virtual void nativeSlot21();
    virtual void nativeSlot22();
    virtual void nativeSlot23();
    virtual void nativeSlot24();
    virtual void nativeSlot25();
    virtual void nativeSlot26();
public:
    virtual bool initWithGPU(IOGPU *, IOGPUDevice *, const void *); // 27
protected:
    virtual void nativeSlot28();
    virtual void nativeSlot29();
    virtual void nativeSlot30();
    virtual void nativeSlot31();
    virtual void nativeSlot32();
    virtual void nativeSlot33();
    virtual void nativeSlot34();
    virtual void submitCommandBuffer(const void *); // 35
};
// IOGPUFamily 104.6.4, 22H374. These objects use the original runtime:
// append -> Scheduler preparation/submission -> completed-event retirement.
class IOGPUWorkQueue : public IOGPUObject {
    OSDeclareAbstractStructors(IOGPUWorkQueue);
protected:
    IOGPUScheduler *nativeScheduler; // +16, borrowed from IOGPU
    IOGPUCommandQueue *nativeContext;
    UInt8 nativeWork32[72-32];
    UInt32 nativeFinished, nativeSubmitted, nativeTail, nativeCapacity;
    IOGPUCommandDescriptor **nativeRing;
    UInt8 nativeWork96[104-96];
    UInt32 nativePriorityKey, nativeNamespaceID;
    UInt8 nativeWork112[192-112];
    IOGPU *nativeGPU;
    UInt8 nativeWork200[256-200];
    void nativeSlot15() APPLE_KEXT_OVERRIDE;
public:
    void free() APPLE_KEXT_OVERRIDE;
    virtual bool initWithGPU(IOGPU *, IOGPUCommandQueue *, UInt32); // 16
    virtual void logRingBuffer(); // 17
    virtual bool retire(); // 18
    virtual void restart(); // 19
    virtual void beginSubmit(); // 20
    virtual void endSubmit(); // 21
    virtual void submit(IOGPUCommandDescriptor *) = 0; // 22, hardware hook
    virtual void restartHardware(); // 23
    virtual const char *copyRestartReason(UInt32 *, UInt64 *); // 24
protected:
    virtual void nativeSlot25(); // relocation only
public:
    void nativeAppend(IOGPUCommandDescriptor *);
    void nativeSetPriority(UInt32);
    bool nativeIdle() const {return __atomic_load_n(&nativeFinished,__ATOMIC_ACQUIRE)==__atomic_load_n(&nativeTail,__ATOMIC_ACQUIRE);}
private:
    static void verifyLayout() {
        static_assert(__builtin_offsetof(IOGPUWorkQueue,nativeFinished)==72 &&
            __builtin_offsetof(IOGPUWorkQueue,nativeRing)==88 &&
            __builtin_offsetof(IOGPUWorkQueue,nativePriorityKey)==104 &&
            __builtin_offsetof(IOGPUWorkQueue,nativeGPU)==192,"IOGPU work queue ABI");
    }
};
class IOGPUCommandDescriptor : public IOGPUObject {
    OSDeclareAbstractStructors(IOGPUCommandDescriptor);
protected:
    IOGPU *nativeGPU; // +16
    UInt8 nativeDescriptor24[76-24];
    UInt8 nativeCancelled, nativePrepared, nativeDescriptor78[2];
    IOGPUWorkQueue *nativeWorkQueue; // +80, held by nativeAppend
    IOGPUTask *nativeTask;
    IOGPUCommandQueue *nativeContext;
    IOGPUEventFence *nativeFence;
    UInt64 nativeSubmittedTime;
    UInt32 nativeContextGeneration, nativeDescriptor124;
    IOGPUEventSet nativeCompletionEvents, nativeDependencies;
    UInt8 nativeDescriptor256[296-256];
public:
    void free() APPLE_KEXT_OVERRIDE;
    virtual bool initWithGPU(IOGPU *, IOGPUCommandQueue *, IOGPUTask *); // 16
    virtual void cancel(); // 17
    virtual IOReturn prepare(UInt32); // 18
    virtual void finish(); // 19
    virtual void willSubmit(); // 20
    virtual void didSubmit(); // 21
protected:
    virtual void nativeSlot22(); // relocation only
public:
    virtual void submitTo(IOGPUWorkQueue *); // 23
private:
    static void verifyLayout() {
        static_assert(__builtin_offsetof(IOGPUCommandDescriptor,nativeWorkQueue)==80 &&
            __builtin_offsetof(IOGPUCommandDescriptor,nativeFence)==104 &&
            __builtin_offsetof(IOGPUCommandDescriptor,nativeCompletionEvents)==128 &&
            __builtin_offsetof(IOGPUCommandDescriptor,nativeDependencies)==192,"IOGPU descriptor ABI");
    }
};
class IOGPUScheduler : public OSObject {
    OSDeclareAbstractStructors(IOGPUScheduler);
    alignas(8) UInt8 nativeScheduler16[552-sizeof(OSObject)];
public:
    void nativeResourcesAvailable();
    void free() APPLE_KEXT_OVERRIDE;
protected:
    virtual void nativeSlot15();
    virtual void nativeSlot16();
    virtual void nativeSlot17();
    virtual void nativeSlot18();
    virtual void nativeSlot19();
    virtual void nativeSlot20();
    virtual void nativeSlot21();
    virtual void nativeSlot22();
};
static_assert(sizeof(IOGPUWorkQueue)==256 && sizeof(IOGPUCommandDescriptor)==296 &&
    sizeof(IOGPUScheduler)==552,"IOGPU scheduling objects ABI");
class IOGPUTask : public IOGPUObject {
    OSDeclareAbstractStructors(IOGPUTask);
    UInt8 nativeTaskState[464 - sizeof(IOGPUObject)];
public:
    void free() APPLE_KEXT_OVERRIDE;
    virtual bool initWithGPU(IOGPU *, task_t, UInt32, OSObject **); // 16
protected:
    virtual void nativeSlot17();
    virtual void nativeSlot18();
    virtual void nativeSlot19();
    virtual void nativeSlot20();
};
class IOGPUEventMachine : public OSObject {
    OSDeclareAbstractStructors(IOGPUEventMachine);
    alignas(8) UInt8 nativeEvent16[64-sizeof(OSObject)];
protected:
    struct StampState { UInt32 reserved[2], issued, submitted; } nativeStampState[128]; // +64
private:
    UInt8 nativeEvent2112[5200-2112];
    static void verifyLayout() {
        static_assert(__builtin_offsetof(IOGPUEventMachine,nativeStampState)==64,"IOGPU stamp state ABI");
        static_assert(sizeof(StampState)==16,"IOGPU stamp stride ABI");
    }
public:
    void nativeAdvanceStamp(UInt32);
    UInt32 nativeIssuedStamp(UInt32);
    void nativeSubmittedStamp(UInt32,UInt32);
    void nativeAddEvent(UInt32,IOGPUEventSet *,bool);
    void nativeSignalStamp(UInt32,UInt32);
    void free() APPLE_KEXT_OVERRIDE;
    virtual bool initWithGPU(IOGPU *, UInt64, UInt32); // 15
    virtual void setStampMemory(void *); // 16
    virtual UInt32 stampOffset(UInt32); // 17
    virtual void encodeStamp(UInt32, UInt32 *, UInt32) = 0; // 18, two words
    virtual void clearEvent(void *) = 0; // 19, native 184-byte event record
    virtual bool supportsEvents() = 0; // 20
    virtual void appendEvent(UInt32 *, UInt32, UInt32) = 0; // 21
protected:
    virtual void nativeSlot22();
    virtual void nativeSlot23();
    virtual void nativeSlot24();
};
class IOGPUEventFence : public OSObject {
    OSDeclareAbstractStructors(IOGPUEventFence);
    alignas(8) UInt8 native16[48-sizeof(OSObject)];
    IOGPUEventSet nativeStartEvents, nativeEndEvents;
    UInt8 native176[24];
    UInt32 nativeStatus, native204;
    static void verifyLayout() {
        static_assert(__builtin_offsetof(IOGPUEventFence,nativeStartEvents)==48,"IOGPU fence start ABI");
        static_assert(__builtin_offsetof(IOGPUEventFence,nativeEndEvents)==112,"IOGPU fence end ABI");
        static_assert(__builtin_offsetof(IOGPUEventFence,nativeStatus)==200,"IOGPU fence status ABI");
    }
public:
    // Caller holds the native FenceMachine lock. These are audited fields,
    // not virtual entry points or fabricated notification implementations.
    void setNativeStatus(UInt32 status) { nativeStatus=status; }
    void clearNativeStartEvents() { nativeStartEvents.clear(); }
    void failNativeEvents() { nativeStatus=8;nativeStartEvents.clear();nativeEndEvents.clear(); }
};
class IOGPUFenceMachine : public OSObject {
    alignas(8) UInt8 native16[56-sizeof(OSObject)];
    UInt32 nativeCallbacks, native60;
    UInt8 native64[112-64];
    UInt32 nativePending, nativeWaiters;
    static void verifyLayout() {
        static_assert(__builtin_offsetof(IOGPUFenceMachine,nativePending)==112,"IOGPU pending fences ABI");
    }
public:
    void nativeLock();
    void nativeUnlock();
    void nativeRetire();
    UInt32 pendingLocked() const { return nativePending || nativeCallbacks; }
};
class IOGPUDeviceUserClient : public IOUserClient {
    OSDeclareDefaultStructors(IOGPUDeviceUserClient);
    UInt8 nativeClientState[288 - sizeof(IOUserClient)];
    IOGPUDevice *nativeDevice; // populated by native deviceUserClientStart
    UInt64 nativeClientOptions;
    static void verifyLayout() {
        static_assert(__builtin_offsetof(IOGPUDeviceUserClient, nativeDevice) == 288, "IOGPU client device ABI");
    }
public:
    IOGPUDevice *device() const { return nativeDevice; }
    void free() APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(UInt32, IOExternalMethodArguments *, IOExternalMethodDispatch *, OSObject *, void *) APPLE_KEXT_OVERRIDE;
    IOReturn registerNotificationPort(mach_port_t, UInt32, io_user_reference_t) APPLE_KEXT_OVERRIDE;
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    IOExternalTrap *getTargetAndTrapForIndex(IOService **, UInt32) APPLE_KEXT_OVERRIDE;
protected:
    virtual bool deviceUserClientStart(); // 186
    virtual void deviceUserClientStop(); // 187
    virtual void nativeSlot188();
};

// The native initializer supplies defaults. Only fields established from
// start() and the query dispatchers are named here; keep all other bytes.
struct alignas(8) IOGPUConfiguration {
    const char *name;                 // 0
    UInt32 flags, family, revision, debug; // 8..20
    UInt32 stampCount, reserved28;    // 24
    UInt64 native32, native40;
    UInt16 maxWidth, maxHeight;
    UInt32 reserved52;
    UInt32 native56, native60, native64, clientInfoBytes;
    UInt32 native72, native76, native80, timeoutSeconds;
    UInt8 reserved88[112];
};
class IOGPU : public IOAcceleratorES {
    OSDeclareAbstractStructors(IOGPU);
    UInt8 nativeGPU136[320-sizeof(IOAcceleratorES)];
    IOGPUEventMachine *nativeEvents;
    UInt8 nativeGPU328[8];
    IOGPUFenceMachine *nativeFences;
    IOGPUScheduler *nativeScheduler; // +344, owned by IOGPU::start/stop
    UInt8 nativeGPU352[744-352];
    static void verifyLayout() {
        static_assert(__builtin_offsetof(IOGPU,nativeEvents)==320,"IOGPU event machine ABI");
        static_assert(__builtin_offsetof(IOGPU,nativeFences)==336,"IOGPU fence machine ABI");
        static_assert(__builtin_offsetof(IOGPU,nativeScheduler)==344,"IOGPU scheduler ABI");
    }
public:
    IOGPUEventMachine *nativeEventMachine() const { return nativeEvents; }
    IOGPUFenceMachine *nativeFenceMachine() const { return nativeFences; }
    IOGPUScheduler *nativeSchedulerObject() const { return nativeScheduler; }
    void nativeNotifyCompletion();
    IOSurface *nativeCopySurfaceForTask(UInt32, task_t); // audited retained task lookup
    UInt32 nativeAcceleratorIdentifier() const {
        UInt32 value;__builtin_memcpy(&value,reinterpret_cast<const UInt8 *>(this)+248,sizeof(value));return value;
    }
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn configureReport(IOReportChannelList *, IOReportConfigureAction, void *, void *) APPLE_KEXT_OVERRIDE;
    IOReturn updateReport(IOReportChannelList *, IOReportUpdateAction, void *, void *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    IOReturn newUserClient(task_t, void *, UInt32, IOUserClient **) APPLE_KEXT_OVERRIDE;
protected:
    virtual void nativeSlot168();
    virtual void nativeSlot169();
    virtual IOMemoryDescriptor *getStampMemory(UInt32 *) = 0; // 170, borrowed
    virtual void nativeSlot171();
    virtual IOGPUScheduler *allocScheduler(); // 172
    virtual void nativeSlot173();
    virtual IOGPUEventMachine *allocEventMachine() = 0; // 174
    virtual void nativeSlot175();
    virtual void nativeSlot176();
    virtual void nativeSlot177();
    virtual void nativeSlot178();
    virtual void nativeSlot179();
    virtual void nativeSlot180();
    virtual void nativeSlot181();
    virtual void nativeSlot182();
    virtual void nativeSlot183();
    virtual void nativeSlot184();
    virtual void nativeSlot185();
    virtual void nativeSlot186();
    virtual IOBufferMemoryDescriptor *createBufferMemoryDescriptor(task_t, IOOptionBits, vm_size_t, vm_size_t); // 187
    virtual void nativeSlot188();
    virtual IOGPUTask *createGPUTask(task_t) = 0; // 189
    virtual void nativeSlot190();
    virtual void nativeSlot191();
    virtual void getGPUConfiguration(IOGPUConfiguration *) = 0; // 192
    virtual void nativeSlot193();
    virtual void nativeSlot194();
    virtual bool configureDevice(IOService *) = 0; // 195
    virtual void nativeSlot196() = 0;
    virtual IOGPUTask *createKernelGPUTask() = 0; // 197
    virtual void nativeSlot198();
    virtual IOGPUDeviceUserClient *allocDeviceUserClient(); // 199
    virtual IOGPUSysMemory *allocSystemMemory() = 0; // 200
    virtual void nativeSlot201();
    virtual IOGPUResource *allocResource() = 0; // 202
    virtual void nativeSlot203();
    virtual void nativeSlot204();
    virtual OSObject *allocWorkQueue() = 0; // 205
    virtual void nativeSlot206();
    virtual void nativeSlot207();
    virtual void nativeSlot208();
    virtual void nativeSlot209();
    virtual IOGPUCommandQueue *allocCommandQueue(); // 210
    virtual void nativeSlot211();
    virtual void nativeSlot212();
    virtual void nativeSlot213();
    virtual void nativeSlot214();
};
static_assert(sizeof(IOAcceleratorES) == 136, "IOAcceleratorES ABI");
static_assert(sizeof(IOGPU) == 744, "IOGPU ABI");
static_assert(sizeof(IOGPUConfiguration) == 200, "IOGPU configuration ABI");
static_assert(__builtin_offsetof(IOGPUConfiguration, stampCount) == 24, "IOGPU stamp count ABI");
static_assert(__builtin_offsetof(IOGPUConfiguration, clientInfoBytes) == 68, "IOGPU client info ABI");
static_assert(sizeof(IOGPUTask) == 464, "IOGPUTask ABI");
static_assert(sizeof(IOGPUEventMachine) == 5200, "IOGPUEventMachine ABI");
static_assert(sizeof(IOGPUEventFence)==208 && sizeof(IOGPUFenceMachine)==120,"IOGPU fence ABI");
static_assert(sizeof(IOGPUDeviceUserClient) == 304, "IOGPUDeviceUserClient ABI");
static_assert(sizeof(IOGPUResource) == 160 && sizeof(IOGPUCommandQueue) == 1408, "IOGPU resource/queue ABI");
static_assert(sizeof(IOGPUMemory) == 112 && sizeof(IOGPUSysMemory) == 168, "IOGPU memory ABI");
static_assert(sizeof(IOGPUMemoryMap) == 72 && sizeof(IOGPUDeviceShmem) == 104, "IOGPU map/shmem ABI");
#pragma clang diagnostic pop
