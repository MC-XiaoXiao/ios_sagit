#ifndef XNU_VIRTUAL_SART_H
#define XNU_VIRTUAL_SART_H
#if !defined(ARM64_BOARD_CONFIG_VMAPPLE) && !defined(ARM64_BOARD_CONFIG_SDM835) && !defined(ARM64_BOARD_CONFIG_SM8150)
#error "A physical target requires its SART implementation"
#endif
/* These targets have no Apple SART. This is not a Qualcomm SMMU driver. */
static inline void sart_bootstrap(void) {}
#endif
