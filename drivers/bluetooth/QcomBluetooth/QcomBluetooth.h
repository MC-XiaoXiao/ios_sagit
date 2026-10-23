// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include "BluetoothController.h"

#include "BluetoothPlatform.h"
#include "QcomBluetoothSerial.h"
#include "BluetoothControl.h"
#include <sys/kern_control.h>

class QcomBluetooth : public IOService {
    OSDeclareDefaultStructors(QcomBluetooth);
    IOWorkLoop* fWorkLoop = nullptr;
    IOCommandGate* fGate = nullptr;
    IOTimerEventSource* fTimer = nullptr;
    bool fActive = false, fPMInitialized = false;
    bool fDesiredPower = false, fPowerAck = false, fPortAcquired = false, fTXInFlight = false;
    uint64_t fRetryAt = 0;
    uint32_t fPortBits = Bluetooth::TXEnable | Bluetooth::RXEnable | Bluetooth::ModemReady;
    Bluetooth::SerialSettings fSerialSettings;
    Bluetooth::Result fLastFault = Bluetooth::Result::Success;
    QcomBluetoothSerial* fSerial = nullptr;
    IOInterruptEventSource* fInterrupt = nullptr;
    Bluetooth::ByteRing<Bluetooth::SerialCapacity> fRX, fTX;
    uint8_t fTXFrame[Bluetooth::MaxH4Frame] { };
    uint8_t fControlTX[Bluetooth::SerialCapacity] { };
    Bluetooth::ControlSession fControlSession;
    Bluetooth::NativeTermios fControlOptions;
    kern_ctl_ref fControl = nullptr;
    static QcomBluetooth* sControlOwner;
    struct ControlRequest {
        enum Operation { Connect, Disconnect, Send, GetOption, SetOption } operation;
        uint32_t unit;
        uint32_t option = 0;
        void* data = nullptr;
        size_t* length = nullptr;
        mbuf_t packet = nullptr;
        bool bootstrap = false;
    };
    static errno_t controlConnect(kern_ctl_ref, struct sockaddr_ctl*, void**);
    static errno_t controlDisconnect(kern_ctl_ref, uint32_t, void*);
    static errno_t controlSend(kern_ctl_ref, uint32_t, void*, mbuf_t, int);
    static errno_t controlGet(kern_ctl_ref, uint32_t, void*, int, void*, size_t*);
    static errno_t controlSet(kern_ctl_ref, uint32_t, void*, int, void*, size_t);
    static IOReturn gatedControl(OSObject*, void*, void*, void*, void*);
    errno_t controlRequest(ControlRequest&);
    errno_t performControl(ControlRequest&);
    void publishControl();
    bool unpublishControl();
    void flushControlRX();
    BluetoothPlatformBackend fPlatform;
    Bluetooth::Controller<BluetoothPlatformBackend> fController { fPlatform };
    static bool validProvider(IOService*);
    static uint64_t uptimeMS();
    static IOReturn gatedRequest(OSObject*, void*, void*, void*, void*);
    static IOReturn gatedBootstrapPower(OSObject*, void*, void*, void*, void*);
    static IOReturn gatedStop(OSObject*, void*, void*, void*, void*);
    static IOReturn gatedPower(OSObject*, void*, void*, void*, void*);
    static IOReturn gatedTerminate(OSObject*, void*, void*, void*, void*);
    static void timerFired(OSObject*, IOTimerEventSource*);
    static void interruptFired(OSObject*, IOInterruptEventSource*, int);
    static IOReturn gatedSerial(OSObject*, void*, void*, void*, void*);
    IOReturn performSerial(Bluetooth::SerialRequest&);
    uint32_t portState() const;
    void pump();
    void publishSerial();
    struct ReceiveSink {
        QcomBluetooth* owner;
        Bluetooth::Result bytes(const uint8_t*, size_t);
    };
    IOReturn perform(uint32_t, uint64_t*, uint64_t);
    void publishState();
    void cleanup();

public:
    IOService* probe(IOService*, SInt32*) APPLE_KEXT_OVERRIDE;
    bool start(IOService*) APPLE_KEXT_OVERRIDE;
    void stop(IOService*) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOWorkLoop* getWorkLoop() const APPLE_KEXT_OVERRIDE;
    IOReturn setPowerState(unsigned long, IOService*) APPLE_KEXT_OVERRIDE;
    IOReturn newUserClient(task_t, void*, UInt32, OSDictionary*, IOUserClient**) APPLE_KEXT_OVERRIDE;
    IOReturn request(uint32_t, uint64_t*, uint64_t = 0);
    IOReturn bootstrapPower(uint64_t, uint64_t, uint64_t*);
    IOReturn serial(Bluetooth::SerialRequest&);
    Bluetooth::Result receivedPacket(const uint8_t*, size_t);
    bool requestTerminate(IOService*, IOOptionBits) APPLE_KEXT_OVERRIDE;
};
