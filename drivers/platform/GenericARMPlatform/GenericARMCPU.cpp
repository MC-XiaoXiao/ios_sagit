#include "GenericARMCPU.h"
#include "ARMPlatformConfig.h"
#include <machine/machine_routines.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSArray.h>
#include <libkern/OSAtomic.h>
#include <IOKit/IOPlatformExpert.h>

#define super IOCPU
OSDefineMetaClassAndStructors(GenericARMCPU, super);
static SInt32 registeredCPUs;
static UInt32 claimedCPUs;

bool GenericARMCPU::start(IOService *provider)
{
    OSData *reg = OSDynamicCast(OSData, provider->getProperty("reg"));
    if (!reg || reg->getLength() != 4) return false;
    OSData *method = OSDynamicCast(OSData, provider->getProperty("enable-method"));
    if (!method || !ARMPlatformConfig::stringEquals(method->getBytesNoCopy(),
            method->getLength(), "psci")) return false;
    uint32_t physical = ARMPlatformConfig::readLE32(reg->getBytesNoCopy());
    int logical = ml_get_cpu_number(physical);
    if (logical < 0) return false; // XNU may exclude a core through cpus=N.
    const ml_topology_info_t *topology = ml_get_topology_info();
    if (!topology || topology->version != CPU_TOPOLOGY_VERSION) return false;
    const ml_topology_cpu_t *cpu = nullptr;
    for (unsigned int i = 0; i < topology->num_cpus; ++i) {
        if (topology->cpus[i].cpu_id == static_cast<unsigned int>(logical) &&
            topology->cpus[i].phys_id == physical) {
            cpu = &topology->cpus[i];
            break;
        }
    }
    if (!cpu) return false;
    fPhysicalID = physical;
    fInterruptController = OSDynamicCast(IOCPUInterruptController,
        getPlatform()->getProperty("GenericARMCPUInterruptController"));
    if (!fInterruptController) return false;
    fCPUName = provider->copyName();
    if (!fCPUName || !super::start(provider)) return false;
    // IOKit matching can run concurrently. Never pass the same processor to
    // ml_processor_register twice; its registration count is irreversible.
    UInt32 bit = 1U << cpu->cpu_id;
    if (OSBitOrAtomic(bit, &claimedCPUs) & bit) return false;
    setCPUNumber(cpu->cpu_id);
    setCPUState(kIOCPUStateUninitalized);
    ml_processor_info_t info = {};
    info.cpu_id = reinterpret_cast<cpu_id_t>(this);
    info.phys_id = cpu->phys_id;
    info.log_id = cpu->cpu_id;
    info.cluster_id = cpu->cluster_id;
    info.cluster_type = cpu->cluster_type;
    info.l2_cache_id = cpu->l2_cache_id;
    info.l2_cache_size = cpu->l2_cache_size;
    info.l3_cache_id = cpu->l3_cache_id;
    info.l3_cache_size = cpu->l3_cache_size;
    // No invented PIO registers, powergate stubs, PMU or idle callbacks.
    perfmon_interrupt_handler_func pmi;
    kern_return_t status = ml_processor_register(&info, &machProcessor, &ipi_handler, &pmi);
    if (status != KERN_SUCCESS) {
        panic("GenericARMCPU: CPU 0x%x registration failed (%d)", physical, status);
    }
    // ml_processor_register owns processor boot in !USE_APPLEARMSMP builds.
    // Calling processor_start here would attempt to boot the same CPU twice.
    unsigned int registered = static_cast<unsigned int>(OSIncrementAtomic(&registeredCPUs) + 1);
    kprintf("[ARM SMP] registered cpu=%u affinity=0x%x count=%u/%u\n",
        cpu->cpu_id, physical, registered, topology->num_cpus);
    if (registered == topology->num_cpus)
        ml_cpu_init_completed();
    registerService();
    return true;
}

void GenericARMCPU::free(void)
{
    OSSafeReleaseNULL(fCPUName);
    super::free();
}

void GenericARMCPU::initCPU(bool)
{
    if (!fInterruptEnabled) {
        fInterruptController->enableCPUInterrupt(this);
        fInterruptEnabled = true;
    }
    setCPUState(kIOCPUStateRunning);
    kprintf("[ARM SMP] online cpu=%u affinity=0x%x\n", getCPUNumber(), fPhysicalID);
}
void GenericARMCPU::quiesceCPU(void) { setCPUState(kIOCPUStateStopped); }
kern_return_t GenericARMCPU::startCPU(vm_offset_t, vm_offset_t)
{
    return KERN_NOT_SUPPORTED; // This target boots secondaries in XNU's machine layer.
}
void GenericARMCPU::haltCPU(void)
{
    panic("GenericARMCPU: system sleep is unavailable without a platform suspend backend");
}
const OSSymbol *GenericARMCPU::getCPUName(void) { return fCPUName; }

void GenericARMCPU::signalCPU(IOCPU *target)
{
    // SDM835's machine layer delivers SGI0 directly. Other !USE_APPLEARMSMP
    // targets can use the normal Apple interrupt-controller interface.
    OSArray *controllers = OSDynamicCast(OSArray, target->getProvider()->getProperty("IOInterruptControllers"));
    const OSSymbol *name = controllers && controllers->getCount() ?
        OSDynamicCast(OSSymbol, controllers->getObject(0)) : nullptr;
    IOInterruptController *controller = name ? getPlatform()->lookUpInterruptController(
        const_cast<OSSymbol *>(name)) : nullptr;
    if (!controller) panic("GenericARMCPU: no IPI interrupt controller");
    controller->sendIPI(target->getCPUNumber(), false);
}

bool GenericARMCPU::requestTerminate(IOService *, IOOptionBits) { return false; }
