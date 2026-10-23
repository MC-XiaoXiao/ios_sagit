/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _QCOM_MIPI_DSI_H
#define _QCOM_MIPI_DSI_H
#include <AppleARMPlatform/AppleARMMIPIDSI.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include "MIPIDSIProtocol.h"

class QcomMIPIDSI : public AppleARMMIPIDSIController {
    OSDeclareDefaultStructors(QcomMIPIDSI);
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    IOReturn enableInterface(bool) APPLE_KEXT_OVERRIDE;
    IOReturn enableHighSpeedMode(bool) APPLE_KEXT_OVERRIDE;
    IOReturn enableVideo(bool) APPLE_KEXT_OVERRIDE;
    IOReturn sendShortCommand(UInt8, UInt8, UInt8) APPLE_KEXT_OVERRIDE;
    IOReturn sendLongCommand(UInt8, const UInt8 *, UInt64) APPLE_KEXT_OVERRIDE;
    IOReturn receiveShortCommand(UInt8, UInt8 *) APPLE_KEXT_OVERRIDE;
    IOReturn receiveLongCommand(UInt8, UInt8 *, UInt64 *) APPLE_KEXT_OVERRIDE;
private:
    struct Registers {
        volatile uint8_t *base;
        uint32_t read(uint32_t);
        void write(uint32_t, uint32_t);
        void delay(unsigned);
    } regs;
    IOWorkLoop *workLoop;
    IOCommandGate *gate;
    IOMemoryMap *map;
    IOBufferMemoryDescriptor *commandMemory;
    IODMACommand *dma;
    IOMapper *mapper;
    uint64_t commandAddress;
    bool prepared, gateAdded, opened, running, highSpeed, dmaPinned;
    struct Operation { unsigned kind; uint8_t type; const uint8_t *data; uint64_t length; bool enable; uint8_t *reply = nullptr; };
    static IOReturn action(OSObject *, void *, void *, void *, void *);
    IOReturn perform(Operation &);
    IOReturn transfer(UInt8, const UInt8 *, UInt64, bool);
    IOReturn setEnabled(unsigned, bool);
    void releaseResources();
};
#endif
