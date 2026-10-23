// SPDX-License-Identifier: BSD-2-Clause
#include "QcomBluetooth.h"
#include "QcomBluetoothUserClient.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <kern/clock.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

#define super IOService
OSDefineMetaClassAndStructors(QcomBluetooth, super);

bool QcomBluetooth::validProvider(IOService* provider)
{
    if (!provider || !provider->metaCast("AppleARMIODevice"))
        return false;
    auto* name = OSString::withCString("qcom,wcn3990-bt");
    if (!name)
        return false;
    const bool matches = IODTCompareNubName(provider, name, static_cast<OSString**>(nullptr));
    name->release();
    if (!matches || provider->getDeviceMemoryCount() != 1)
        return false;
    auto* memory = provider->getDeviceMemoryWithIndex(0);
    if (!memory || memory->getPhysicalAddress() != Bluetooth::UARTBase
        || memory->getLength() != Bluetooth::UARTSize)
        return false;
    auto* controllers = OSDynamicCast(OSArray, provider->getProperty("IOInterruptControllers"));
    auto* specifiers = OSDynamicCast(OSArray, provider->getProperty("IOInterruptSpecifiers"));
    if (!controllers || !specifiers || controllers->getCount() != 1 || specifiers->getCount() != 1
        || !OSDynamicCast(OSSymbol, controllers->getObject(0)))
        return false;
    auto* specifier = OSDynamicCast(OSData, specifiers->getObject(0));
    // The bridge and IODTMapInterrupts produce native little-endian cells.
    if (!specifier || specifier->getLength() != 8)
        return false;
    const auto* cells = static_cast<const uint8_t*>(specifier->getBytesNoCopy());
    return cells && Bluetooth::read32(cells) == Bluetooth::UARTInterrupt + 32
        && Bluetooth::read32(cells + 4) == 1; // level-triggered
}

IOService* QcomBluetooth::probe(IOService* provider, SInt32* score)
{
    return super::probe(provider, score) && validProvider(provider) ? this : nullptr;
}
uint64_t QcomBluetooth::uptimeMS()
{
    uint64_t absolute = 0, nanos = 0;
    clock_get_uptime(&absolute);
    absolutetime_to_nanoseconds(absolute, &nanos);
    return nanos / 1000000;
}
bool QcomBluetooth::start(IOService* provider)
{
    if (!super::start(provider))
        return false;
    if (!validProvider(provider))
        goto fail;
    fWorkLoop = IOWorkLoop::workLoop();
    fGate = IOCommandGate::commandGate(this);
    fTimer = IOTimerEventSource::timerEventSource(this, timerFired);
    fInterrupt = IOInterruptEventSource::interruptEventSource(this, interruptFired, provider, 0);
    if (!fWorkLoop || !fGate || !fTimer || !fInterrupt || fWorkLoop->addEventSource(fGate) != kIOReturnSuccess
        || fWorkLoop->addEventSource(fTimer) != kIOReturnSuccess
        || fWorkLoop->addEventSource(fInterrupt) != kIOReturnSuccess)
        goto fail;
    fInterrupt->disable();
    if (!fPlatform.bind(provider, this, fInterrupt))
        goto fail;
    // bluetoothd discovers this native DT nub by its service-plane name and
    // reads local-mac-address in the DeviceTree plane. The bridge supplies the
    // exact six bytes already checked against the NVM tag; no generated MAC.
    if (fPlatform.ready())
        provider->setName("bluetooth", gIOServicePlane);
    fController.attach();
    setProperty("BluetoothTransportVersion", Bluetooth::ProtocolVersion, 32);
    setProperty("BluetoothChipset", "WCN3990");
    setProperty("BluetoothBus", "UART-H4-IBS");
    setProperty("BluetoothNativeStackReady", false);
    setProperty("BluetoothNativeTransportReady", false);
    setProperty("BluetoothHardwareAccessEnabled", false);
    publishState();
    {
        static IOPMPowerState states[2] = { { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
            { 1, IOPMDeviceUsable, IOPMPowerOn, IOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 } };
        PMinit();
        fPMInitialized = true;
        provider->joinPMtree(this);
        if (registerPowerDriver(this, states, 2) != kIOReturnSuccess)
            goto fail;
    }
    fActive = true;
    registerService();
    fDesiredPower = fPlatform.ready();
    if (fDesiredPower)
        fTimer->setTimeoutMS(1);
    return true;
fail:
    cleanup();
    super::stop(provider);
    return false;
}
void QcomBluetooth::publishState()
{
    setProperty("BluetoothState", Bluetooth::stateName(fController.currentState()));
    setProperty("BluetoothTransportReady", fController.currentState() == Bluetooth::State::Ready);
    setProperty("BluetoothResourceMask", fController.resources(), 32);
    setProperty("BluetoothGeneration", fController.epoch(), 64);
    setProperty("BluetoothMissingRequirements",
        fPlatform.ready() ? Bluetooth::NativeStackABI : Bluetooth::InitialMissing, 32);
    setProperty("BluetoothHardwareAccessEnabled", fPlatform.hasUART());
    setProperty("BluetoothNativeTransportReady",
        fControl && fSerial && fController.currentState() == Bluetooth::State::Ready);
    setProperty("BluetoothLastFault", uint32_t(fLastFault), 32);
}
IOReturn QcomBluetooth::perform(uint32_t selector, uint64_t* output, uint64_t value)
{
    if (!fActive || isInactive())
        return kIOReturnNotAttached;
    if (selector == Bluetooth::GetInfo) {
        if (!output)
            return kIOReturnBadArgument;
        output[0] = Bluetooth::ProtocolVersion;
        output[1] = Bluetooth::UARTBase;
        output[2] = Bluetooth::UARTSize;
        output[3] = Bluetooth::UARTInterrupt;
        output[4] = Bluetooth::InitialBaud;
        output[5] = Bluetooth::OperationalBaud;
        output[6] = Bluetooth::MaxACLPayload;
        output[7] = fPlatform.ready() ? Bluetooth::NativeStackABI : Bluetooth::InitialMissing;
    } else if (selector == Bluetooth::GetStatus) {
        if (!output)
            return kIOReturnBadArgument;
        output[0] = uint32_t(fController.currentState());
        output[1] = fController.resources();
        output[2] = fController.epoch();
        output[3] = uint32_t(fController.lastResult());
        output[4] = fPlatform.ready() ? Bluetooth::NativeStackABI : Bluetooth::InitialMissing;
        output[5] = 0; // daemon operation is not inferred from tty publication
        output[6] = 0;
        output[7] = 0; // reserved, always zero in protocol v1
    } else if (selector == Bluetooth::GetBootstrap) {
        using namespace Bluetooth;
        if (!output)
            return kIOReturnBadArgument;
        for (unsigned i = 0; i < BootstrapScalars; ++i)
            output[i] = 0;
        output[BootstrapVersion] = 1;
        output[BootstrapGeneration] = fController.epoch();
        output[BootstrapState] = uint32_t(fController.currentState());
        output[BootstrapResources] = fController.resources();
        output[BootstrapFault] = uint32_t(fLastFault);
        output[BootstrapFlags] = (fPlatform.ready() ? BootstrapConfigured : 0)
            | (fSerial && fControl ? BootstrapEndpoints : 0)
            | (fPortAcquired || fControlSession.unit() ? BootstrapPortOpen : 0)
            | (fDesiredPower ? BootstrapPowerRequested : 0)
            | (!fTX.size() && !fTXInFlight && (!fPlatform.hasUART() || fPlatform.drained() == Result::Success)
                    ? BootstrapDrained : 0);
        output[BootstrapResult] = uint32_t(fController.lastResult());
        fPlatform.bootstrapPolicy(output);
    } else if (selector == Bluetooth::SetPower) {
        if (value > 1)
            return kIOReturnBadArgument;
        fDesiredPower = value != 0;
        if (value)
            fLastFault = Bluetooth::Result::Success;
        const auto result = value ? fController.start(uptimeMS()) : fController.stop();
        if (fDesiredPower || fController.resources())
            fTimer->setTimeoutMS(1);
        else
            fTimer->cancelTimeout();
        if (!fDesiredPower) {
            fPortAcquired = false;
            fTXInFlight = false;
            fRX.clear();
            fTX.clear();
            fGate->commandWakeup(&fPortBits);
        }
        publishState();
        switch (result) {
        case Bluetooth::Result::Success:
            return kIOReturnSuccess;
        case Bluetooth::Result::Pending:
            return kIOReturnSuccess;
        case Bluetooth::Result::NotReady:
            return kIOReturnNotReady;
        case Bluetooth::Result::Busy:
            return kIOReturnBusy;
        case Bluetooth::Result::Timeout:
            return kIOReturnTimeout;
        case Bluetooth::Result::Invalid:
            return kIOReturnBadArgument;
        case Bluetooth::Result::Unsupported:
            return kIOReturnUnsupported;
        default:
            return kIOReturnError;
        }
    } else
        return kIOReturnUnsupported;
    return kIOReturnSuccess;
}
IOReturn QcomBluetooth::gatedRequest(OSObject* owner, void* selector, void* output, void* value, void*)
{
    auto* driver = static_cast<QcomBluetooth*>(owner);
    return driver->perform(uint32_t(reinterpret_cast<uintptr_t>(selector)),
        static_cast<uint64_t*>(output), reinterpret_cast<uintptr_t>(value));
}
IOReturn QcomBluetooth::request(uint32_t selector, uint64_t* output, uint64_t value)
{
    // Gate lifetime extends to free(); retained clients may outlive stop().
    return fGate ? fGate->runAction(gatedRequest, reinterpret_cast<void*>(uintptr_t(selector)), output,
                       reinterpret_cast<void*>(uintptr_t(value)))
                 : kIOReturnNotAttached;
}
IOReturn QcomBluetooth::gatedBootstrapPower(OSObject* object, void* operation, void* epoch, void* output, void*)
{
    using namespace Bluetooth;
    auto* driver = static_cast<QcomBluetooth*>(object);
    const auto on = reinterpret_cast<uintptr_t>(operation);
    if (!driver->fActive || driver->isInactive())
        return kIOReturnNotAttached;
    if (on > 1 || !output)
        return kIOReturnBadArgument;
    // Compare and change under the same gate. A stale XPC request must never
    // turn off a later controller generation or steal a daemon/tty session.
    if (reinterpret_cast<uintptr_t>(epoch) != driver->fController.epoch())
        return kIOReturnNotReady;
    if (driver->fPortAcquired || driver->fControlSession.unit())
        return kIOReturnBusy;
    const auto state = driver->fController.currentState();
    if (on && (state == State::Fault || state == State::Stopping || state == State::Detached))
        return kIOReturnNotReady;
    IOReturn result = kIOReturnSuccess;
    if (!(on && driver->fDesiredPower && (state == State::Starting || state == State::WaitingForReset)))
        result = driver->perform(SetPower, nullptr, on);
    if (result != kIOReturnSuccess) {
        // The power request was admitted, but synchronous hardware progress
        // failed. Report its generation so user space can cancel only this
        // attempt; completion/failure remains visible in the snapshot.
        driver->fLastFault = BluetoothPlatformBackend::convert(result);
        driver->fDesiredPower = false;
        if (driver->fController.resources())
            driver->fTimer->setTimeoutMS(1);
        driver->publishState();
    }
    *static_cast<uint64_t*>(output) = driver->fController.epoch();
    return kIOReturnSuccess;
}
IOReturn QcomBluetooth::bootstrapPower(uint64_t on, uint64_t epoch, uint64_t* accepted)
{
    return fGate ? fGate->runAction(gatedBootstrapPower, reinterpret_cast<void*>(uintptr_t(on)),
        reinterpret_cast<void*>(uintptr_t(epoch)), accepted) : kIOReturnNotAttached;
}
void QcomBluetooth::timerFired(OSObject* owner, IOTimerEventSource*)
{
    auto* driver = static_cast<QcomBluetooth*>(owner);
    if (!driver->fActive)
        return;
    driver->pump();
}
void QcomBluetooth::interruptFired(OSObject* owner, IOInterruptEventSource*, int)
{
    auto* driver = static_cast<QcomBluetooth*>(owner);
    if (driver->fActive)
        driver->pump();
}
Bluetooth::Result QcomBluetooth::ReceiveSink::bytes(const uint8_t* p, size_t bytes)
{
    const auto result = owner->fController.receive(owner->fController.epoch(), uptimeMS(), p, bytes);
    // WCN3990 emits an unreliable vendor indication during the baud switch.
    // Stop this FIFO visit as soon as the version handler deasserts RTS. RX
    // remains masked until runtimeBaud resets the packing buffer and decoder.
    return result == Bluetooth::Result::Success && !owner->fPlatform.wantsHCI() ? Bluetooth::Result::Pending
                                                                                : result;
}
void QcomBluetooth::pump()
{
    using namespace Bluetooth;
    const uint64_t time = uptimeMS();
    if (fDesiredPower && fController.currentState() == State::Off && time >= fRetryAt) {
        const auto r = fController.start(time);
        fRetryAt = time + 1000;
        if (r != Result::Success && r != Result::Pending && r != Result::NotReady)
            fLastFault = r;
    }
    if (fPlatform.hasUART()) {
        auto r = fPlatform.serviceTX();
        if (r == Result::Success || r == Result::Pending) {
            if (fPlatform.wantsHCI()) {
                ReceiveSink sink { this };
                r = fPlatform.receive(sink);
            }
        }
        if (r != Result::Success && r != Result::Pending && fController.currentState() != State::Stopping) {
            fLastFault = r;
            fController.transportFault(r);
            fDesiredPower = false;
        }
    }
    if (!fDesiredPower && fController.currentState() != State::Detached
        && fController.currentState() != State::Off)
        fController.stop();
    const auto result = fController.tick(fController.epoch(), time);
    if (result != Result::Success && result != Result::Pending && result != Result::Ignored) {
        fLastFault = result;
        if (result != Result::NotReady)
            fDesiredPower = false;
    }
    if (fPlatform.takeFramingReset())
        fController.resetBootstrapFraming();
    if (fController.currentState() == State::Ready) {
        publishControl();
        publishSerial();
        flushControlRX();
        if (fTXInFlight && fPlatform.drained() == Result::Success) {
            fTXInFlight = false;
            fController.transmitDrained(time);
            fGate->commandWakeup(&fPortBits);
        }
        if (!fTXInFlight && fPortAcquired && (fPortBits & Active) && (fPortBits & TXEnable) && fTX.size()) {
            const auto header = headerBytes(fTX.peek(0));
            if (!header || fTX.peek(0) == Event) {
                fController.transportFault(Result::Invalid);
                fLastFault = Result::Invalid;
            } else if (fTX.size() >= header) {
                const auto payload = fTX.peek(0) == ACL ? unsigned(fTX.peek(3)) | (unsigned(fTX.peek(4)) << 8)
                                                        : unsigned(fTX.peek(header - 1));
                const auto length = header + payload;
                if (length > MaxH4Frame) {
                    fController.transportFault(Result::Invalid);
                    fLastFault = Result::Invalid;
                } else if (fTX.size() >= length) {
                    for (size_t i = 0; i < length; ++i)
                        fTXFrame[i] = fTX.peek(i);
                    const auto r = fController.transmit(fController.epoch(), time, fTXFrame, length);
                    if (r == Result::Success) {
                        fTX.pop(nullptr, length);
                        fTXInFlight = true;
                        fGate->commandWakeup(&fPortBits);
                    } else if (r != Result::Pending && r != Result::Busy)
                        fLastFault = r;
                }
            }
        }
    }
    if (fController.currentState() == State::Fault) {
        fDesiredPower = false;
        fPortAcquired = false;
        fTXInFlight = false;
        fRX.clear();
        fTX.clear();
        fGate->commandWakeup(&fPortBits);
    }
    if (fPowerAck && !fController.resources()) {
        fPowerAck = false;
        acknowledgeSetPowerState();
    }
    publishState();
    if (fDesiredPower || fController.resources())
        fTimer->setTimeoutMS(5);
}
IOReturn QcomBluetooth::setPowerState(unsigned long state, IOService*)
{
    if (state > 1)
        return kIOReturnBadArgument;
    return fGate ? fGate->runAction(gatedPower, reinterpret_cast<void*>(uintptr_t(state))) : IOPMAckImplied;
}
IOReturn QcomBluetooth::gatedPower(OSObject* object, void* argument, void*, void*, void*)
{
    auto* driver = static_cast<QcomBluetooth*>(object);
    if (!driver->fActive)
        return IOPMAckImplied;
    const bool on = reinterpret_cast<uintptr_t>(argument) != 0;
    if (on) {
        if (driver->fPlatform.ready())
            driver->perform(Bluetooth::SetPower, nullptr, 1);
    } else {
        driver->perform(Bluetooth::SetPower, nullptr, 0);
        if (driver->fController.resources()) {
            driver->fPowerAck = true;
            return 2000000;
        }
    }
    return IOPMAckImplied;
}
IOWorkLoop* QcomBluetooth::getWorkLoop() const
{
    return fWorkLoop;
}
IOReturn QcomBluetooth::newUserClient(
    task_t task, void* token, UInt32 type, OSDictionary* properties, IOUserClient** handler)
{
    if (!handler)
        return kIOReturnBadArgument;
    *handler = nullptr;
    if (type != Bluetooth::DiagnosticClient && type != Bluetooth::BootstrapClient)
        return kIOReturnUnsupported;
    // initWithTask enforces the separate administrator/bootstrap identities.
    if (isInactive())
        return kIOReturnNotAttached;
    auto* client = new QcomBluetoothUserClient;
    if (!client)
        return kIOReturnNoMemory;
    if (!client->initWithTask(task, token, type, properties)) {
        client->release();
        return kIOReturnError;
    }
    if (!client->attach(this)) {
        client->release();
        return kIOReturnNotAttached;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnNotReady;
    }
    *handler = client;
    return kIOReturnSuccess;
}
IOReturn QcomBluetooth::gatedStop(OSObject* owner, void*, void*, void*, void*)
{
    auto* driver = static_cast<QcomBluetooth*>(owner);
    driver->fActive = false;
    if (driver->fTimer)
        driver->fTimer->cancelTimeout();
    if (driver->fInterrupt)
        driver->fInterrupt->disable();
    driver->fPortAcquired = false;
    driver->fGate->commandWakeup(&driver->fPortBits);
    const auto result = driver->fController.detach();
    driver->publishState();
    return result == Bluetooth::Result::Success ? kIOReturnSuccess : kIOReturnError;
}
void QcomBluetooth::cleanup()
{
    if (fPMInitialized) {
        PMstop();
        fPMInitialized = false;
    }
    if (fGate && fGate->getWorkLoop())
        fGate->runAction(gatedStop);
}
void QcomBluetooth::stop(IOService* provider)
{
    cleanup();
    super::stop(provider);
}
bool QcomBluetooth::requestTerminate(IOService*, IOOptionBits)
{
    return !fGate || fGate->runAction(gatedTerminate) == kIOReturnSuccess;
}
IOReturn QcomBluetooth::gatedTerminate(OSObject* object, void*, void*, void*, void*)
{
    auto* driver = static_cast<QcomBluetooth*>(object);
    driver->perform(Bluetooth::SetPower, nullptr, 0);
    return driver->fController.resources() || !driver->unpublishControl() ? kIOReturnBusy : kIOReturnSuccess;
}
void QcomBluetooth::free()
{
    cleanup();
    if (fInterrupt && fInterrupt->getWorkLoop())
        fWorkLoop->removeEventSource(fInterrupt);
    if (fTimer && fTimer->getWorkLoop())
        fWorkLoop->removeEventSource(fTimer);
    if (fGate && fGate->getWorkLoop())
        fWorkLoop->removeEventSource(fGate);
    OSSafeReleaseNULL(fSerial);
    fPlatform.unbind();
    OSSafeReleaseNULL(fInterrupt);
    OSSafeReleaseNULL(fTimer);
    OSSafeReleaseNULL(fGate);
    OSSafeReleaseNULL(fWorkLoop);
    super::free();
}
