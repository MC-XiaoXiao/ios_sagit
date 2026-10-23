#include <metal_stdlib>
using namespace metal;

// Bit-preserving attachment snapshots; format conversion happens only when
// the fragment shader reads its private native texture descriptor aliases.
kernel void framebuffer_snapshot(device const uint *source [[buffer(0)]],
                                 device uint *destination [[buffer(1)]],
                                 uint2 position [[thread_position_in_grid]]) {
    uint index=position.x+position.y*256u;
    destination[index]=source[index];
}

fragment float4 framebuffer_source_over(constant float4 &source [[buffer(0)]],float4 destination [[color(0)]]) {
    return source+destination*(1.0-source.a);
}

struct CompositionVertex {
    float4 position [[position]];
    float2 uv;
};

vertex CompositionVertex composition_vertex(device const float4 *positions [[buffer(0)]],
                                              device const float2 *coordinates [[buffer(1)]],
                                              uint i [[vertex_id]]) {
    CompositionVertex out;
    out.position=positions[i];out.uv=coordinates[i];return out;
}

fragment float4 composition_fragment(CompositionVertex in [[stage_in]],texture2d<float> image [[texture(0)]],
                                      sampler filtering [[sampler(0)]],constant float &opacity [[buffer(0)]]) {
    return image.sample(filtering,in.uv)*opacity;
}

fragment float4 composition_clear(constant float4 &color [[buffer(0)]]) {return color;}
