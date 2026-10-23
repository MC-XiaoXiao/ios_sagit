// SPDX-License-Identifier: BSD-2-Clause
#include "QcomBluetooth.h"
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSBoolean.h>
#include <sys/errno.h>
#include <sys/kpi_mbuf.h>
#include <sys/termios.h>
#include <sys/ttycom.h>

using namespace Bluetooth;
static_assert(sizeof(termios) == sizeof(NativeTermios), "native UART termios size");
static_assert(
    offsetof(termios, c_ispeed) == offsetof(NativeTermios, inputSpeed), "native UART termios layout");
static_assert(TIOCGETA == NativeGetTermios && TIOCSETA == NativeSetTermios, "native UART socket options");
static_assert(sizeof(ctl_info) == sizeof(NativeControlInfo) && CTLIOCGINFO == NativeControlGetInfo,
    "native control lookup ABI");
static_assert(sizeof(sockaddr_ctl) == sizeof(NativeControlAddress) &&
    offsetof(sockaddr_ctl,sc_unit) == offsetof(NativeControlAddress,unit), "native control address ABI");
QcomBluetooth* QcomBluetooth::sControlOwner = nullptr;

void QcomBluetooth::publishControl()
{
    if (fControl || fController.currentState() != State::Ready)
        return;
    retain();
    if (!OSCompareAndSwapPtr(nullptr, this, reinterpret_cast<void* volatile*>(&sControlOwner))) {
        release();
        return;
    }
    kern_ctl_reg registration { };
    strlcpy(registration.ctl_name, NativeControlName, sizeof(registration.ctl_name));
    registration.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;
    registration.ctl_sendsize = registration.ctl_recvsize = SerialCapacity;
    registration.ctl_connect = controlConnect;
    registration.ctl_disconnect = controlDisconnect;
    registration.ctl_send = controlSend;
    registration.ctl_getopt = controlGet;
    registration.ctl_setopt = controlSet;
    const auto error = ctl_register(&registration, &fControl);
    if (error) {
        fControl = nullptr;
        OSCompareAndSwapPtr(this, nullptr, reinterpret_cast<void* volatile*>(&sControlOwner));
        release();
        return;
    }
    setProperty("BluetoothNativeControl", NativeControlName);
}
bool QcomBluetooth::unpublishControl()
{
    if (!fControl)
        return true;
    if (fControlSession.unit() || ctl_deregister(fControl))
        return false;
    fControl = nullptr;
    OSCompareAndSwapPtr(this, nullptr, reinterpret_cast<void* volatile*>(&sControlOwner));
    release();
    return true;
}
errno_t QcomBluetooth::controlConnect(kern_ctl_ref reference, sockaddr_ctl* address, void** context)
{
    // The signed system daemon carries this entitlement in the pinned sample.
    // CTL_FLAG_PRIVILEGED would incorrectly exclude its mobile launchd user.
    auto* entitlement = IOUserClient::copyClientEntitlement(nullptr, "com.apple.bluetooth.control");
    bool allowed = entitlement == kOSBooleanTrue;
    bool bootstrap = false;
    OSSafeReleaseNULL(entitlement);
    if (!allowed) {
        entitlement = IOUserClient::copyClientEntitlement(nullptr, BootstrapEntitlement);
        allowed = entitlement == kOSBooleanTrue;
        bootstrap = allowed;
        OSSafeReleaseNULL(entitlement);
    }
    if (!allowed)
        return EPERM;
    auto* owner = sControlOwner;
    if (!owner || !address || !context)
        return ENXIO;
    ControlRequest request { ControlRequest::Connect, address->sc_unit };
    request.bootstrap = bootstrap;
    const auto error = owner->controlRequest(request);
    if (!error)
        *context = owner;
    (void)reference;
    return error;
}
errno_t QcomBluetooth::controlDisconnect(kern_ctl_ref, uint32_t unit, void* context)
{
    auto* owner = static_cast<QcomBluetooth*>(context);
    if (!owner)
        return ENXIO;
    ControlRequest request { ControlRequest::Disconnect, unit };
    return owner->controlRequest(request);
}
errno_t QcomBluetooth::controlSend(kern_ctl_ref, uint32_t unit, void* context, mbuf_t packet, int)
{
    auto* owner = static_cast<QcomBluetooth*>(context);
    ControlRequest request { ControlRequest::Send, unit };
    request.packet = packet;
    const auto error = owner ? owner->controlRequest(request) : ENXIO;
    if (packet)
        mbuf_freem(packet);
    return error;
}
errno_t QcomBluetooth::controlGet(
    kern_ctl_ref, uint32_t unit, void* context, int option, void* data, size_t* length)
{
    auto* owner = static_cast<QcomBluetooth*>(context);
    ControlRequest request { ControlRequest::GetOption, unit };
    request.option = uint32_t(option);
    request.data = data;
    request.length = length;
    return owner ? owner->controlRequest(request) : ENXIO;
}
errno_t QcomBluetooth::controlSet(
    kern_ctl_ref, uint32_t unit, void* context, int option, void* data, size_t length)
{
    auto* owner = static_cast<QcomBluetooth*>(context);
    ControlRequest request { ControlRequest::SetOption, unit };
    request.option = uint32_t(option);
    request.data = data;
    request.length = &length;
    return owner ? owner->controlRequest(request) : ENXIO;
}
errno_t QcomBluetooth::controlRequest(ControlRequest& request)
{
    return fGate ? int(fGate->runAction(gatedControl, &request)) : ENXIO;
}
IOReturn QcomBluetooth::gatedControl(OSObject* owner, void* argument, void*, void*, void*)
{
    return static_cast<QcomBluetooth*>(owner)->performControl(*static_cast<ControlRequest*>(argument));
}
errno_t QcomBluetooth::performControl(ControlRequest& r)
{
    if (r.operation == ControlRequest::Disconnect) {
        const bool reset = fControlSession.requiresReset();
        if (!fControlSession.disconnect(r.unit))
            return ENOTCONN;
        // A physical frame in progress must not leak into a later HCI host
        // session. Restart through the real shutdown/firmware state machine.
        if (reset)
            fDesiredPower = false;
        if (reset || fTX.size() || fTXInFlight)
            fController.stop();
        fPortAcquired = fTXInFlight = false;
        fPortBits &= ~Active;
        fRX.clear();
        fTX.clear();
        fGate->commandWakeup(&fPortBits);
        if (fActive && (fDesiredPower || fController.resources()))
            fTimer->setTimeoutMS(1);
        return 0;
    }
    if (!fActive || isInactive() || fController.currentState() != State::Ready || !fDesiredPower)
        return ENXIO;
    if (r.operation == ControlRequest::Connect) {
        if (fPortAcquired || !fControlSession.connect(r.unit, fController.epoch(), r.bootstrap))
            return EBUSY;
        fPortAcquired = true;
        fPortBits |= Active | TXEnable | RXEnable;
        fRX.clear();
        fTX.clear();
        fControlOptions = { };
        fControlOptions.characters[VMIN] = 1;
        return 0;
    }
    if (!fControlSession.matches(r.unit, fController.epoch()) || !fPortAcquired)
        return ENOTCONN;
    if (r.operation == ControlRequest::Send) {
        if (fControlSession.sealed())
            return EPIPE;
        if (!r.packet)
            return EINVAL;
        const size_t bytes = mbuf_pkthdr_len(r.packet);
        if (!bytes)
            return 0;
        if (bytes > sizeof(fControlTX))
            return EMSGSIZE;
        if (bytes > fTX.available())
            return ENOBUFS;
        const auto error = mbuf_copydata(r.packet, 0, bytes, fControlTX);
        if (error)
            return error;
        fTX.push(fControlTX, bytes);
        fTimer->setTimeoutMS(1);
        return 0;
    }
    if (!r.length)
        return EINVAL;
    if (r.operation == ControlRequest::GetOption) {
        if (r.option != NativeGetTermios)
            return ENOPROTOOPT;
        const size_t capacity = *r.length;
        *r.length = sizeof(fControlOptions);
        if (!r.data)
            return 0;
        if (capacity < sizeof(fControlOptions))
            return EMSGSIZE;
        memcpy(r.data, &fControlOptions, sizeof(fControlOptions));
        return 0;
    }
    if (r.operation == ControlRequest::SetOption) {
        if (r.option == BootstrapHandoff) {
            if (!r.data || *r.length != sizeof(uint64_t))
                return EINVAL;
            uint64_t epoch = 0;
            memcpy(&epoch, r.data, sizeof(epoch));
            if (fTX.size() || fTXInFlight || fPlatform.drained() != Result::Success)
                return EBUSY;
            return fControlSession.handoff(r.unit,epoch) ? 0 : EPERM;
        }
        if (fControlSession.sealed())
            return EPIPE;
        if (r.option != NativeSetTermios)
            return ENOPROTOOPT;
        if (!r.data || *r.length != sizeof(fControlOptions))
            return EINVAL;
        NativeTermios options;
        memcpy(&options, r.data, sizeof(options));
        if (!validNativeTermios(options))
            return EINVAL;
        fControlOptions = options;
        return 0;
    }
    return EINVAL;
}
void QcomBluetooth::flushControlRX()
{
    const auto unit = fControlSession.unit();
    if (!unit || !fControl || !fControlSession.matches(unit, fController.epoch()))
        return;
    // XNU releases the control socket lock around every send/get/set/connect
    // callback. Enqueue from this workloop therefore cannot invert that lock
    // against our command gate. ENOBUFS retains the complete pending frame.
    for (unsigned budget = 0; budget < 8 && fRX.size(); ++budget) {
        const auto header = headerBytes(fRX.peek(0));
        if (!header || fRX.size() < header)
            return;
        const auto payload = fRX.peek(0) == ACL ? unsigned(fRX.peek(3)) | (unsigned(fRX.peek(4)) << 8)
                                                : unsigned(fRX.peek(header - 1));
        const size_t bytes = header + payload;
        if (bytes > sizeof(fTXFrame) || fRX.size() < bytes)
            return;
        for (size_t i = 0; i < bytes; ++i)
            fTXFrame[i] = fRX.peek(i);
        const auto error = ctl_enqueuedata(fControl, unit, fTXFrame, bytes, 0);
        if (error == ENOBUFS)
            return;
        if (error) {
            fLastFault = Result::Fault;
            fController.transportFault(Result::Fault);
            return;
        }
        fRX.pop(nullptr, bytes);
    }
}
