// SPDX-License-Identifier: BSD-2-Clause
#include "QcomBluetoothUserClient.h"
#include "QcomBluetooth.h"
#include <libkern/c++/OSBoolean.h>
#define super IOUserClient
OSDefineMetaClassAndStructors(QcomBluetoothUserClient, super);

bool QcomBluetoothUserClient::initWithTask(task_t task, void *token, UInt32 type, OSDictionary *properties) {
    bool allowed = false;
    if (type == Bluetooth::DiagnosticClient)
        allowed = clientHasPrivilege(task,kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
    else if (type == Bluetooth::BootstrapClient) {
        auto* entitlement = copyClientEntitlement(task, Bluetooth::BootstrapEntitlement);
        allowed = entitlement == kOSBooleanTrue;
        OSSafeReleaseNULL(entitlement);
    }
    fBootstrap = type == Bluetooth::BootstrapClient;
    return allowed && super::initWithTask(task,token,type,properties);
}
bool QcomBluetoothUserClient::start(IOService *provider) {
    auto *controller = OSDynamicCast(QcomBluetooth,provider);
    if (!controller || !super::start(provider)) return false;
    fController = controller; fController->retain(); return true;
}
void QcomBluetoothUserClient::free() { OSSafeReleaseNULL(fController); super::free(); }
IOReturn QcomBluetoothUserClient::clientClose() { terminate(); return kIOReturnSuccess; }
IOReturn QcomBluetoothUserClient::externalMethod(UInt32 selector, IOExternalMethodArguments *a,
    IOExternalMethodDispatch *, OSObject *, void *) {
    if (!a || !fController || isInactive()) return kIOReturnNotAttached;
    if (selector >= Bluetooth::SelectorCount) return kIOReturnUnsupported;
    // MIG may provide non-null inband buffers at zero length even for scalar
    // calls. Validate their lengths, not those unused buffer addresses.
    if (a->version != kIOExternalMethodArgumentsCurrentVersion || a->selector != selector ||
        a->structureInputSize || a->structureOutputSize || a->structureOutputDescriptorSize ||
        a->structureInputDescriptor || a->structureOutputDescriptor || a->structureVariableOutputData ||
        a->asyncWakePort != MACH_PORT_NULL || a->asyncReferenceCount ||
        (a->scalarInputCount && !a->scalarInput) || (a->scalarOutputCount && !a->scalarOutput) ||
        !Bluetooth::validCall(selector,a->scalarInputCount,a->scalarOutputCount,
            a->scalarInputCount ? a->scalarInput[0] : 0)) return kIOReturnBadArgument;
    if (selector == Bluetooth::BootstrapSetPower)
        return fBootstrap ? fController->bootstrapPower(a->scalarInput[0],a->scalarInput[1],a->scalarOutput)
                          : kIOReturnNotPrivileged;
    if (fBootstrap && selector == Bluetooth::SetPower) return kIOReturnNotPrivileged;
    return fController->request(selector,a->scalarOutput,a->scalarInputCount ? a->scalarInput[0] : 0);
}
