/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _QCOM_PANEL_H
#define _QCOM_PANEL_H
#include <AppleARMPlatform/AppleARMMIPIDSI.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOWorkLoop.h>
#include "PanelProtocol.h"

class QcomPanel : public IOService {
    OSDeclareDefaultStructors(QcomPanel);
public:
    IOService *probe(IOService *, SInt32 *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool requestTerminate(IOService *, IOOptionBits) APPLE_KEXT_OVERRIDE;
    IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) APPLE_KEXT_OVERRIDE;
private:
    AppleARMMIPIDSIDevice *device;
    IOWorkLoop *workLoop;
    IOCommandGate *gate;
    bool added, opened, ready;
    uint32_t brightnessBits;
    bool initializeSequence(const uint8_t *, size_t);
    IOReturn brightness(uint32_t);
    void releaseResources();
    static IOReturn brightnessAction(OSObject *, void *, void *, void *, void *);
};
#endif
