// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "GPUJITProtocol.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Internal compiler ABI only. Neither LLVM nor NIR objects cross the dylib
// boundary, and these fields are never uploaded to the kernel.
typedef struct QGPUAIRConstant {
    uint32_t index, type;
    uint8_t bytes[32];
} QGPUAIRConstant;
typedef struct QGPUAIRNIRTiming {
    uint64_t lowerNS, backendNS;
} QGPUAIRNIRTiming;
__attribute__((visibility("default"))) int qgpu_compile_air_nir(const void *, size_t, const char *,
                                                                const QGPUAIRConstant *, unsigned, uint64_t,
                                                                QGPUProgramImage *, QGPUAIRNIRTiming *, char *,
                                                                unsigned);
struct nir_shader;
int qgpu_compile_nir(struct nir_shader *, const QGPUProgramDescription *, QGPUProgramImage *, char *, unsigned);
#ifdef __cplusplus
}
#endif
