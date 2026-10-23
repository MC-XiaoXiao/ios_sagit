// SPDX-License-Identifier: BSD-2-Clause
#include "QcomBluetoothSerial.h"
#include "QcomBluetooth.h"
#include <IOKit/IOLib.h>
#define super IOSerialDriverSync
OSDefineMetaClassAndStructors(QcomBluetoothSerial, super);
bool QcomBluetoothSerial::start(IOService* provider)
{
    auto* owner = OSDynamicCast(QcomBluetooth, provider);
    if (!owner || !super::start(provider))
        return false;
    fOwner = owner;
    fOwner->retain();
    auto fail = [&] {
        OSSafeReleaseNULL(fStream);
        OSSafeReleaseNULL(fOwner);
        super::stop(provider);
        return false;
    };
    auto* object = OSMetaClass::allocClassWithName("IOSerialStreamSync");
    fStream = OSDynamicCast(IOSerialStreamSync, object);
    if (object && !fStream)
        object->release();
    if (!fStream || !fStream->init(nullptr, this))
        return fail();
    fStream->setProperty("IOTTYBaseName", "bluetooth");
    fStream->setProperty("IOTTYSuffix", "");
    fStream->setProperty("IOSerialBSDClientType", "IORS232SerialStream");
    fStream->setProperty("BluetoothPhysicalBaud", Bluetooth::OperationalBaud, 32);
    if (!fStream->attach(this))
        return fail();
    if (!fStream->start(this)) {
        fStream->detach(this);
        return fail();
    }
    // IOSerialBSDClient owns BSD device creation. There is no custom devfs
    // implementation or replacement bluetoothd in this driver.
    fStream->registerService();
    registerService();
    return true;
}
void QcomBluetoothSerial::stop(IOService* provider)
{
    if (fStream)
        fStream->terminate(kIOServiceRequired);
    OSSafeReleaseNULL(fOwner);
    super::stop(provider);
}
void QcomBluetoothSerial::free()
{
    OSSafeReleaseNULL(fStream);
    OSSafeReleaseNULL(fOwner);
    super::free();
}
IOReturn QcomBluetoothSerial::call(Bluetooth::SerialRequest& r)
{
    return fOwner ? fOwner->serial(r) : kIOReturnNotAttached;
}
IOReturn QcomBluetoothSerial::acquirePort(bool sleep, void*)
{
    Bluetooth::SerialRequest r { Bluetooth::Acquire };
    r.sleep = sleep;
    return call(r);
}
IOReturn QcomBluetoothSerial::releasePort(void*)
{
    Bluetooth::SerialRequest r { Bluetooth::Release };
    return call(r);
}
IOReturn QcomBluetoothSerial::setState(UInt32 value, UInt32 mask, void*)
{
    Bluetooth::SerialRequest r { Bluetooth::SetState };
    r.value = value;
    r.mask = mask;
    return call(r);
}
UInt32 QcomBluetoothSerial::getState(void*)
{
    UInt32 value = 0;
    Bluetooth::SerialRequest r { Bluetooth::GetState };
    r.output = &value;
    call(r);
    return value;
}
IOReturn QcomBluetoothSerial::watchState(UInt32* value, UInt32 mask, void*)
{
    Bluetooth::SerialRequest r { Bluetooth::WatchState };
    r.output = value;
    r.mask = mask;
    return call(r);
}
UInt32 QcomBluetoothSerial::nextEvent(void*)
{
    UInt32 value = 0;
    Bluetooth::SerialRequest r { Bluetooth::NextEvent };
    r.output = &value;
    call(r);
    return value;
}
IOReturn QcomBluetoothSerial::executeEvent(UInt32 event, UInt32 data, void*)
{
    Bluetooth::SerialRequest r { Bluetooth::ExecuteEvent };
    r.value = event;
    r.mask = data;
    return call(r);
}
IOReturn QcomBluetoothSerial::requestEvent(UInt32 event, UInt32* data, void*)
{
    Bluetooth::SerialRequest r { Bluetooth::RequestEvent };
    r.value = event;
    r.output = data;
    return call(r);
}
IOReturn QcomBluetoothSerial::enqueueEvent(UInt32 event, UInt32 data, bool sleep, void*)
{
    Bluetooth::SerialRequest r { Bluetooth::EnqueueEvent };
    r.value = event;
    r.mask = data;
    r.sleep = sleep;
    return call(r);
}
IOReturn QcomBluetoothSerial::dequeueEvent(UInt32* event, UInt32* data, bool sleep, void*)
{
    Bluetooth::SerialRequest r { Bluetooth::DequeueEvent };
    r.output = event;
    r.count = data;
    r.sleep = sleep;
    return call(r);
}
IOReturn QcomBluetoothSerial::enqueueData(UInt8* p, UInt32 size, UInt32* count, bool sleep, void*)
{
    Bluetooth::SerialRequest r { Bluetooth::EnqueueData };
    r.buffer = p;
    r.size = size;
    r.count = count;
    r.sleep = sleep;
    return call(r);
}
IOReturn QcomBluetoothSerial::dequeueData(UInt8* p, UInt32 size, UInt32* count, UInt32 minimum, void*)
{
    Bluetooth::SerialRequest r { Bluetooth::DequeueData };
    r.buffer = p;
    r.size = size;
    r.count = count;
    r.minimum = minimum;
    return call(r);
}
