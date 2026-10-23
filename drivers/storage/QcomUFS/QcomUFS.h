/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _QCOM_UFS_H
#define _QCOM_UFS_H
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include <IOKit/IOLocks.h>
#include <IOKit/storage/IOStorage.h>
#include <kern/thread_call.h>
#include "UFSProtocol.h"

class QcomUFSBlockDevice;
class QcomUFS : public IOService {
    OSDeclareDefaultStructors(QcomUFS);
public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    IOReturn setPowerState(unsigned long, IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn readWrite(uint8_t lun, IOMemoryDescriptor *buffer, uint64_t block,
        uint64_t blocks, IOStorageAttributes *, IOStorageCompletion *);
    IOReturn synchronize(uint8_t lun);
    bool mediaPresent(uint8_t lun) const;
    uint64_t blocks(uint8_t lun) const;
    uint32_t blockSize(uint8_t lun) const;
    bool writeProtected(uint8_t lun) const;

private:
    struct Registers {
        volatile uint8_t *base;
        uint32_t read(uint32_t offset);
        void write(uint32_t offset, uint32_t value);
        void delay(unsigned usec);
    } regs, phy;
    uint32_t recoveredUICNotifications;
    bool linkChecking;
    struct DMARegion {
        IOBufferMemoryDescriptor *memory;
        IODMACommand *dma;
        uint64_t address;
        void *bytes;
        bool prepared;
    } transferList, commandList, taskList;
    struct Request {
        Request *next;
        IOMemoryDescriptor *buffer;
        IODMACommand *dma;
        IOLock *syncLock;
        IOStorageCompletion completion;
        UFS::PRD prds[UFS::MaxPRDs];
        uint8_t packet[512], reply[512];
        uint32_t length, actual;
        unsigned prdCount;
        uint8_t lun, responseType;
        uint64_t deadline;
        IOReturn result;
        bool prepared, dmaPrepared, synchronous, ordered, completed, linkProbe;
        bool admitted;
    };
    struct LUN {
        QcomUFSBlockDevice *device;
        uint64_t blocks;
        uint32_t blockSize;
        uint8_t depth;
        bool enabled, writeProtected;
    } luns[UFS::MaxLUNs];
    IOWorkLoop *workLoop;
    IOCommandGate *gate;
    IOInterruptEventSource *interrupt;
    IOTimerEventSource *timer;
    IOMemoryMap *hostMap, *phyMap;
    IOMapper *mapper;
    thread_call_t finisher, powerCall, preparer, backgroundCall, linkCheckCall;
    Request *mappingHead, *mappingTail;
    unsigned mappingCount, mappingInFlight;
    Request *pendingHead, *pendingTail, *doneHead, *doneTail;
    Request *active[UFS::MaxSlots];
    unsigned pendingCount, queueSlots, addressBits, laneLimit, firmwareGear, firmwarePowerMode;
    uint32_t activeMask;
    uint64_t timeoutTicks;
    bool running, suspended, published, dmaCoherent, dmaQuiesced, warmHandoff, polledCompletion;
    bool gateAdded, interruptAdded, timerAdded, hardwareOwned;
    bool providerOpened;
    bool powerChanging, pmStarted;
    bool mediaNotificationPending;
    unsigned powerTarget;
    unsigned negotiatedGear, negotiatedTXLanes, negotiatedRXLanes;

    bool configurePlatform(IOService *);
    bool initializeLink();
    bool negotiateLink();
    bool allocateRegion(DMARegion &, unsigned bytes, unsigned alignment);
    void freeRegion(DMARegion &);
    bool initializeLists();
    bool discover();
    IOReturn query(uint8_t opcode, uint8_t idn, uint8_t index, uint8_t *reply);
    IOReturn execute(Request *);
    IOReturn scsi(uint8_t lun, const uint8_t *cdb, IOMemoryDescriptor *buffer,
        uint32_t bytes, bool ordered = false);
    IOReturn prepare(Request *, IOMemoryDescriptor *);
    void dispose(Request *);
    void submitPending();
    void complete(unsigned tag);
    void appendDone(Request *, IOReturn, uint32_t actual = 0);
    void failController(IOReturn reason);
    void releaseResources();
    static IOReturn enqueueRequest(OSObject *, void *, void *, void *, void *);
    static IOReturn mappingAction(OSObject *, void *, void *, void *, void *);
    static IOReturn mediaAction(OSObject *, void *, void *, void *, void *);
    static IOReturn takeDone(OSObject *, void *, void *, void *, void *);
    static IOReturn shutdown(OSObject *, void *, void *, void *, void *);
    static IOReturn powerAction(OSObject *, void *, void *, void *, void *);
    static void interruptAction(OSObject *, IOInterruptEventSource *, int);
    static void timerAction(OSObject *, IOTimerEventSource *);
    static void finish(thread_call_param_t, thread_call_param_t);
    static void changePower(thread_call_param_t, thread_call_param_t);
    static void prepareGated(thread_call_param_t, thread_call_param_t);
    static void background(thread_call_param_t, thread_call_param_t);
    static void checkLink(thread_call_param_t, thread_call_param_t);
    static IOReturn linkChecked(OSObject *,void *,void *,void *,void *);
};
#endif
