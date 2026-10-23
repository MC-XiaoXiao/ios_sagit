// SPDX-License-Identifier: BSD-2-Clause
#include "BluetoothPlatform.h"
#include "QcomBluetooth.h"
#include <ClockControllerProtocol.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/c++/OSData.h>
using Bluetooth::Result;

uint32_t BluetoothPlatformBackend::Registers::read(uint32_t offset)
{
    if (!base || (offset & 3) || offset >= Bluetooth::UARTSize)
        return UINT32_MAX;
    const uint32_t value = base[offset / 4];
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}
void BluetoothPlatformBackend::Registers::write(uint32_t offset, uint32_t value)
{
    if (!base || (offset & 3) || offset >= Bluetooth::UARTSize)
        return;
    __asm__ volatile("dmb oshst" ::: "memory");
    base[offset / 4] = value;
    __asm__ volatile("dsb oshst" ::: "memory");
}
uint64_t BluetoothPlatformBackend::now()
{
    uint64_t absolute = 0, nanoseconds = 0;
    clock_get_uptime(&absolute);
    absolutetime_to_nanoseconds(absolute, &nanoseconds);
    return nanoseconds / 1000000;
}
Result BluetoothPlatformBackend::convert(IOReturn r)
{
    switch (r) {
    case kIOReturnSuccess:
        return Result::Success;
    case kIOReturnBusy:
        return Result::Busy;
    case kIOReturnNotReady:
    case kIOReturnNotFound:
        return Result::NotReady;
    case kIOReturnUnsupported:
    case kIOReturnNotPermitted:
        return Result::Unsupported;
    case kIOReturnBadArgument:
        return Result::Invalid;
    case kIOReturnTimeout:
        return Result::Timeout;
    default:
        return Result::Fault;
    }
}
bool BluetoothPlatformBackend::bind(IOService* p, QcomBluetooth* o, IOInterruptEventSource* source)
{
    provider = p;
    owner = o;
    irq = source;
    bus = provider->getProvider();
    const auto* data = OSDynamicCast(OSData, p->getProperty("qcom,bluetooth-config"));
    if (!data)
        return !p->getProperty("qcom,bluetooth-config"); // diagnostics only
    if (data->getLength() != sizeof(config) || !data->getBytesNoCopy() || !bus)
        return false;
    memcpy(&config, data->getBytesNoCopy(), sizeof(config));
    if (!Bluetooth::validConfiguration(config))
        return false;
    auto* patchSource = OSDynamicCast(OSData, p->getProperty("bluetooth-patch"));
    auto* nvmSource = OSDynamicCast(OSData, p->getProperty("bluetooth-nvm"));
    if (!patchSource || !nvmSource || patchSource->getLength() != config.patchBytes
        || nvmSource->getLength() != config.nvmBytes)
        return false;
    patchData = OSData::withBytes(patchSource->getBytesNoCopy(), patchSource->getLength());
    nvmData = OSData::withBytes(nvmSource->getBytesNoCopy(), nvmSource->getLength());
    if (!patchData || !nvmData
        || Bluetooth::parseFirmware(static_cast<const uint8_t*>(patchData->getBytesNoCopy()),
               patchData->getLength(), Bluetooth::FirmwareType::Patch, patch)
            != Result::Success
        || Bluetooth::parseFirmware(static_cast<const uint8_t*>(nvmData->getBytesNoCopy()),
               nvmData->getLength(), Bluetooth::FirmwareType::NVM, nvm)
            != Result::Success
        || Bluetooth::read16(patch.data + 16) != uint16_t(config.product)
        || Bluetooth::read16(patch.data + 18) != config.rom || !Bluetooth::transportNVM(nvm))
        return false;
    const auto* address = OSDynamicCast(OSData, p->getProperty("local-mac-address"));
    if (!address || address->getLength() != 6
        || !Bluetooth::localAddress(nvm, static_cast<const uint8_t*>(address->getBytesNoCopy())))
        return false;
    pins.owner = o;
    pins.count = 4;
    for (unsigned i = 0; i < 4; ++i) {
        pins.pins[i] = 45 + i;
        pins.configuration[i] = config.pinConfiguration[i];
    }
    configured = true;
    return true;
}
void BluetoothPlatformBackend::unbind()
{
    // requestTerminate/PM keep this owner alive until all hardware leases have
    // gone. Never turn an ambiguous stop into permission to unmap registers.
    if (railMask || clockMask || pins.token || port.enabled() || extraClock || uartClockToken)
        return;
    OSSafeReleaseNULL(map);
    OSSafeReleaseNULL(gpio);
    OSSafeReleaseNULL(patchData);
    OSSafeReleaseNULL(nvmData);
    registers.base = nullptr;
    provider = bus = nullptr;
    owner = nullptr;
}
Result BluetoothPlatformBackend::clock(uint32_t op, uint32_t id, uint64_t& value, unsigned controller)
{
    if (!bus)
        return Result::NotReady;
    auto request = ClockController::targeted(ClockController::Controller(controller),
        ClockController::Operation(op), id, value, ClockController::BluetoothConsumer);
    const auto* function = OSSymbol::withCStringNoCopy(ClockController::Function);
    if (!function)
        return Result::Fault;
    const auto r
        = convert(bus->callPlatformFunction(function, false, &request.request, nullptr, nullptr, nullptr));
    function->release();
    if (!ClockController::valid(request.request)
        || request.controller != ClockController::Controller(controller)
        || request.consumer != ClockController::BluetoothConsumer
        || request.request.operation != ClockController::Operation(op) || request.request.id != id
        || request.flags || request.reserved || request.request.name) {
        quarantined = true;
        return Result::Fault;
    }
    if (r == Result::Fault)
        quarantined = true;
    value = request.request.value;
    return r;
}
Result BluetoothPlatformBackend::pinRequest(Pinmux::Action action)
{
    if (!gpio) {
        auto* matching = IOService::serviceMatching("QcomGPIO");
        OSIterator* iterator = nullptr;
        if (!matching)
            return Result::Fault;
        iterator = IOService::getMatchingServices(matching);
        matching->release();
        if (!iterator)
            return Result::NotReady;
        auto* first = OSDynamicCast(IOService, iterator->getNextObject());
        if (first && !iterator->getNextObject() && first->getProvider()
            && first->getProvider()->getProvider() == bus) {
            gpio = first;
            gpio->retain();
        }
        iterator->release();
        if (!gpio)
            return Result::NotReady;
    }
    pins.action = action;
    const auto* function = OSSymbol::withCStringNoCopy(Pinmux::Function);
    if (!function)
        return Result::Fault;
    const auto r = convert(gpio->callPlatformFunction(function, false, &pins, nullptr, nullptr, nullptr));
    function->release();
    return r;
}
Result BluetoothPlatformBackend::configure(uint32_t speed, bool flow)
{
    const auto* baud = Bluetooth::UARTDM::baud(speed);
    if (!baud || !map || !clockMask)
        return Result::NotReady;
    if (port.enabled() && port.drained() != Result::Success)
        return Result::Busy;
    auto result = port.stop();
    if (result != Result::Success)
        return result;
    // Gate the leaf before programming the RCG. GCC deliberately refuses
    // changes while any sibling or consumer still holds the root active.
    if (!uartClockToken) {
        result = clock(ClockController::AdoptClock, 53, uartClockToken);
        if (result != Result::Success)
            return result;
        if (!uartClockToken) {
            quarantined = true;
            return Result::Fault;
        }
    }
    if (extraClock) {
        uint64_t value = 0;
        result = clock(ClockController::ReleaseClock, 53, value);
        if (result != Result::Success)
            return result;
        extraClock = releaseVote = false;
    }
    if (clockMask & 2) {
        uint64_t value = 0;
        result = clock(ClockController::ReleaseClock, 53, value);
        if (result != Result::Success)
            return result;
        clockMask &= ~2U;
    }
    uint64_t rate = baud->clock;
    result = clock(ClockController::SetRate, 14, rate);
    if (result == Result::Success)
        result = clock(ClockController::GetRate, 14, rate);
    if (result != Result::Success || rate != baud->clock)
        return result == Result::Success ? Result::Fault : result;
    uint64_t value = 0;
    result = clock(ClockController::RetainClock, 53, value);
    if (result != Result::Success)
        return result;
    clockMask |= 2;
    result = port.configure(speed, flow);
    port.receiveInterrupts(loaderStarted);
    return result;
}
void BluetoothPlatformBackend::bootstrapPolicy(uint64_t* values) const
{
    using namespace Bluetooth;
    if (!configured || !values)
        return;
    values[BootstrapProduct] = config.product;
    values[BootstrapSoC] = config.soc;
    values[BootstrapROM] = config.rom;
    values[BootstrapPatch] = read16(patch.data + 20);
    for (size_t pos = 4; pos < nvm.bytes;) {
        const auto tag = read16(nvm.data + pos), bytes = read16(nvm.data + pos + 2);
        if (tag == 2 && bytes == 6) {
            uint64_t address = 0;
            for (unsigned i = 0; i < 6; ++i)
                address = (address << 8) | nvm.data[pos + 12 + 5 - i];
            values[BootstrapAddress] = address;
            break;
        }
        pos += 12 + bytes; // bind() validated the complete, immutable TLV
    }
}
Result BluetoothPlatformBackend::runtimeBaud()
{
    const auto r = configure(Bluetooth::OperationalBaud, true);
    if (r == Result::Success)
        resetFraming = true;
    return r;
}
Result BluetoothPlatformBackend::acquire(Bluetooth::Resource resource, uint64_t)
{
    using namespace Bluetooth;
    using namespace ClockController;
    if (!configured)
        return Result::NotReady;
    if (quarantined)
        return Result::Fault;
    if (resource == Rails) {
        for (unsigned i = 0; i < config.railCount; ++i)
            if (!(railMask & (1U << i))) {
                uint64_t voltage = config.rails[i].voltage;
                const auto r = clock(SetVoltage, config.rails[i].id, voltage, RPM);
                if (r != Result::Success) {
                    if (r == Result::Timeout || r == Result::Fault)
                        quarantined = true;
                    return railMask || quarantined ? Result::Fault : r;
                }
                railMask |= 1U << i;
            }
        return Result::Success;
    }
    if (resource == ReferenceClock) {
        if (!clockMask) {
            const auto r = clock(GetRate, 14, originalRate);
            if (r != Result::Success)
                return r;
        }
        const uint32_t ids[] = { 37, 53 };
        for (unsigned i = 0; i < 2; ++i)
            if (!(clockMask & (1U << i))) {
                uint64_t value = 0;
                const auto r = clock(RetainClock, ids[i], value);
                if (r != Result::Success)
                    return clockMask ? Result::Fault : r;
                clockMask |= 1U << i;
            }
        return Result::Success;
    }
    if (resource == Pins)
        return pins.token ? pinRequest(Pinmux::Action::Validate) : pinRequest(Pinmux::Action::Acquire);
    if (resource == UART) {
        if (port.enabled())
            return Result::Success;
        map = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
        if (!map || map->getLength() != UARTSize || map->getPhysicalAddress() != UARTBase
            || !map->getVirtualAddress())
            return Result::Fault;
        registers.base = reinterpret_cast<volatile uint32_t*>(map->getVirtualAddress());
        if (port.takeover() != Result::Success)
            return Result::Fault;
        const auto result = configure(InitialBaud, false);
        if (result == Result::Success)
            irq->enable();
        // The mapping is owned even when configuring the UART fails. The
        // controller must run UART release before releasing pins and clocks.
        return result == Result::Success ? result : Result::Fault;
    }
    if (resource == BootROM) {
        if (!bootStarted) {
            boot.resetAfterQuiesce();
            bootStarted = true;
            return boot.begin(now());
        }
        return boot.advance(now());
    }
    if (resource == Firmware) {
        if (!loaderStarted) {
            loader.resetAfterQuiesce();
            loaderStarted = true;
            port.receiveInterrupts(true);
            return loader.begin(patch, nvm, { config.product, config.soc, uint16_t(config.rom) }, now());
        }
        return loader.advance(now());
    }
    return Result::Unsupported;
}
Result BluetoothPlatformBackend::release(Bluetooth::Resource resource, uint64_t)
{
    using namespace Bluetooth;
    using namespace ClockController;
    if (quarantined)
        return Result::Fault;
    if (resource == Firmware) {
        loaderStarted = false;
        port.receiveInterrupts(false);
        loader.resetAfterQuiesce();
        return Result::Success;
    }
    if (resource == BootROM) {
        if (!bootStarted)
            return Result::Success;
        if (!map)
            return Result::Fault;
        const uint64_t time = now();
        if (!shutdownPhase) {
            port.stop();
            const auto r = configure(2400, false);
            if (r != Result::Success)
                return r;
            if (pulse(0xc0) != Result::Success || !deadlineAfter(time, 1000, shutdownDeadline))
                return Result::Fault;
            shutdownPhase = 1;
            return Result::Pending;
        }
        if (shutdownPhase == 1) {
            if (time >= shutdownDeadline)
                return Result::Timeout;
            const auto r = port.drained();
            if (r != Result::Success)
                return r;
            shutdownPhase = 2;
            if (!deadlineAfter(time, 10, shutdownDeadline))
                return Result::Fault;
            return Result::Pending;
        }
        if (time < shutdownDeadline)
            return Result::Pending;
        bootStarted = false;
        shutdownPhase = 0;
        boot.resetAfterQuiesce();
        return Result::Success;
    }
    if (resource == UART) {
        irq->disable();
        const auto r = port.stop();
        if (r != Result::Success)
            return r;
        registers.base = nullptr;
        OSSafeReleaseNULL(map);
        return Result::Success;
    }
    if (resource == Pins)
        return pins.token ? pinRequest(Pinmux::Action::Release) : Result::Success;
    if (resource == ReferenceClock) {
        if (extraClock) {
            uint64_t value = 0;
            const auto r = clock(ReleaseClock, 53, value);
            if (r != Result::Success)
                return r;
            extraClock = releaseVote = false;
        }
        if (clockMask & 2) {
            uint64_t value = 0;
            const auto r = clock(ReleaseClock, 53, value);
            if (r != Result::Success)
                return r;
            clockMask &= ~2U;
        }
        if (uartClockToken) {
            auto r = clock(RestoreClock, 53, uartClockToken);
            if (r != Result::Success)
                return r;
            if (uartClockToken) {
                quarantined = true;
                return Result::Fault;
            }
            originalRate = 0;
        }
        const uint32_t ids[] = { 37, 53 };
        for (unsigned i = 2; i; --i)
            if (clockMask & (1U << (i - 1))) {
                uint64_t value = 0;
                const auto r = clock(ReleaseClock, ids[i - 1], value);
                if (r != Result::Success)
                    return r;
                clockMask &= ~(1U << (i - 1));
            }
        return Result::Success;
    }
    if (resource == Rails) {
        for (unsigned i = config.railCount; i; --i)
            if (railMask & (1U << (i - 1))) {
                uint64_t value = 0;
                const auto r = clock(SetVoltage, config.rails[i - 1].id, value, RPM);
                if (r != Result::Success)
                    return r;
                railMask &= ~(1U << (i - 1));
            }
        return Result::Success;
    }
    return Result::Unsupported;
}
Result BluetoothPlatformBackend::vote(bool on, uint64_t)
{
    if (on) {
        releaseVote = false;
        if (extraClock)
            return Result::Success;
        uint64_t value = 0;
        const auto r = clock(ClockController::RetainClock, 53, value);
        if (r == Result::Success)
            extraClock = true;
        return r;
    }
    // Keep the physical reference until the queued sleep/ack byte left the
    // wire. The resource-level UART clock remains retained throughout HCI use.
    releaseVote = extraClock;
    return Result::Success;
}
Result BluetoothPlatformBackend::control(uint8_t byte, uint64_t)
{
    return port.write(&byte, 1);
}
Result BluetoothPlatformBackend::serviceTX()
{
    const auto r = port.serviceTX();
    if (releaseVote && port.drained() == Result::Success) {
        uint64_t value = 0;
        const auto released = clock(ClockController::ReleaseClock, 53, value);
        if (released != Result::Success)
            return released;
        releaseVote = extraClock = false;
    }
    return r;
}
Result BluetoothPlatformBackend::packet(const uint8_t* p, size_t bytes, uint64_t)
{
    return owner->receivedPacket(p, bytes);
}
Result BluetoothPlatformBackend::bootstrapPacket(const uint8_t* p, size_t bytes, uint64_t, uint64_t time)
{
    return loader.receive(p, bytes, time);
}
