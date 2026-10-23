/* Qualcomm ARMv8 non-secure GICv3/PSCI bootstrap. Only timer/IPI sources
 * are enabled. pe_sdm835_* symbol names are retained for existing kext ABI. */
#include <pexpert/arm64/board_config.h>
#if defined(QCOM_ARM64)
#include <stdint.h>
#include <arm64/proc_reg.h>
#include <kern/debug.h>
#include <kern/clock.h>
#include <arm/rtclock.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/machine_cpu.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/arm_smp.h>
#include <os/atomic_private.h>

static vm_offset_t sdm835_gicr;
static bool sdm835_gic_security_disabled;
static int sdm835_timer_trace;
static unsigned sdm835_boot_timer_ticks;
static uint32_t sdm835_timer_cpus;
static uint32_t sdm835_ipi_cpus;
extern bool PE_dispatch_irq(unsigned int intid);
static uint32_t
sdm835_read32(void *base, uint32_t offset)
{
	return *(volatile uint32_t *)((uintptr_t)base + offset);
}

static uint64_t
sdm835_read64(void *base, uint32_t offset)
{
	return *(volatile uint64_t *)((uintptr_t)base + offset);
}

static void
sdm835_write32(void *base, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)((uintptr_t)base + offset) = value;
	__builtin_arm_dsb(DSB_SY);
}

static void
sdm835_barrier(void *unused __unused)
{
	__builtin_arm_dsb(DSB_SY);
}

static uint64_t
sdm835_counter(void *unused __unused)
{
	return __builtin_arm_rsr64("CNTVCT_EL0");
}

static void
sdm835_relax(void *unused __unused)
{
	__asm__ volatile("yield");
}

void
pe_sdm835_init_interrupts(void)
{
	/* Firmware must enable distributor ARE_NS and Group1NS forwarding.
	 * Don't alter secure ownership or take over existing SPIs at EL1. */
	vm_offset_t base = os_atomic_load(&sdm835_gicr, acquire);
	if (!base) {
		/* Only the boot CPU may allocate this shared mapping. All subsequent
		 * CPU starts consume it after release/acquire publication. */
		if (cpu_number() != master_cpu) {
			panic(QCOM_PLATFORM_NAME ": secondary CPU reached GIC before bootstrap");
		}
		PE_parse_boot_argn("storage-trace", &sdm835_timer_trace, sizeof(sdm835_timer_trace));
		vm_offset_t distributor = ml_io_map(QCOM_GICD_BASE, QCOM_GICD_SIZE);
		if (!distributor) {
			panic(QCOM_PLATFORM_NAME ": cannot map distributor");
		}
		uint32_t control = sdm835_read32((void *)distributor, 0);
		if ((control & 0x12) != 0x12) {
			panic(QCOM_PLATFORM_NAME ": firmware must enable affinity routing and Group1NS (0x%x)", control);
		}
		sdm835_gic_security_disabled = (control & 0x40) != 0;
		base = ml_io_map(QCOM_GICR_BASE, QCOM_GICR_SIZE);
		if (!base) {
			panic(QCOM_PLATFORM_NAME ": cannot map redistributors");
		}
		os_atomic_store(&sdm835_gicr, base, release);
	}
	uint32_t affinity = __builtin_arm_rsr64("MPIDR_EL1") & 0xffffff;
	if (!arm_smp_valid_affinity(affinity)) {
		panic(QCOM_PLATFORM_NAME ": unsupported CPU affinity 0x%x", affinity);
	}
	struct arm_gic_io io = { (void *)base, sdm835_read32, sdm835_read64,
	                        sdm835_write32, sdm835_barrier, sdm835_counter, sdm835_relax };
	uint32_t local = 0;
	enum arm_gic_result result = arm_gic_find_cpu(&io, QCOM_GICR_SIZE, affinity, &local);
	if (result == ARM_GIC_OK) {
		result = arm_gic_init_cpu(&io, local, QCOM_VTIMER_INTID,
		    QCOM_IPI_INTID, QCOM_TIMER_HZ, sdm835_gic_security_disabled);
	}
	if (result != ARM_GIC_OK) {
		panic(QCOM_PLATFORM_NAME ": CPU 0x%x local GIC initialization failed (%u)", affinity, result);
	}
	__builtin_arm_wsr64("ICC_SRE_EL1", __builtin_arm_rsr64("ICC_SRE_EL1") | 1);
	__builtin_arm_isb(ISB_SY);
	if (!(__builtin_arm_rsr64("ICC_SRE_EL1") & 1)) {
		panic(QCOM_PLATFORM_NAME ": firmware did not permit GIC system registers");
	}
	__builtin_arm_wsr64("ICC_IGRPEN1_EL1", 0);
	__builtin_arm_isb(ISB_SY);
	__builtin_arm_wsr64("ICC_PMR_EL1", 0xff);
	__builtin_arm_wsr64("ICC_BPR1_EL1", 0);
	__builtin_arm_wsr64("ICC_CTLR_EL1", __builtin_arm_rsr64("ICC_CTLR_EL1") & ~2ULL);
	__builtin_arm_wsr64("ICC_IGRPEN1_EL1", 1);
	__builtin_arm_isb(ISB_SY);
	kprintf("[ARM SMP] interrupts cpu=%u affinity=0x%x redistributor=0x%x\n",
	    cpu_number(), affinity, local);
	if (sdm835_timer_trace) {
		kprintf("[" QCOM_PLATFORM_NAME " timer] group=%08x enable=%08x CTL=%llx TVAL=%llx\n",
		    io.read32(io.context, local + ARM_GICR_IGROUPR0),
		    io.read32(io.context, local + ARM_GICR_ISENABLER0),
		    __builtin_arm_rsr64("CNTV_CTL_EL0"), __builtin_arm_rsr64("CNTV_TVAL_EL0"));
	}
}

void
pe_sdm835_handle_irq(void)
{
	uint64_t iar = __builtin_arm_rsr64("ICC_IAR1_EL1");
	unsigned int id = iar & 0xffffff;
	if (id >= 1020 && id <= 1023) {
		return;
	}
	if (id == QCOM_VTIMER_INTID) {
		uint32_t bit = 1U << cpu_number();
		if (!(os_atomic_or_orig(&sdm835_timer_cpus, bit, relaxed) & bit)) {
			kprintf("[ARM SMP] timer cpu=%u\n", cpu_number());
		}
		if(sdm835_timer_trace && !(__builtin_arm_rsr64("MPIDR_EL1")&0xffff) && sdm835_boot_timer_ticks++<3)kprintf("[" QCOM_PLATFORM_NAME " timer] IRQ%u tick %u\n",QCOM_VTIMER_INTID,sdm835_boot_timer_ticks);
		getCpuDatap()->cpu_decrementer = -1;
		rtclock_intr(TRUE);
	} else if (id == QCOM_IPI_INTID) {
		/* Retire this SGI before consuming the software signal mask so
		 * another core can deliver a fresh SGI while we process it. */
		__builtin_arm_wsr64("ICC_EOIR1_EL1", iar);
		__builtin_arm_isb(ISB_SY);
		cpu_signal_handler();
		uint32_t bit = 1U << cpu_number();
		if (!(os_atomic_or_orig(&sdm835_ipi_cpus, bit, relaxed) & bit)) {
			kprintf("[ARM SMP] ipi cpu=%u\n", cpu_number());
		}
		return;
	} else if (id >= 32 && id < 1020 && PE_dispatch_irq(id)) {
		/* Apple PassthruInterruptController forwards the acknowledged INTID
		 * to the IOKit GIC driver. Do not read IAR again in the child. */
	} else {
		panic(QCOM_PLATFORM_NAME ": unregistered GIC interrupt %u", id);
	}
	/* Complete peripheral writes before priority drop/deactivation. */
	__builtin_arm_dsb(DSB_SY);
	__builtin_arm_wsr64("ICC_EOIR1_EL1", iar);
	__builtin_arm_isb(ISB_SY);
}
#endif

#if defined(QCOM_ARM64)
/* SMCCC 1.0: MSM8998 and SM8150 PSCI firmware use the SMC conduit. */
static int64_t
sdm835_psci(uint64_t function, uint64_t arg1, uint64_t arg2)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = arg1;
	register uint64_t x2 __asm__("x2") = arg2;
	register uint64_t x3 __asm__("x3") = 0;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
	    : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12",
	        "x13", "x14", "x15", "x16", "x17", "memory", "cc");
	/* PSCI status/version results are signed 32-bit values, including the
	 * SMC64 calls. Firmware need not sign-extend their upper word. */
	return (int32_t)x0;
}

int64_t
pe_sdm835_cpu_on(uint32_t mpidr, uint64_t entry)
{
	if (!arm_smp_valid_affinity(mpidr) || !entry || (entry & 3) || entry >= (1ULL << 40)) {
		return -2; /* PSCI_INVALID_PARAMETERS; don't enter firmware on bad input. */
	}
	int64_t version = sdm835_psci(0x84000000, 0, 0);
	if (version < 2) {
		return -1; /* CPU_ON requires PSCI 0.2 or later. */
	}
	__builtin_arm_dsb(DSB_SY);
	int64_t status = sdm835_psci(0xc4000003, mpidr, entry);
	kprintf("[ARM SMP] CPU_ON affinity=0x%x entry=0x%llx status=%lld\n", mpidr, entry, status);
	return status;
}

void
pe_sdm835_cpu_off(void)
{
	int64_t status = sdm835_psci(0x84000002, 0, 0);
	panic(QCOM_PLATFORM_NAME ": PSCI CPU_OFF returned %lld", status);
}

void
pe_sdm835_send_ipi(uint32_t mpidr)
{
	uint64_t sgi;
	if (!arm_smp_sgi(mpidr, QCOM_IPI_INTID, &sgi)) {
		panic(QCOM_PLATFORM_NAME ": invalid IPI affinity 0x%x", mpidr);
	}
	__builtin_arm_dsb(DSB_ISHST);
	__builtin_arm_wsr64("ICC_SGI1R_EL1", sgi);
	__builtin_arm_isb(ISB_SY);
}
#endif

#if defined(ARM64_BOARD_CONFIG_SDM835)
/* Only valid until arm_vm_init replaces the bootstrap identity map. */
uint64_t sdm835_boot_uart_table[512] __attribute__((aligned(4096)));
int sdm835_early_uart_active = 1;
void
sdm835_early_puts(const char *s)
{
    if (!sdm835_early_uart_active) return;
    volatile uint32_t *uart = (void *)0x0c1b0000;
    while (*s) {
        unsigned int timeout = 1000000;
        while (!(uart[2] & 8) && --timeout) __asm__ volatile("yield");
        if (!timeout) return;
        uart[0x40 / 4] = 1;
        __builtin_arm_dsb(DSB_SY);
        uart[0x70 / 4] = (unsigned char)*s++;
        __builtin_arm_dsb(DSB_SY);
    }
}
#endif
