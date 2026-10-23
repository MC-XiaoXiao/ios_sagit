#ifndef XNU_VIRTUAL_UAT_H
#define XNU_VIRTUAL_UAT_H
#if !defined(ARM64_BOARD_CONFIG_VMAPPLE) && !defined(ARM64_BOARD_CONFIG_SDM835) && !defined(ARM64_BOARD_CONFIG_SM8150)
#error "A physical target requires its UAT implementation"
#endif
/* These targets have no Apple GPU UAT device. */
static inline void uat_bootstrap(void) {}
#endif
