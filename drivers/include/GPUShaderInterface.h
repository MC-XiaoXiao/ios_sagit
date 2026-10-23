// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#define QGPU_MAX_VARYINGS 32
#define QGPU_MAX_CODE_WORDS 4096u
#define QGPU_MAX_INSTRUCTION_UNITS (QGPU_MAX_CODE_WORDS/32u)
#define QGPU_VARIABLE_LOCAL_SIZE (1u << 2)
#define QGPU_BOUNDED_BUFFERS (1u << 3)
// Exact admitted byte lengths, independent of A5xx RESINFO dimensions.
#define QGPU_BUFFER_SIZE_CONSTANTS (1u << 26)
// Native admission must prove every declared window complete and aligned.
#define QGPU_CONSTANT_WINDOWS_REQUIRED (1u << 27)
// Framebuffer fetch coordinates are relative to a native-owned tile origin.
// The bounds check and ordinary fragment coordinates remain attachment-global.
#define QGPU_FRAMEBUFFER_TILE_ORIGIN (1u << 28)
// A5xx shares SB4_SSBO between graphics stages. Metal binding namespaces are
// independent: VS uses [0,8), FS uses [8,16); CS has its own state block.
#define QGPU_STAGE_BUFFER_COUNT 8u
#define QGPU_STAGE_TEXTURE_COUNT 8u
// Metal binding indices are independent of the shader's compact descriptor
// set. At most eight textures may be enabled by one specialized AIR entry.
#define QGPU_METAL_TEXTURE_BINDING_COUNT 16u
#define QGPU_STAGE_SAMPLER_COUNT 8u
#define QGPU_COLOR_TARGET_COUNT 8u
// Fragment termination and scalar depth export are separate from late-Z mode.
#define QGPU_FRAGMENT_DISCARD (1u << 24)
#define QGPU_FRAGMENT_DEPTH (1u << 25)
#define QGPU_FRAGMENT_FLAGS (QGPU_FRAGMENT_DISCARD | QGPU_FRAGMENT_DEPTH)
// Separate from texture sampleMask. Registers are zero when their presence
// bit is absent; a live scalar is allowed to occupy register zero.
#define QGPU_SAMPLE_EXECUTION (1u << 0)
#define QGPU_SAMPLE_ID (1u << 1)
#define QGPU_SAMPLE_MASK_IN (1u << 2)
#define QGPU_SAMPLE_MASK_OUT (1u << 3)
#define QGPU_CENTROID_PERSPECTIVE (1u << 4)
#define QGPU_CENTROID_LINEAR (1u << 5)
#define QGPU_SAMPLE_PERSPECTIVE (1u << 6)
#define QGPU_SAMPLE_LINEAR (1u << 7)
#define QGPU_SAMPLE_MSAA (1u << 8)
#define QGPU_SAMPLE_FLAGS 511u
#define QGPU_HARDWARE_SAMPLER_COUNT 16u
#define QGPU_MAX_CONSTANT_SAMPLERS 16u
// Semantic state, never a hardware descriptor. Immutable AOT/JIT programs
// carry their own samplers; native submission builds the A5xx state packets.
typedef struct QGPUConstantSampler {
    uint32_t filters,minLOD,maxLOD,addressModes,maxAnisotropy,borderColor,normalized;
} QGPUConstantSampler;
// Integer fetches use aliases after the public sampled descriptors. Cube
// aliases are 2D arrays of faces and never consume a public sampler slot.
#define QGPU_FETCH_TEXTURE_BASE QGPU_STAGE_TEXTURE_COUNT
#define QGPU_GRAPHICS_TEXTURE_COUNT (2u * QGPU_STAGE_TEXTURE_COUNT)
// A5xx has sixteen stage-local descriptors. Private framebuffer reads occupy
// unused physical slots; both regular/MS instructions share the same view,
// selected by the uniform native attachment sample count.
static inline uint32_t qgpu_framebuffer_slot(uint32_t sampled,uint32_t fetched,uint32_t mask,uint32_t index) {
    if((sampled|fetched|mask)>255 || index>=QGPU_COLOR_TARGET_COUNT || !(mask&(1u<<index)))return UINT32_MAX;
    uint32_t occupied=sampled|(fetched<<QGPU_FETCH_TEXTURE_BASE);
    for(uint32_t i=0;i<=index;++i)if(mask&(1u<<i)) {
        uint32_t slot=0;while(slot<QGPU_GRAPHICS_TEXTURE_COUNT && (occupied&(1u<<slot)))++slot;
        if(slot==QGPU_GRAPHICS_TEXTURE_COUNT)return UINT32_MAX;
        occupied|=1u<<slot;if(i==index)return slot;
    }
    return UINT32_MAX;
}
#define QGPU_FRAGMENT_BUFFER_BASE QGPU_STAGE_BUFFER_COUNT
#define QGPU_GRAPHICS_BUFFER_COUNT (2u * QGPU_STAGE_BUFFER_COUNT)
#define QGPU_FRAGMENT_BUFFER_BANK (1u << 4)
#define QGPU_THREADGROUP_BARRIER (1u << 5)
#define QGPU_MAX_THREADGROUP_BYTES (32u * 1024u)
#define QGPU_DYNAMIC_THREADGROUP (1u << 6)
#define QGPU_DISPATCH_GEOMETRY (1u << 7)
// Per-texture type bits, independent of resource IDs and sampler bindings.
#define QGPU_ARRAY_TEXTURE_SHIFT 8u
#define QGPU_ARRAY_TEXTURE_MASK (255u << QGPU_ARRAY_TEXTURE_SHIFT)
#define QGPU_CUBE_TEXTURE_SHIFT 16u
#define QGPU_CUBE_TEXTURE_MASK (255u << QGPU_CUBE_TEXTURE_SHIFT)
#define QGPU_THREADGROUP_CONSTANT_WORDS (2u * QGPU_STAGE_BUFFER_COUNT)
#define QGPU_COMPUTE_IMAGE_COUNT 8u
#define QGPU_COMPUTE_IMAGE_BASE 8u
#define QGPU_COMPUTE_BUFFER_COUNT 16u
#define QGPU_IMAGE_CONSTANT_BASE QGPU_THREADGROUP_CONSTANT_WORDS
#define QGPU_IMAGE_CONSTANT_WORDS (4u * QGPU_COMPUTE_IMAGE_COUNT)
#define QGPU_IMAGE_MEMORY_CONSTANT_WORDS (QGPU_IMAGE_CONSTANT_BASE + QGPU_IMAGE_CONSTANT_WORDS)
#define QGPU_SAMPLER_CONSTANT_BASE QGPU_IMAGE_MEMORY_CONSTANT_WORDS
#define QGPU_SAMPLER_CONSTANT_WORDS 16u
#define QGPU_GRAPHICS_TEXTURE_CONSTANT_BASE QGPU_SAMPLER_CONSTANT_WORDS
#define QGPU_GRAPHICS_MEMORY_CONSTANT_WORDS (QGPU_GRAPHICS_TEXTURE_CONSTANT_BASE + 4u * QGPU_STAGE_TEXTURE_COUNT)
#define QGPU_FRAMEBUFFER_CONSTANT_BASE QGPU_GRAPHICS_MEMORY_CONSTANT_WORDS
#define QGPU_FRAMEBUFFER_MEMORY_CONSTANT_WORDS (QGPU_FRAMEBUFFER_CONSTANT_BASE + 4u * QGPU_COLOR_TARGET_COUNT)
// Graphics-only gap before buffer-size constants. Reserve a complete 64-byte
// CP_LOAD_STATE block so an indirect tile update cannot overwrite immediates.
// Compute array metadata uses this address in its separate stage bank.
#define QGPU_FRAMEBUFFER_TILE_ORIGIN_BASE QGPU_FRAMEBUFFER_MEMORY_CONSTANT_WORDS
#define QGPU_FRAMEBUFFER_TILE_ORIGIN_WORDS 16u
#define QGPU_FRAMEBUFFER_TILE_MEMORY_CONSTANT_WORDS (QGPU_FRAMEBUFFER_TILE_ORIGIN_BASE + QGPU_FRAMEBUFFER_TILE_ORIGIN_WORDS)
#define QGPU_SAMPLER_MEMORY_CONSTANT_WORDS (QGPU_SAMPLER_CONSTANT_BASE + QGPU_SAMPLER_CONSTANT_WORDS)
#define QGPU_DISPATCH_CONSTANT_BASE QGPU_SAMPLER_MEMORY_CONSTANT_WORDS
#define QGPU_DISPATCH_CONSTANT_WORDS 16u
#define QGPU_DISPATCH_MEMORY_CONSTANT_WORDS (QGPU_DISPATCH_CONSTANT_BASE + QGPU_DISPATCH_CONSTANT_WORDS)
#define QGPU_ARRAY_CONSTANT_BASE QGPU_DISPATCH_MEMORY_CONSTANT_WORDS
#define QGPU_ARRAY_CONSTANT_WORDS (2u * QGPU_COMPUTE_IMAGE_COUNT)
#define QGPU_ARRAY_MEMORY_CONSTANT_WORDS (QGPU_ARRAY_CONSTANT_BASE + QGPU_ARRAY_CONSTANT_WORDS)
#define QGPU_BUFFER_SIZE_CONSTANT_BASE QGPU_ARRAY_MEMORY_CONSTANT_WORDS
#define QGPU_BUFFER_SIZE_CONSTANT_WORDS QGPU_COMPUTE_BUFFER_COUNT
#define QGPU_MEMORY_CONSTANT_WORDS (QGPU_BUFFER_SIZE_CONSTANT_BASE + QGPU_BUFFER_SIZE_CONSTANT_WORDS)
#if QGPU_FRAMEBUFFER_TILE_MEMORY_CONSTANT_WORDS > QGPU_BUFFER_SIZE_CONSTANT_BASE
#error "Framebuffer tile-origin block overlaps buffer-size constants"
#endif
// Bounded GPU-to-constant-cache windows. Source offsets are logical buffer
// offsets, never user-supplied GPU addresses. CP_LOAD_STATE4 uses 64B units;
// flags occupy a complete vec4 after this program's data windows.
#define QGPU_MAX_CONSTANT_WINDOWS 4u
#define QGPU_CONSTANT_WINDOW_BYTES 64u
#define QGPU_CONSTANT_WINDOW_WORDS 16u
#define QGPU_CONSTANT_WINDOW_BASE QGPU_MEMORY_CONSTANT_WORDS
#define QGPU_MAX_MEMORY_CONSTANT_WORDS (QGPU_MEMORY_CONSTANT_WORDS + QGPU_MAX_CONSTANT_WINDOWS*QGPU_CONSTANT_WINDOW_WORDS + 4u)
typedef struct QGPUConstantWindow { uint32_t buffer,byteOffset; } QGPUConstantWindow;
static inline uint32_t qgpu_constant_window_flag(uint32_t count,uint32_t index) {
    return QGPU_CONSTANT_WINDOW_BASE+count*QGPU_CONSTANT_WINDOW_WORDS+index;
}
static inline uint32_t qgpu_constant_memory_words(uint32_t count) {
    return QGPU_CONSTANT_WINDOW_BASE+count*QGPU_CONSTANT_WINDOW_WORDS+4u;
}
// Full float32 scalar/vector interface. Semantic identity is SHA-256 truncated
// to 128 bits; hardware locations remain local to each independently compiled
// stage and are linked by identity, never by declaration order.
typedef struct QGPUVarying {
    uint32_t semantic[4];
    uint32_t components, mode, regid, mask, inloc, reserved;
} QGPUVarying;

#define QGPU_MAX_ATTRIBUTES 16
// Sparse Metal attribute locations, with real IR3 scalar register assignments.
// type: 0 float32, 1 int32, 2 uint32. Zero records are inactive.
typedef struct QGPUAttribute { uint32_t components,type,regid,mask; } QGPUAttribute;
