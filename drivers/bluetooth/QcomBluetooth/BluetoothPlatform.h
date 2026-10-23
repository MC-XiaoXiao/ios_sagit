// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <PinmuxProtocol.h>
#include "BluetoothConfiguration.h"
#include "BluetoothBoot.h"
#include "BluetoothUART.h"
class QcomBluetooth;
class BluetoothPlatformBackend {
    QcomBluetooth* owner = nullptr;
    IOService *provider = nullptr, *bus = nullptr, *gpio = nullptr;
    IOInterruptEventSource* irq = nullptr;
    IOMemoryMap* map = nullptr;
    OSData *patchData = nullptr, *nvmData = nullptr;
    Bluetooth::Configuration config { };
    Bluetooth::FirmwareView patch { }, nvm { };
    Pinmux::Request pins { };
    struct Registers {
        volatile uint32_t* base = nullptr;
        uint32_t read(uint32_t);
        void write(uint32_t, uint32_t);
    } registers;
    Bluetooth::UARTPort<Registers> port { registers };
    Bluetooth::BootSequence<BluetoothPlatformBackend> boot { *this };
    Bluetooth::FirmwareLoader<BluetoothPlatformBackend> loader { *this };
    uint32_t railMask = 0, clockMask = 0;
    uint64_t originalRate = 0, shutdownDeadline = 0, uartClockToken = 0;
    unsigned shutdownPhase = 0;
    bool configured = false, bootStarted = false, loaderStarted = false, extraClock = false,
         releaseVote = false;
    bool resetFraming = false, quarantined = false;
    Bluetooth::Result clock(uint32_t operation, uint32_t id, uint64_t& value, unsigned controller = 1);
    Bluetooth::Result pinRequest(Pinmux::Action);

public:
    static Bluetooth::Result convert(IOReturn);
    static uint64_t now();
    bool bind(IOService*, QcomBluetooth*, IOInterruptEventSource*);
    void unbind();
    bool ready() const
    {
        return configured;
    }
    void bootstrapPolicy(uint64_t* values) const;
    bool hasUART() const
    {
        return port.enabled();
    }
    bool wantsHCI() const
    {
        using Phase = Bluetooth::FirmwareLoader<BluetoothPlatformBackend>::Phase;
        return loaderStarted && loader.currentPhase() != Phase::BaudDrain
            && loader.currentPhase() != Phase::BaudSettle;
    }
    bool takeFramingReset()
    {
        const bool result = resetFraming;
        resetFraming = false;
        return result;
    }
    Bluetooth::Result acquire(Bluetooth::Resource, uint64_t);
    Bluetooth::Result release(Bluetooth::Resource, uint64_t);
    Bluetooth::Result vote(bool, uint64_t);
    Bluetooth::Result control(uint8_t, uint64_t);
    Bluetooth::Result write(const uint8_t* p, size_t n, uint64_t)
    {
        return port.write(p, n);
    }
    Bluetooth::Result write(const uint8_t* p, size_t n)
    {
        return port.write(p, n);
    }
    Bluetooth::Result packet(const uint8_t*, size_t, uint64_t);
    Bluetooth::Result bootstrapPacket(const uint8_t*, size_t, uint64_t, uint64_t);
    Bluetooth::Result configure(uint32_t, bool);
    Bluetooth::Result runtimeBaud();
    Bluetooth::Result beginBaudChange()
    {
        port.receiveInterrupts(false);
        return port.flowControl(false);
    }
    Bluetooth::Result flush()
    {
        return Bluetooth::Result::Success;
    }
    Bluetooth::Result pulse(uint8_t p)
    {
        return port.write(&p, 1);
    }
    Bluetooth::Result drained()
    {
        return port.drained();
    }
    Bluetooth::Result reopen(uint32_t baud)
    {
        return configure(baud, true);
    }
    Bluetooth::Result serviceTX();
    template <class Sink> Bluetooth::Result receive(Sink& sink)
    {
        return port.receive(sink);
    }
};
