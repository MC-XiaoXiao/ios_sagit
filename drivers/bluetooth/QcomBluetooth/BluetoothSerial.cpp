// SPDX-License-Identifier: BSD-2-Clause
#include "QcomBluetooth.h"
#include <kern/thread.h>
using namespace Bluetooth;
uint32_t QcomBluetooth::portState() const
{
    uint32_t state = fPortBits | (fPortAcquired ? Acquired : 0);
    if (!fTX.size() && !fTXInFlight)
        state |= TXEmpty;
    if (fTX.size() <= fSerialSettings.txLow)
        state |= TXLow;
    if (fTX.size() >= fSerialSettings.txHigh)
        state |= TXHigh;
    if (!fTX.available())
        state |= TXFull;
    if (fTXInFlight)
        state |= TXBusy;
    if (!fRX.size())
        state |= RXEmpty;
    if (fRX.size() <= fSerialSettings.rxLow)
        state |= RXLow;
    if (fRX.size() >= fSerialSettings.rxHigh)
        state |= RXHigh;
    if (!fRX.available())
        state |= RXFull;
    return state;
}
IOReturn QcomBluetooth::serial(SerialRequest& r)
{
    return fGate ? fGate->runAction(gatedSerial, &r) : kIOReturnNotAttached;
}
IOReturn QcomBluetooth::gatedSerial(OSObject* owner, void* request, void*, void*, void*)
{
    return static_cast<QcomBluetooth*>(owner)->performSerial(*static_cast<SerialRequest*>(request));
}
IOReturn QcomBluetooth::performSerial(SerialRequest& r)
{
    if (fControlSession.unit())
        return kIOReturnExclusiveAccess;
    auto online = [&] {
        return fActive && !isInactive() && fController.currentState() == State::Ready && fDesiredPower;
    };
    auto wait = [&] { return fGate->commandSleep(&fPortBits, THREAD_ABORTSAFE) == THREAD_AWAKENED; };
    if (!online())
        return kIOReturnOffline;
    if (r.operation == Acquire) {
        while (fPortAcquired) {
            if (!r.sleep)
                return kIOReturnExclusiveAccess;
            if (!wait())
                return kIOReturnAborted;
            if (!online())
                return kIOReturnOffline;
        }
        fPortAcquired = true;
        fPortBits |= Active | TXEnable | RXEnable;
        fRX.clear();
        fTX.clear();
        return kIOReturnSuccess;
    }
    if (r.operation == Release) {
        if (!fPortAcquired)
            return kIOReturnNotOpen;
        // Do not discard a partial outbound H4 frame and reuse its wire
        // prefix on the next open. Close requests a complete controller stop.
        if (fTX.size() || fTXInFlight) {
            fDesiredPower = false;
            fController.stop();
            fTimer->setTimeoutMS(1);
        }
        fPortAcquired = false;
        fPortBits &= ~Active;
        fRX.clear();
        fTX.clear();
        fTXInFlight = false;
        fGate->commandWakeup(&fPortBits);
        return kIOReturnSuccess;
    }
    if (r.operation == GetState) {
        if (!r.output)
            return kIOReturnBadArgument;
        *r.output = portState();
        return kIOReturnSuccess;
    }
    if (r.operation == SetState) {
        constexpr uint32_t allowed = TXEnable | RXEnable | 0x6;
        if (r.mask & ~allowed)
            return kIOReturnBadArgument;
        fPortBits = (fPortBits & ~r.mask) | (r.value & r.mask);
        fGate->commandWakeup(&fPortBits);
        return kIOReturnSuccess;
    }
    if (r.operation == WatchState) {
        if (!r.output || !r.mask)
            return kIOReturnBadArgument;
        const uint32_t expected = *r.output;
        while (!((~(portState() ^ expected)) & r.mask)) {
            if (!wait())
                return kIOReturnAborted;
            if (!online())
                return kIOReturnOffline;
        }
        *r.output = portState();
        return kIOReturnSuccess;
    }
    if (r.operation == NextEvent) {
        if (!r.output)
            return kIOReturnBadArgument;
        *r.output = 0;
        return kIOReturnSuccess;
    }
    if (r.operation == DequeueEvent) {
        if (!r.output || !r.count)
            return kIOReturnBadArgument;
        *r.output = *r.count = 0;
        return kIOReturnSuccess; // bytes are delivered by dequeueData
    }
    if (r.operation == ExecuteEvent || r.operation == EnqueueEvent || r.operation == RequestEvent) {
        const bool query = r.operation == RequestEvent;
        if (query && !r.output)
            return kIOReturnBadArgument;
        uint32_t value = r.mask;
        switch (r.value) {
        case PortActive:
            if (query)
                value = (fPortBits & Active) != 0;
            else if (value <= 1)
                fPortBits = value ? fPortBits | Active : fPortBits & ~Active;
            else
                return kIOReturnBadArgument;
            break;
        case TXAvailable:
        case RXAvailable:
            if (!query)
                return kIOReturnUnsupported;
            value = uint32_t(r.value == TXAvailable ? fTX.available() : fRX.size());
            break;
        case TXFlush:
            // Whole H4 frames already admitted to the UART finish draining;
            // flush only the bytes still owned by the virtual tty queue.
            if (!query)
                fTX.clear();
            value = 0;
            break;
        case RXFlush:
            if (!query)
                fRX.clear();
            value = 0;
            break;
        default:
            switch (fSerialSettings.event(r.value, query, value)) {
            case Result::Success:
                break;
            case Result::Invalid:
                return kIOReturnBadArgument;
            default:
                return kIOReturnUnsupported;
            }
        }
        if (query)
            *r.output = value;
        fGate->commandWakeup(&fPortBits);
        return kIOReturnSuccess;
    }
    if (r.operation != EnqueueData && r.operation != DequeueData)
        return kIOReturnUnsupported;
    if (!r.count || (!r.buffer && r.size) || r.minimum > r.size)
        return kIOReturnBadArgument;
    *r.count = 0;
    if (!r.size)
        return kIOReturnSuccess;
    if (!fPortAcquired)
        return kIOReturnNotOpen;
    if (r.operation == EnqueueData) {
        while (*r.count < r.size) {
            if (!online() || !fPortAcquired)
                return kIOReturnOffline;
            const auto n = fTX.push(r.buffer + *r.count, r.size - *r.count);
            *r.count += uint32_t(n);
            fTimer->setTimeoutMS(1);
            if (*r.count == r.size || !r.sleep)
                break;
            if (!wait())
                return kIOReturnAborted;
        }
    } else {
        while (*r.count < r.minimum || (!*r.count && fRX.size())) {
            if (!online() || !fPortAcquired)
                return kIOReturnOffline;
            *r.count += uint32_t(fRX.pop(r.buffer + *r.count, r.size - *r.count));
            if (*r.count >= r.minimum)
                break;
            if (!wait())
                return kIOReturnAborted;
        }
        if (!r.minimum && !*r.count)
            *r.count = uint32_t(fRX.pop(r.buffer, r.size));
    }
    fGate->commandWakeup(&fPortBits);
    return kIOReturnSuccess;
}
Result QcomBluetooth::receivedPacket(const uint8_t* p, size_t bytes)
{
    if (!Bluetooth::validPacket(p, bytes, false) || fRX.available() < bytes)
        return Result::Fault;
    fRX.push(p, bytes);
    fGate->commandWakeup(&fPortBits);
    return Result::Success;
}
void QcomBluetooth::publishSerial()
{
    if (fSerial || fController.currentState() != State::Ready)
        return;
    auto* serial = new QcomBluetoothSerial;
    if (!serial || !serial->init()) {
        if (serial)
            serial->release();
        return;
    }
    if (!serial->attach(this)) {
        serial->release();
        return;
    }
    if (!serial->start(this)) {
        serial->detach(this);
        serial->release();
        return;
    }
    fSerial = serial;
    setProperty("BluetoothNativeTransportReady", true);
}
