// SPDX-License-Identifier: BSD-2-Clause
#include "QcomWLANUserClient.h"
#include "QcomWLAN.h"
#include <WLANProtocol.h>
#define super IOUserClient
OSDefineMetaClassAndStructors(QcomWLANUserClient, super);

bool QcomWLANUserClient::initWithTask(task_t task, void *token, UInt32 type, OSDictionary *properties) {
    if (type != WLAN::DiagnosticClient || clientHasPrivilege(task,kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return false;
    return super::initWithTask(task,token,type,properties);
}
bool QcomWLANUserClient::start(IOService *provider) {
    auto *controller = OSDynamicCast(QcomWLAN,provider);
    if (!controller || !super::start(provider)) return false;
    // A retained owner survives concurrent service termination; request()
    // serializes admission against stop() and rejects subsequent operations.
    fController = controller; fController->retain(); return true;
}
void QcomWLANUserClient::free() { OSSafeReleaseNULL(fController); super::free(); }
IOReturn QcomWLANUserClient::clientClose() { terminate(); return kIOReturnSuccess; }
IOReturn QcomWLANUserClient::externalMethod(UInt32 selector, IOExternalMethodArguments *a,
    IOExternalMethodDispatch *, OSObject *, void *) {
    if (!a || !fController || isInactive()) return kIOReturnNotAttached;
    if (selector >= WLAN::SelectorCount) return kIOReturnUnsupported;
    if (a->structureInputSize || a->structureOutputSize || a->structureInputDescriptor ||
        a->structureOutputDescriptor || a->asyncReferenceCount ||
        (a->scalarInputCount && !a->scalarInput) || (a->scalarOutputCount && !a->scalarOutput) ||
        !WLAN::validCall(selector,a->scalarInputCount,a->scalarOutputCount,
            a->scalarInputCount == 1 ? a->scalarInput[0] : 0)) return kIOReturnBadArgument;
    return fController->request(selector,a->scalarOutput,a->scalarInputCount ? a->scalarInput[0] : 0);
}
