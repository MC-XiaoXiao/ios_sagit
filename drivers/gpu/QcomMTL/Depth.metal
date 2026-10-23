#include <metal_stdlib>
using namespace metal;

kernel void depth_read(depth2d<float, access::read> image [[texture(0)]],
                       depth2d_array<float, access::read> layers [[texture(1)]],
                       depth2d_ms<float> samples [[texture(2)]],
                       depth2d_ms_array<float> sampleLayers [[texture(3)]],
                       device float4 *output [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    uint2 p=uint2(i,0u);
    output[i]=float4(image.read(p),layers.read(p,layers.get_array_size()-1u),
                     samples.read(p,samples.get_num_samples()-1u),
                     sampleLayers.read(p,sampleLayers.get_array_size()-1u,1u));
}

kernel void depth_query(depth2d<float> image [[texture(0)]],
                        depth2d_array<float> layers [[texture(1)]],
                        depth2d_ms<float> samples [[texture(2)]],
                        depth2d_ms_array<float> sampleLayers [[texture(3)]],
                        device uint4 *output [[buffer(0)]]) {
    output[0]=uint4(image.get_width(),layers.get_array_size(),samples.get_num_samples(),sampleLayers.get_array_size());
}

kernel void depth_sample(depth2d<float> image [[texture(0)]],
                         depth2d_array<float> layers [[texture(1)]], sampler filtering [[sampler(3)]],
                         device float4 *output [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    constexpr sampler shadow(compare_func::less_equal,min_filter::nearest,mag_filter::linear,mip_filter::none);
    float2 uv=float2((float(i)+0.5)/float(image.get_width()),0.5);
    output[i]=float4(image.sample(filtering,uv),image.sample_compare(filtering,uv,0.5),
        layers.sample(shadow,uv,0u,level(0.0)),layers.sample_compare(shadow,uv,0u,0.5,level(1.0)));
}

vertex float4 depth_vertex(device const float4 *vertices [[buffer(0)]],
                           depth2d<float> image [[texture(2)]], depth2d_array<float> layers [[texture(4)]],
                           sampler filtering [[sampler(1)]], uint i [[vertex_id]]) {
    float d=image.read(uint2(i,0u))+layers.sample_compare(filtering,float2(0.5),0u,0.5);
    return vertices[i]+float4(d*0.01,0.0,0.0,0.0);
}

fragment float4 depth_sample_fragment(depth2d<float> image [[texture(0)]],
                               depth2d_array<float> layers [[texture(2)]],sampler filtering [[sampler(4)]],
                               float4 pixel [[position]]) {
    constexpr sampler shadow(filter::linear,compare_func::greater);
    float2 uv=pixel.xy/float2(image.get_width(),image.get_height());
    return float4(image.sample(filtering,uv),image.sample_compare(filtering,uv,0.5,bias(0.0)),
        layers.read(uint2(pixel.xy),0u),layers.sample_compare(shadow,uv,0u,0.5,gradient2d(float2(0.01,0.0),float2(0.0,0.01))));
}

fragment float4 depth_multisample_fragment(depth2d_ms<float> image [[texture(2)]],
                                           depth2d_ms_array<float> layers [[texture(6)]],float4 pixel [[position]]) {
    return float4(image.read(uint2(pixel.xy),1u),layers.read(uint2(pixel.xy),0u,1u),0.0,1.0);
}
