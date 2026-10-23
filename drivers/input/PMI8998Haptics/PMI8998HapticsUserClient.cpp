// SPDX-License-Identifier: GPL-2.0-only
#include "PMI8998HapticsUserClient.h"
#include "PMI8998Haptics.h"
OSDefineMetaClassAndStructors(PMI8998HapticsUserClient, IOUserClient);
bool PMI8998HapticsUserClient::initWithTask(task_t task, void *token, UInt32 type, OSDictionary *properties) {
    return task && type == Haptics::DiagnosticClient &&
        clientHasPrivilege(task, kIOClientPrivilegeAdministrator) == kIOReturnSuccess &&
        IOUserClient::initWithTask(task, token, type, properties);
}
bool PMI8998HapticsUserClient::start(IOService *provider) {
    auto *haptics = OSDynamicCast(PMI8998Haptics, provider);
    if (!haptics || !IOUserClient::start(provider)) return false;
    controller = haptics; controller->retain();
    return true;
}
IOReturn PMI8998HapticsUserClient::clientClose() {
    if (controller) controller->closeClient(this);
    terminate(); return kIOReturnSuccess;
}
void PMI8998HapticsUserClient::stop(IOService *provider) {
    if (controller) controller->closeClient(this);
    IOUserClient::stop(provider);
}
void PMI8998HapticsUserClient::free() {
    if (controller) controller->closeClient(this);
    OSSafeReleaseNULL(controller); IOUserClient::free();
}
IOReturn PMI8998HapticsUserClient::externalMethod(UInt32 selector, IOExternalMethodArguments *a,
    IOExternalMethodDispatch *, OSObject *, void *) {
    if (!a || !controller || isInactive()) return kIOReturnNotAttached;
    if (selector >= Haptics::SelectorCount) return kIOReturnUnsupported;
    if (a->version != kIOExternalMethodArgumentsCurrentVersion || a->selector != selector ||
        a->structureInputSize || a->structureOutputSize || a->structureOutputDescriptorSize ||
        a->structureInputDescriptor || a->structureOutputDescriptor || a->structureVariableOutputData ||
        a->asyncWakePort != MACH_PORT_NULL || a->asyncReferenceCount ||
        (a->scalarInputCount && !a->scalarInput) || (a->scalarOutputCount && !a->scalarOutput) ||
        !Haptics::validCall(selector, a->scalarInputCount, a->scalarOutputCount, a->scalarInput))
        return kIOReturnBadArgument;
    return controller->request(selector, this, a->scalarInput, a->scalarOutput);
}
