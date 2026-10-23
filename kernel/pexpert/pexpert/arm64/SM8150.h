/* Snapdragon 855 / SM8150 Kryo 485 bring-up.
 * Register/topology source: Linux v6.12 arch/arm64/boot/dts/qcom/sm8150.dtsi.
 * Firmware must preserve DRAM, clocks and the non-secure GIC/PSCI handoff.
 */
#ifndef _PEXPERT_ARM64_SM8150_H
#define _PEXPERT_ARM64_SM8150_H
#define SM8150 1
#define QCOM_ARM64 1
#define QCOM_PLATFORM_NAME "SM8150"
#define NO_MONITOR 1
#define NO_CPU_OVRD 1
#define __ARM_AMP__ 1
#define __ARM_ARCH__ 8
#define __ARM_VMSA__ 8
#define __ARM_VFP__ 4
#define __ARM_COHERENT_CACHE__ 1
/* Keep device DMA cache maintenance enabled. */
#define __ARM_IC_NOALIAS_ICACHE__ 1
#define __ARM_DEBUG__ 7
#define __ARM_ENABLE_SWAP__ 1
#define __ARM_V8_CRYPTO_EXTENSIONS__ 1
#define ARM_ARCH_TIMER
#define HAS_GIC_V3 1
#define HAS_CLUSTER 1
#define CORE_NCTRS 0
#define BTI_ENFORCED 0
/* Conservative 4K/40-bit ARMv8-A profile until runtime feature probing exists.
 * Do not opt in to Apple registers, PAC, LSE, PAN or range TLBI implicitly. */
#define SM8150_GICD_BASE 0x17a00000
#define SM8150_GICD_SIZE 0x10000
#define SM8150_GICR_BASE 0x17a60000
#define SM8150_GICR_SIZE 0x100000
#define SM8150_TIMER_HZ 19200000
#define SM8150_VTIMER_INTID 19
#define SM8150_IPI_INTID 0
#define QCOM_GICD_BASE SM8150_GICD_BASE
#define QCOM_GICD_SIZE SM8150_GICD_SIZE
#define QCOM_GICR_BASE SM8150_GICR_BASE
#define QCOM_GICR_SIZE SM8150_GICR_SIZE
#define QCOM_TIMER_HZ SM8150_TIMER_HZ
#define QCOM_VTIMER_INTID SM8150_VTIMER_INTID
#define QCOM_IPI_INTID SM8150_IPI_INTID
#ifndef ASSEMBLER
/* Bounded GENI handoff is available only for an explicitly enabled DT UART.
 * GENI is not UARTDM; never reuse the SDM835 physical UART accessors. */
#define QCOM_GENI_UART
#define PLATFORM_PANIC_LOG_DISABLED
#endif
#endif
