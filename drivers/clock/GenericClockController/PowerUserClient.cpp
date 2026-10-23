// SPDX-License-Identifier: BSD-2-Clause
#include "PowerUserClient.h"
#include "GenericClockController.h"
#include <PowerUserProtocol.h>
#define super IOUserClient
OSDefineMetaClassAndStructors(PowerUserClient, super);
bool PowerUserClient::initWithTask(task_t task, void *token, UInt32 type, OSDictionary *properties)
{
    if (!super::initWithTask(task, token, type, properties)) return false;
    fAdmin = clientHasPrivilege(task, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
    return true;
}
bool PowerUserClient::start(IOService *provider)
{
    fController = OSDynamicCast(GenericClockController, provider);
    return fController && super::start(provider);
}
IOReturn PowerUserClient::clientClose()
{
    terminate(); return kIOReturnSuccess;
}
IOReturn PowerUserClient::externalMethod(UInt32 selector, IOExternalMethodArguments *a,
    IOExternalMethodDispatch *, OSObject *, void *)
{
    if (!a || !fController || isInactive()) return kIOReturnNotAttached;
    if (a->structureInputSize || a->structureOutputSize || a->structureInputDescriptor ||
        a->structureOutputDescriptor || a->asyncReferenceCount) return kIOReturnBadArgument;
    using namespace ClockController;
    auto check = PowerUser::validate(selector, a->scalarInputCount, a->scalarOutputCount,
        a->scalarInputCount ? a->scalarInput[0] : 0,
        a->scalarInputCount > 1 ? a->scalarInput[1] : 0, fAdmin, fController->controllerKind());
    if (check != PowerUser::Valid) return check == PowerUser::NotPrivileged ? kIOReturnNotPrivileged :
        check == PowerUser::UnsupportedSelector ? kIOReturnUnsupported : kIOReturnBadArgument;
    if (selector == PowerUser::GetInfo) {
        if (a->scalarInputCount || a->scalarOutputCount != 3) return kIOReturnBadArgument;
        auto r = targeted(fController->controllerKind(), GetCapabilities, 0);
        IOReturn result = fController->submit(r.request);
        if (result != kIOReturnSuccess) return result;
        a->scalarOutput[0] = 2; a->scalarOutput[1] = r.controller; a->scalarOutput[2] = r.request.value;
        return kIOReturnSuccess;
    }
    if (a->scalarInputCount != 2 || a->scalarOutputCount != 1) return kIOReturnBadArgument;
    TargetedRequest r = targeted(fController->controllerKind(), GetRate, 0);
    if (selector == PowerUser::ReadResource) {
        if (a->scalarInput[0] > UINT32_MAX || !PowerUser::readable(static_cast<UInt32>(a->scalarInput[0])) ||
            a->scalarInput[1] > UINT32_MAX) return kIOReturnBadArgument;
        r.request.operation = static_cast<Operation>(a->scalarInput[0]);
        r.request.id = static_cast<UInt32>(a->scalarInput[1]);
    } else if (selector == PowerUser::SetLimit || selector == PowerUser::SetCPUFrequency || selector == PowerUser::SetAutomatic) {
        if (!fAdmin) return kIOReturnNotPrivileged;
        if (a->scalarInput[0] > UINT32_MAX) return kIOReturnBadArgument;
        r.request.operation = selector == PowerUser::SetLimit ? SetPerformanceLimit :
            selector == PowerUser::SetAutomatic ? SetAutomaticPerformance : SetRate;
        r.request.id = static_cast<UInt32>(a->scalarInput[0]); r.request.value = a->scalarInput[1];
    } else return kIOReturnUnsupported;
    IOReturn result = fController->submit(r.request);
    if (result == kIOReturnSuccess && (selector == PowerUser::SetCPUFrequency || selector == PowerUser::SetAutomatic)) {
        r.request.operation = selector == PowerUser::SetCPUFrequency ? GetRate : GetAutomaticPerformance; result = fController->submit(r.request);
    }
    if (result == kIOReturnSuccess) a->scalarOutput[0] = r.request.value;
    return result;
}
