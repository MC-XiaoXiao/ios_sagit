#include <metal_stdlib>
using namespace metal;

struct SampleVaryings {
    float4 position [[position]];
    float2 center [[center_perspective]];
    float2 edge [[centroid_perspective]];
    float2 linearEdge [[centroid_no_perspective]];
    float2 sample [[sample_perspective]];
    float2 linearSample [[sample_no_perspective]];
};

vertex SampleVaryings sample_vertex(device const float4 *vertices [[buffer(0)]],
                                    uint index [[vertex_id]]) {
    SampleVaryings result;
    result.position = vertices[index];
    result.center = vertices[index].xy;
    result.edge = vertices[index].xy;
    result.linearEdge = vertices[index].xy;
    result.sample = vertices[index].xy;
    result.linearSample = vertices[index].xy;
    return result;
}

fragment float4 sample_interpolation(SampleVaryings input [[stage_in]]) {
    float2 value = (input.center + input.edge + input.linearEdge + input.sample + input.linearSample) * 0.2;
    return float4(value, input.position.z, input.position.w);
}

struct CentroidVaryings {
    float2 edge [[centroid_perspective]];
    float2 linearEdge [[centroid_no_perspective]];
};

fragment float4 centroid_fragment(CentroidVaryings input [[stage_in]]) {
    return float4(input.edge, input.linearEdge);
}

struct SampleResult {
    float4 color [[color(0)]];
    uint coverage [[sample_mask]];
};

fragment SampleResult sample_coverage(uint sample [[sample_id]], uint coverage [[sample_mask]]) {
    SampleResult result;
    result.color = float4(float(sample) / 3.0, 0.5, 1.0, 1.0);
    result.coverage = coverage & (1u << sample);
    return result;
}

fragment void sample_store(uint sample [[sample_id]], float4 pixel [[position]],
                           device uint *samples [[buffer(0)]]) {
    if (pixel.x < 0.5) discard_fragment();
    samples[uint(pixel.x) * 4u + sample] = sample;
}
