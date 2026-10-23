#include <metal_stdlib>
using namespace metal;

struct ResolveParameters {
    uint width;
    uint height;
    uint sourcePitch;
    uint destinationPitch;
    uint samples;
    uint depthFilter;
    uint depthPitch;
    uint stencilFilter;
};

kernel void resolve_depth(device const uint *source [[buffer(0)]],
                          device uint *destination [[buffer(1)]],
                          constant ResolveParameters &p [[buffer(3)]],
                          uint2 pixel [[thread_position_in_grid]]) {
    if (pixel.x >= p.width || pixel.y >= p.height) return;
    uint base = pixel.y * (p.sourcePitch / 4u) + pixel.x * p.samples;
    uint bits = source[base];
    float selected = as_type<float>(bits);
    for (uint sample = 1u; sample < p.samples; ++sample) {
        uint next = source[base + sample];
        float value = as_type<float>(next);
        if ((p.depthFilter == 1u && value < selected) || (p.depthFilter == 2u && value > selected)) {
            bits = next;
            selected = value;
        }
    }
    destination[pixel.y * (p.destinationPitch / 4u) + pixel.x] = bits;
}

kernel void resolve_stencil(device const uint *source [[buffer(0)]],
                            device uint *destination [[buffer(1)]],
                            device const uint *depth [[buffer(2)]],
                            constant ResolveParameters &p [[buffer(3)]],
                            uint2 word [[thread_position_in_grid]]) {
    if (word.x >= (p.width + 3u) / 4u || word.y >= p.height) return;
    uint index = word.y * (p.destinationPitch / 4u) + word.x;
    uint packed = destination[index];
    // Each invocation owns the complete destination word, including the
    // partial final pixel group. No competing byte RMW or stencil atomics.
    for (uint lane = 0u; lane < 4u; ++lane) {
        uint x = word.x * 4u + lane;
        if (x < p.width) {
            uint selectedSample = 0u;
            if (p.stencilFilter == 1u) {
                uint base = word.y * (p.depthPitch / 4u) + x * p.samples;
                float selected = as_type<float>(depth[base]);
                for (uint sample = 1u; sample < p.samples; ++sample) {
                    float value = as_type<float>(depth[base + sample]);
                    if ((p.depthFilter == 1u && value < selected) || (p.depthFilter == 2u && value > selected)) {
                        selected = value;
                        selectedSample = sample;
                    }
                }
            }
            uint offset = word.y * p.sourcePitch + x * p.samples + selectedSample;
            uint value = (source[offset / 4u] >> ((offset & 3u) * 8u)) & 255u;
            uint shift = lane * 8u;
            packed = (packed & ~(255u << shift)) | (value << shift);
        }
    }
    destination[index] = packed;
}
