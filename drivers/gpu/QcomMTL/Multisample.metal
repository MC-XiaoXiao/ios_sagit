#include <metal_stdlib>
using namespace metal;

kernel void multisample_array_copy(texture2d_ms_array<float> source [[texture(3)]],
                                   texture2d_array<float, access::write> destination [[texture(7)]],
                                   uint3 pixel [[thread_position_in_grid]]) {
    if (pixel.x >= source.get_width() || pixel.y >= source.get_height() || pixel.z >= source.get_array_size()) return;
    float4 value = float4(0.0);
    uint count = source.get_num_samples();
    for (uint sample = 0u; sample < count; ++sample) value += source.read(pixel.xy, pixel.z, sample);
    destination.write(value / float(count), pixel.xy, pixel.z);
}

kernel void multisample_array_query(texture2d_ms_array<float> source [[texture(2)]],
                                    device uint4 *result [[buffer(0)]]) {
    result[0] = uint4(source.get_width(), source.get_height(), source.get_array_size(), source.get_num_samples());
}

vertex float4 multisample_array_vertex(device const float4 *vertices [[buffer(0)]],
                                       texture2d_ms_array<float> source [[texture(2)]],
                                       uint index [[vertex_id]]) {
    return vertices[index] + source.read(uint2(index % source.get_width(), 0u), source.get_array_size() - 1u, source.get_num_samples() - 1u) * 0.01;
}

fragment float4 multisample_array_fragment(texture2d_ms_array<float> source [[texture(5)]],
                                           constant uint &layer [[buffer(0)]], float4 pixel [[position]]) {
    return source.read(uint2(pixel.xy), layer, 1u) + float4(float(source.get_array_size()), float(source.get_num_samples()), 0.0, 0.0);
}
