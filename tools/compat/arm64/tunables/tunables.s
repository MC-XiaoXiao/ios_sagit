#if !defined(ARM64_BOARD_CONFIG_VMAPPLE) && !defined(ARM64_BOARD_CONFIG_SDM835) && !defined(ARM64_BOARD_CONFIG_SM8150)
#error "A physical Apple CPU requires its actual SoC tunables"
#endif
/* No Apple CPU here; Qualcomm targets retain firmware Kryo tunables. */
.macro APPLY_TUNABLES midr, temporary1, temporary2
.endmacro
