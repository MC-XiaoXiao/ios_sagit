// SPDX-License-Identifier: BSD-2-Clause
#include "QcomUSBHostPort.h"
#include "QcomUSBHost.h"
#include "USBHostCurrent.h"

OSDefineMetaClassAndStructors(QcomUSBHostPort,AppleUSBHostPort);

QcomUSBHostPort *QcomUSBHostPort::withController(QcomUSBHost *controller,UInt32 number,bool super) {
    if (!controller || !number || number>15) return nullptr;
    auto *port=new QcomUSBHostPort;
    if (!port) return nullptr;
    if (!port->init()) { port->release();return nullptr; }
    // Native start dereferences OSData("port") and derives locationID from
    // its provider. Supplying an OSNumber or skipping this property is invalid.
    if (!port->setProperty("port",&number,sizeof(number))) { port->release();return nullptr; }
    port->fController=controller;controller->retain();port->fNumber=number;port->fSuper=super;
    port->setName("usb-port");
    return port;
}
bool QcomUSBHostPort::start(IOService *provider) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
    static_assert(__builtin_offsetof(QcomUSBHostPort,fHostController)==768,"native retained controller offset");
#pragma clang diagnostic pop
    if (provider!=fController || fStarted || fOpened || fHostController ||
        __atomic_load_n(&fPublication,__ATOMIC_ACQUIRE)==2) return false;
    if (!fController->open(this)) return false;
    fHostController=fController;fHostController->retain();
    fOpened=true;fStarted=true;
    if (!AppleUSBHostPort::start(provider)) {
        // Native start invokes virtual stop on its own failed-start path.
        if (fStarted) stop(provider);
        return false;
    }
    return true;
}
void QcomUSBHostPort::stop(IOService *provider) {
    __atomic_store_n(&fPublication,UInt8(2),__ATOMIC_RELEASE);
    if (fStarted) { fStarted=false;AppleUSBHostPort::stop(provider); }
    if (fOpened) { fController->close(this);fOpened=false; }
}
IOReturn QcomUSBHostPort::message(UInt32 type,IOService *provider,void *argument) {
    if (type==UInt32(0xe0005104)) {
        const auto state=__atomic_load_n(&fPublication,__ATOMIC_ACQUIRE);
        if (provider!=fController || state==2) return kIOReturnNotReady;
        if (state==1) return kIOReturnSuccess;
    }
    const auto result=AppleUSBHostPort::message(type,provider,argument);
    // The Native controller sends this only after its PM action created the
    // complete port table. Its Native port handler must run first; IRQs seen
    // during partial construction are replaced by this fresh status sample.
    if (type==UInt32(0xe0005104) && result==kIOReturnSuccess) {
        UInt8 expected=0;
        if (__atomic_compare_exchange_n(&fPublication,&expected,UInt8(1),false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
            signalHardwareChange();
    }
    return result;
}
void QcomUSBHostPort::signalHardwareChange() {
    if (__atomic_load_n(&fPublication,__ATOMIC_ACQUIRE)==1) signalInterrupt(0);
}
void QcomUSBHostPort::free() {
    if (fOpened && fController) fController->close(this);
    fOpened=false;
    OSSafeReleaseNULL(fController);
    AppleUSBHostPort::free();
}
UInt32 QcomUSBHostPort::getPortType() {
    // Current Native EHCI/OHCI return 0 for an external connector, 2 for
    // internal and 4 for captive. These bits are independent of link speed.
    return 0;
}
IOReturn QcomUSBHostPort::initBusCurrent() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
    static_assert(__builtin_offsetof(QcomUSBHostPort,fMinimumBusCurrent)==624,"Native enumeration current offset");
#pragma clang diagnostic pop
    // Native EHCI/OHCI seed this field before delegating to the Family.
    // Its allocator replaces the port's enumeration reserve with a device's
    // configured request, and the controller pool accounts across ports.
    fMinimumBusCurrent=USB::HostCurrent::enumeration(fSuper);
    return AppleUSBHostPort::initBusCurrent();
}
IOReturn QcomUSBHostPort::getCompanionPortGated(UInt32 speed,AppleUSBHostPort *&port) {
    port=nullptr;
    if (!fStarted || __atomic_load_n(&fPublication,__ATOMIC_ACQUIRE)==2) return kIOReturnNotReady;
    // Native's base getter retains/caches the borrowed controller result.
    // Keep that whole handoff under the controller gate, so teardown cannot
    // drop the table's last peer reference between lookup and Native retain.
    return fController->resolveCompanion(this,speed,port);
}
IOReturn QcomUSBHostPort::powerOff() {
    return fController->portOperation(fNumber,QcomUSBHost::PortOperation::PowerOff);
}
IOReturn QcomUSBHostPort::powerOn() {
    return fController->portOperation(fNumber,QcomUSBHost::PortOperation::PowerOn);
}
IOReturn QcomUSBHostPort::suspend() {
    return fController->portOperation(fNumber,QcomUSBHost::PortOperation::Suspend);
}
IOReturn QcomUSBHostPort::resume() {
    return fController->portOperation(fNumber,QcomUSBHost::PortOperation::Resume);
}
IOUSBHostDevice *QcomUSBHostPort::resetAndCreateDevice(UInt32) {
    // xHCI controls the electrical reset duration. Complete the hardware
    // reset plus recovery before asking the Native port to attach/start the
    // real Family device through controller slot192.
    if (fController->portOperation(fNumber,QcomUSBHost::PortOperation::Reset)!=kIOReturnSuccess)
        return nullptr;
    return AppleUSBHostPort::createDevice(0);
}
IOReturn QcomUSBHostPort::getPortStatusGated(UInt32 &status) {
    return fController->portStatus(fNumber,status);
}
