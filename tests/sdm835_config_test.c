#include <pexpert/arm64/board_config.h>
#include <arm64/proc_reg.h>
#if !defined(SDM835) || defined(VMAPPLE) || defined(APPLEVIRTUALPLATFORM)
#error Incorrect physical platform
#endif
#if defined(__ARM_16K_PG__) || defined(__ARM_MIXED_PAGE_SIZE__) || defined(__ARM_ARCH_8_1__) || defined(HAS_APPLE_PAC) || defined(__ARM_RANGE_TLBI__) || defined(__ARM_PAN_AVAILABLE__) || defined(HAS_ARM_FEAT_SME)
#error Unsupported feature selected
#endif
#if defined(__ARM_COHERENT_IO__)
#error SDM835 UFS requires real DMA cache maintenance
#endif
_Static_assert(MAX_CPUS == 8 && MAX_CPU_CLUSTERS == 2, "incorrect topology");
/* The current SDM835 header selects native Clutch/Edge using DT E/P types;
 * the old dualq opt-out assertion predates that platform scheduler change. */
_Static_assert(__ARM_AMP__ == 1, "heterogeneous CPU scheduling must remain enabled");
_Static_assert(ARM_PGBYTES == 4096, "expected 4K translation granule");
_Static_assert(TCR_IPS_VALUE == TCR_IPS_40BITS, "expected 40bit physical space");
_Static_assert(BTI_ENFORCED == 0 && CORE_NCTRS == 0, "unsupported Apple facility");
_Static_assert(SDM835_VTIMER_INTID == 19, "MSM8998 virtual timer is PPI3, INTID19");
