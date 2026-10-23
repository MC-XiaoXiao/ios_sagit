#include <metal_stdlib>
using namespace metal;

kernel void depth_mip_read(depth2d<float,access::read> image [[texture(0)]],
                           depth2d_array<float,access::read> layers [[texture(1)]],
                           device float4 *output [[buffer(0)]],constant uint &levelIndex [[buffer(1)]],
                           uint i [[thread_position_in_grid]]) {
    if(levelIndex>=image.get_num_mip_levels() || levelIndex>=layers.get_num_mip_levels())return;
    uint width=image.get_width(levelIndex);
    uint2 p=uint2(i%width,i/width);
    output[i]=float4(image.read(p,levelIndex),layers.read(p,1u,levelIndex),
        float(width),float(layers.get_height(levelIndex)));
}

kernel void depth_mip_sample(depth2d<float> image [[texture(0)]],depth2d_array<float> layers [[texture(1)]],
                             sampler filtering [[sampler(0)]],device float4 *output [[buffer(0)]],
                             constant float &lod [[buffer(1)]],uint i [[thread_position_in_grid]]) {
    float2 uv=float2((float(i)+0.5)/float(image.get_width()),0.5);
    output[i]=float4(image.sample(filtering,uv,level(lod)),image.sample_compare(filtering,uv,0.5,level(lod)),
        layers.sample(filtering,uv,1u,level(lod)),layers.sample_compare(filtering,uv,1u,0.5,level(lod)));
}

vertex float4 depth_mip_vertex(device const float4 *vertices [[buffer(0)]],constant uint &levelIndex [[buffer(1)]],
                               depth2d<float> image [[texture(0)]],depth2d_array<float> layers [[texture(1)]],
                               sampler filtering [[sampler(2)]],uint i [[vertex_id]]) {
    float d=image.sample_compare(filtering,float2(0.5),0.5,level(float(levelIndex)))+
        layers.read(uint2(i,0u),0u,levelIndex);
    return vertices[i]+float4(d*0.01,0.0,0.0,0.0);
}

fragment float4 depth_mip_fragment(depth2d<float> image [[texture(0)]],depth2d_array<float> layers [[texture(1)]],
                                   sampler filtering [[sampler(2)]],constant float &lod [[buffer(0)]],float4 pixel [[position]]) {
    float2 uv=pixel.xy/float2(image.get_width(),image.get_height());
    float scale=exp2(lod);
    return float4(image.sample(filtering,uv,gradient2d(float2(scale/float(image.get_width()),0.0),float2(0.0,scale/float(image.get_height())))),
        image.sample_compare(filtering,uv,0.5,level(lod)),layers.sample(filtering,uv,0u,level(lod)),
        layers.sample_compare(filtering,uv,0u,0.5,level(lod)));
}

// Coordinate and gradient operands have separate, observable side effects.
kernel void gradient_operand_sample(texture2d<float> image [[texture(0)]],sampler filtering [[sampler(0)]],
                                     device atomic_uint *counters [[buffer(0)]],device float4 *output [[buffer(1)]],
                                     uint i [[thread_position_in_grid]]) {
    if(i!=0u)return;
    output[0]=image.sample(filtering,float2(float(atomic_fetch_add_explicit(&counters[0],1u,memory_order_relaxed))*0.01,0.5),
        gradient2d(float2(float(atomic_fetch_add_explicit(&counters[1],1u,memory_order_relaxed))*0.01,0.0),float2(0.0,0.01)));
}
