// SPDX-License-Identifier: BSD-2-Clause
#include "QcomGPU.h"
#include <IOKit/IOMessage.h>
#include "QcomGPUUserClient.h"
#include "QcomGPUNative.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSString.h>
#include <machine/machine_routines.h>
#include <mach/mach_time.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <sys/codesign.h>
#include <sys/proc.h>
#include "NativePosterIdentity.h"
#define super IOGPU
OSDefineMetaClassAndStructors(QcomGPU, super);

namespace {
enum : uint32_t { Open = GPU::SelectorCount, Close, PowerOff, PowerOn, Shutdown };
IOReturn result(GPU::Result r) {
    switch (r) {
        case GPU::Result::Success: return kIOReturnSuccess;
        case GPU::Result::Invalid: return kIOReturnBadArgument;
        case GPU::Result::NoResources: return kIOReturnNoResources;
        case GPU::Result::Busy: return kIOReturnBusy;
        case GPU::Result::Unsupported: return kIOReturnUnsupported;
        case GPU::Result::Offline: return kIOReturnOffline;
        case GPU::Result::Timeout: return kIOReturnTimeout;
        case GPU::Result::Fault: return kIOReturnIOError;
    }
    return kIOReturnError;
}
bool compatible(IOService *provider) {
    auto *data = OSDynamicCast(OSData, provider->getProperty("compatible"));
    if (!data) return false;
    const char *bytes = static_cast<const char *>(data->getBytesNoCopy());
    unsigned length = data->getLength();
    constexpr char name[] = "qcom,adreno-540.1";
    for (unsigned pos = 0; pos < length;) {
        unsigned end = pos;
        while (end < length && bytes[end]) ++end;
        if (end == length) return false;
        if (end - pos == sizeof(name) - 1 && !memcmp(bytes + pos, name, sizeof(name) - 1)) return true;
        pos = end + 1;
    }
    return false;
}
bool compositorTask(task_t task) {
    // Actual 22H374 backboardd role entitlement; a task name, UID or caller
    // supplied property cannot grant this access. Original IOGPUFamily still
    // performs its task/device creation and IOSurface ownership checks.
    auto entitlement=IOUserClient::copyClientEntitlement(task,"com.apple.backboardd");
    const bool allowed=entitlement==kOSBooleanTrue;
    if(entitlement) entitlement->release();
    return allowed;
}
bool nativeDisplayClientTask(task_t task) {
    // System UI clients already carry Apple's native IOGPU class allowance.
    // Require both restricted QuartzCore roles as well: the class list alone
    // does not authorize arbitrary shader code. Never infer a role from a PID,
    // executable name, UID or caller-supplied connection properties.
    auto display=IOUserClient::copyClientEntitlement(task,"com.apple.QuartzCore.displayable-context");
    auto secure=IOUserClient::copyClientEntitlement(task,"com.apple.QuartzCore.secure-mode");
    auto clients=IOUserClient::copyClientEntitlement(task,"com.apple.security.iokit-user-client-class");
    auto *classes=OSDynamicCast(OSArray,clients);
    bool native=false;
    if(display==kOSBooleanTrue && secure==kOSBooleanTrue && classes) {
        for(unsigned i=0;i<classes->getCount();++i) {
            auto *name=OSDynamicCast(OSString,classes->getObject(i));
            if(name && name->isEqualTo("IOGPUDeviceUserClient")){native=true;break;}
        }
    }
    OSSafeReleaseNULL(display);OSSafeReleaseNULL(secure);OSSafeReleaseNULL(clients);
    return native;
}
bool nativePosterTask(task_t task) {
    auto provider=IOUserClient::copyClientEntitlement(task,"com.apple.posterkit.provider");
    auto secure=IOUserClient::copyClientEntitlement(task,"com.apple.QuartzCore.secure-mode");
    auto process=static_cast<proc_t>(get_bsdtask_info(task));
    const bool allowed=GPU::NativePoster::allowed(provider==kOSBooleanTrue,secure==kOSBooleanTrue,
        process ? cs_get_cdhash(process) : nullptr);
    OSSafeReleaseNULL(provider);OSSafeReleaseNULL(secure);
    return allowed;
}
}
IOService *QcomGPU::probe(IOService *provider, SInt32 *score)
{
    if (!super::probe(provider, score) || !compatible(provider)) return nullptr;
    return this;
}
bool QcomGPU::start(IOService *provider)
{
    if (!compatible(provider)) return false;
    fNativeStarting = true;
    bool nativeStarted = super::start(provider);
    fNativeStarting = false;
    if (!nativeStarted) return false; // IOGPU unwinds its own partial start.
    fStarted = true;
    if(!OSDynamicCast(QcomGPUScheduler,nativeSchedulerObject())) {stop(provider);return false;}
    setProperty("GPUWorkScheduling","IOGPUScheduler/IOGPUWorkQueue");
    kprintf("QcomGPU: native IOGPUScheduler + IOGPUWorkQueue enabled\n");
    // AppleARMIODevice translates reg into standard memory descriptors. MMIO
    // remains untouched unless a complete retained-firmware contract is present.
    auto *memory = provider->getDeviceMemoryWithIndex(0);
    if (!memory || memory->getPhysicalAddress() != Adreno::RegisterBase ||
        memory->getLength() != Adreno::RegisterBytes) { stop(provider); return false; }
    fLoop = super::getWorkLoop();
    if (fLoop) fLoop->retain();
    if (fLoop) fGate = IOCommandGate::commandGate(this, action);
    if (!fGate || fLoop->addEventSource(fGate) != kIOReturnSuccess) { stop(provider); return false; }
    fAdded = true; fState = GPU::FrameworkReady;
    if(provider->getProperty("qcom,gpu-handoff")) {
        fTimer=IOTimerEventSource::timerEventSource(this,timerAction);
        if(!fTimer || fLoop->addEventSource(fTimer)!=kIOReturnSuccess){stop(provider);return false;}
        fTimerAdded=true;fBootstrapPending=true;
        uint64_t interval=0;nanoseconds_to_absolutetime(10000000000ULL,&interval);
        fBootstrapDeadline=mach_absolute_time()+interval;
        const auto status=fGate->runAction(bootstrapAction);
        if(status!=kIOReturnSuccess && !fBootstrapPending){stop(provider);return false;}
    }
    if(!publishHardwareState()){stop(provider);return false;}
    IOPMPowerState states[2] = {};
    states[0].version = states[1].version = 1;
    // These are software admission states. The platform leases survive both
    // states; system suspend/power collapse still needs a separate protocol.
    states[0].inputPowerRequirement = states[1].inputPowerRequirement = kIOPMPowerOn;
    states[0].outputPowerCharacter = states[1].outputPowerCharacter = kIOPMPowerOn;
    states[1].capabilityFlags = kIOPMDeviceUsable;
    PMinit(); fPM = true;
    provider->joinPMtree(this);
    if (registerPowerDriver(this, states, 2) != kIOReturnSuccess) { stop(provider); return false; }
    changePowerStateTo(1);
    if (fInterrupt) fInterrupt->enable();
    if (fTimer) fTimer->setTimeoutMS(100);
    registerService(); return true;
}
IOWorkLoop *QcomGPU::getWorkLoop() const { return fLoop ? fLoop : super::getWorkLoop(); }
bool QcomGPU::requestTerminate(IOService *, IOOptionBits) { return false; }
IOReturn QcomGPU::request(QcomGPUUserClient *client, uint32_t selector, IOExternalMethodArguments *a)
{
    if (ml_at_interrupt_context()) return kIOReturnNotPermitted;
    if (!fAdded) return kIOReturnNotReady;
    return fGate->runAction(action, client, reinterpret_cast<void *>(uintptr_t(selector)), a);
}
IOReturn QcomGPU::action(OSObject *owner, void *client, void *selector, void *arguments, void *)
{
    return static_cast<QcomGPU *>(owner)->perform(static_cast<QcomGPUUserClient *>(client),
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(selector)), static_cast<IOExternalMethodArguments *>(arguments));
}
IOReturn QcomGPU::perform(QcomGPUUserClient *c, uint32_t selector, IOExternalMethodArguments *a)
{
    if (selector == PowerOff || selector == PowerOn || selector == Shutdown) {
        if(selector==PowerOff || selector==PowerOn)fAdmissionOn=selector==PowerOn;
        if(selector==PowerOn && fBootstrapPending) {
            uint64_t interval=0;nanoseconds_to_absolutetime(10000000000ULL,&interval);
            fBootstrapDeadline=mach_absolute_time()+interval;
        }
        if (selector == Shutdown) {
            fAdmissionOn=false;fBootstrapPending=false;fRecoveryPending=false;
            const bool stopped=stopHardware();
            setProperty("GPUShutdownDMAStopped",stopped);
            kprintf("QcomGPU: shutdown CP/VBIF/TLB drain stopped=%u quarantined=%u\n",stopped,fQuarantined);
        }
        if (fState != GPU::Stopped && fState != GPU::Faulted) fState = selector == Shutdown ? GPU::Stopped :
            (selector == PowerOn && !fBootstrapPending ? GPU::FrameworkReady : GPU::Suspended);
        if (selector == Shutdown) for (auto &client : fClients) if (client) {
            client->releaseResources(); client->fClosed = true; client->fRegistered = false; client = nullptr;
        }
        return kIOReturnSuccess;
    }
    if (!c) return kIOReturnBadArgument;
    if (selector == Close) {
        if (c->fRegistered) for (auto &client : fClients) if (client == c) client = nullptr;
        c->fRegistered = false; c->fClosed = true; c->releaseResources(); return kIOReturnSuccess;
    }
    if (selector == Open) {
        if (c->fClosed || c->fRegistered || fState != GPU::FrameworkReady) return kIOReturnNotReady;
        for (auto &client : fClients) if (!client) { client = c; c->fRegistered = true; return kIOReturnSuccess; }
        return kIOReturnNoResources;
    }
    if (!c->fRegistered || c->fClosed || !a) return kIOReturnNotAttached;
    if (selector == GPU::GetInfo) {
        const uint64_t info[] = {GPU::ProtocolVersion, GPU::ChipID, GPU::GMEMBytes,
            features(), fState, blockers()};
        memcpy(a->scalarOutput, info, sizeof(info)); return kIOReturnSuccess;
    }
    if (selector == GPU::GetLimits) {
        const uint64_t limits[] = {GPU::MaxContexts, GPU::MaxQueues, GPU::MaxBuffers,
            GPU::MaxBufferBytes, GPU::MaxSessionBytes, GPU::MaxCopyBytes};
        memcpy(a->scalarOutput, limits, sizeof(limits)); return kIOReturnSuccess;
    }
    if (selector == GPU::GetFence) {
        bool done = false;
        auto status = c->fSession.queryFence(a->scalarInput[0], a->scalarInput[1], done);
        if (status == GPU::Result::Success) a->scalarOutput[0] = done;
        return result(status);
    }
    // Cleanup remains possible while admission is suspended.
    if (fState != GPU::FrameworkReady && selector != GPU::DestroyContext &&
        selector != GPU::DestroyQueue && selector != GPU::DestroyBuffer) return kIOReturnOffline;
    auto &s = c->fSession;
    const uint64_t *in = a->scalarInput;
    switch (selector) {
        case GPU::CreateContext: return result(s.createContext(a->scalarOutput[0]));
        case GPU::DestroyContext: return result(s.destroyContext(in[0]));
        case GPU::CreateQueue: return result(s.createQueue(in[0], a->scalarOutput[0]));
        case GPU::DestroyQueue: return result(s.destroyQueue(in[0]));
        case GPU::CreateBuffer: {
            uint64_t handle = 0, bytes = 0;
            auto r = s.createBuffer(in[0], in[1], in[2], handle, bytes);
            if (r != GPU::Result::Success) return result(r);
            auto *memory = QcomGPUBuffer::allocate(bytes, fEngine.ready() ? fMapper : nullptr);
            if (!memory) { s.destroyBuffer(handle); return kIOReturnNoMemory; }
            c->fMemory[handle & 0xff] = memory;
            a->scalarOutput[0] = handle; a->scalarOutput[1] = bytes; return kIOReturnSuccess;
        }
        case GPU::DestroyBuffer: {
            if (s.buffers.find(in[0]) && c->fMemory[in[0] & 0xff] && c->fMemory[in[0] & 0xff]->pending)
                return kIOReturnBusy;
            auto r = s.destroyBuffer(in[0]);
            if (r == GPU::Result::Success) OSSafeReleaseNULL(c->fMemory[in[0] & 0xff]);
            return result(r);
        }
        case GPU::ReadBuffer: case GPU::WriteBuffer: {
            size_t bytes = selector == GPU::ReadBuffer ? a->structureOutputSize : a->structureInputSize;
            auto r = s.validateCopy(in[0], in[1], bytes);
            if (r != GPU::Result::Success) return result(r);
            auto *memory = c->fMemory[in[0] & 0xff];
            if (!memory) return kIOReturnNotReady;
            if (memory->pending) return kIOReturnBusy;
            auto *base = static_cast<uint8_t *>(memory->bytes()) + in[1];
            if (selector == GPU::ReadBuffer) memcpy(a->structureOutput, base, bytes);
            else memcpy(base, a->structureInput, bytes);
            return kIOReturnSuccess;
        }
        case GPU::Submit: {
            auto r = s.validateSubmit(in[0], in[1], in[2], in[3]);
            return r == GPU::Result::Success ? submit(c, a) : result(r);
        }
        default: return kIOReturnUnsupported;
    }
}
IOReturn QcomGPU::setPowerState(unsigned long state, IOService *)
{
    if (state > 1) return kIOPMNoSuchState;
    if (fAdded) request(nullptr, state ? PowerOn : PowerOff);
    return IOPMAckImplied;
}
void QcomGPU::systemWillShutdown(IOOptionBits specifier)
{
    if (specifier==kIOMessageSystemWillPowerOff || specifier==kIOMessageSystemWillRestart) {
        // Native shutdown does not terminate a still-published IOService.
        // Stop DMA while its ring, page tables and power leases remain owned.
        if (fAdded) request(nullptr,Shutdown);
        else stopHardware();
    }
    super::systemWillShutdown(specifier);
}
void QcomGPU::stop(IOService *provider)
{
    if (fAdded) request(nullptr, Shutdown);
    else stopHardware();
    if (fPM) { PMstop(); fPM = false; }
    // Finish the Family's notification/fence bookkeeping after waking native
    // submissions, before IOGPU::stop releases its fence machine/scheduler.
    if (fAdded && fGate->runAction(nativeDrainAction) != kIOReturnSuccess) return;
    if (fAdded) {
        closeNativeClients(); // native queue cleanup can wait; keep the gate open
        if (fGate->runAction(nativeDrainAction) != kIOReturnSuccess) return;
    }
    if (fStarted || fNativeStarting) {
        fStarted = false; fNativeStarting = false;
        super::stop(provider);
    }
    releaseHardware();
    // Keep the gate alive until free: outstanding clients can close safely.
}
void QcomGPU::free()
{
    releaseHardware();
    if(fQuarantined)return;
    if (fAdded) { fLoop->removeEventSource(fGate); fAdded = false; }
    OSSafeReleaseNULL(fGate); OSSafeReleaseNULL(fLoop);
    OSSafeReleaseNULL(fStampMemory);
    super::free();
}
bool QcomGPU::nativeShaderTask(task_t task)
{
    return task && (IOUserClient::clientHasPrivilege(task,kIOClientPrivilegeAdministrator)==kIOReturnSuccess ||
        compositorTask(task) || nativeDisplayClientTask(task) || nativePosterTask(task));
}
bool QcomGPU::nativeCompositorTask(task_t task) { return task && compositorTask(task); }
IOReturn QcomGPU::newUserClient(task_t task, void *token, UInt32 type, OSDictionary *properties, IOUserClient **out)
{
    if (!out) return kIOReturnBadArgument;
    *out = nullptr;
    bool administrator=task && IOUserClient::clientHasPrivilege(task,kIOClientPrivilegeAdministrator)==kIOReturnSuccess;
    bool native=type!=GPU::ConnectionType && ((type&0xffff)==1 || (type&0xffff)==33);
    if (!task || (!administrator && (!native || !nativeShaderTask(task))))
        return kIOReturnNotPrivileged;
    if (!fStarted || isInactive()) return kIOReturnNotReady;
    if (type != GPU::ConnectionType) {
        // Native type 1 (device) and 33 (memory information), including the
        // high-word options, are dispatched by the actual Apple Family.
        if ((type & 0xffff) != 1 && (type & 0xffff) != 33) return kIOReturnUnsupported;
        return fGate->runAction(nativeOpenAction, task, token,
            reinterpret_cast<void *>(uintptr_t(type)), out);
    }
    auto *client = new QcomGPUUserClient;
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(task, token, type, properties)) { client->release(); return kIOReturnError; }
    if (!client->attach(this)) { client->release(); return kIOReturnNotAttached; }
    if (!client->start(this)) { client->detach(this); client->release(); return kIOReturnNotReady; }
    *out = client; return kIOReturnSuccess;
}
