// SPDX-License-Identifier: BSD-2-Clause
#include "QcomGPUUserClient.h"
#include "QcomGPU.h"
#define super IOUserClient
OSDefineMetaClassAndStructors(QcomGPUUserClient, super);

bool QcomGPUUserClient::initWithTask(task_t task, void *token, UInt32 type, OSDictionary *properties)
{
    return task && type == GPU::ConnectionType &&
        clientHasPrivilege(task, kIOClientPrivilegeAdministrator) == kIOReturnSuccess &&
        super::initWithTask(task, token, type, properties);
}
bool QcomGPUUserClient::start(IOService *provider)
{
    auto *gpu = OSDynamicCast(QcomGPU, provider);
    if (!gpu || !super::start(provider)) return false;
    fGPU = gpu; fGPU->retain();
    if (fGPU->request(this, GPU::SelectorCount) != kIOReturnSuccess) {
        super::stop(provider); return false;
    }
    return true;
}
void QcomGPUUserClient::releaseResources()
{
    for (auto &memory : fMemory) OSSafeReleaseNULL(memory);
    fSession.clear();
}
IOReturn QcomGPUUserClient::clientClose()
{
    if (fGPU) fGPU->request(this, GPU::SelectorCount + 1);
    terminate(); return kIOReturnSuccess;
}
void QcomGPUUserClient::stop(IOService *provider)
{
    if (fGPU) fGPU->request(this, GPU::SelectorCount + 1);
    super::stop(provider);
}
void QcomGPUUserClient::free()
{
    // Registered clients are closed by stop/clientClose under the gate.
    if (fGPU) fGPU->request(this, GPU::SelectorCount + 1);
    releaseResources();
    OSSafeReleaseNULL(fGPU);
    super::free();
}
IOReturn QcomGPUUserClient::externalMethod(UInt32 selector, IOExternalMethodArguments *a,
    IOExternalMethodDispatch *, OSObject *, void *)
{
    if (!a || !fGPU || isInactive()) return kIOReturnNotAttached;
    if (selector >= GPU::SelectorCount) return kIOReturnUnsupported;
    // No out-of-line structures, async references, CPU/MMIO mappings or raw
    // addresses. The bounded inline data is copied by Apple's IOKit MIG layer.
    if (a->structureInputDescriptor || a->structureOutputDescriptor || a->asyncReferenceCount ||
        !GPU::validCall(selector, a->scalarInputCount, a->scalarOutputCount,
            a->structureInputSize, a->structureOutputSize) ||
        (a->scalarInputCount && !a->scalarInput) || (a->scalarOutputCount && !a->scalarOutput) ||
        (a->structureInputSize && !a->structureInput) || (a->structureOutputSize && !a->structureOutput))
        return kIOReturnBadArgument;
    return fGPU->request(this, selector, a);
}
