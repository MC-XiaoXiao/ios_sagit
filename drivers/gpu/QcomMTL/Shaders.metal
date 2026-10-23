#include <metal_stdlib>
using namespace metal;

kernel void add(device const uint *input [[buffer(0)]],
                device uint *output [[buffer(1)]],
                uint index [[thread_position_in_grid]]) {
    output[index] = input[index] + 7u;
}

vertex float4 position(device const float4 *vertices [[buffer(0)]],
                       uint index [[vertex_id]]) {
    return vertices[index];
}

fragment float4 color() {
    return float4(0.25, 0.5, 0.75, 1.0);
}

fragment float4 textured(texture2d<float> image [[texture(0)]],
                         sampler filtering [[sampler(0)]],
                         float4 pixel [[position]]) {
    return image.sample(filtering, pixel.xy / float2(64.0, 64.0));
}

vertex float4 instanced_position(device const float4 *vertices [[buffer(0)]],
                                 device const float4 *offsets [[buffer(1)]],
                                 uint vertexIndex [[vertex_id]], uint instanceIndex [[instance_id]]) {
    return vertices[vertexIndex] + offsets[instanceIndex];
}

struct RasterVertex {
    float4 position [[position]];
    float4 tint [[user(color)]];
    float2 uv [[user(texcoord)]];
};
// Input member order intentionally differs from the vertex output: the
// pipeline links user semantics rather than source declaration positions.
struct RasterFragment {
    float2 uv [[user(texcoord)]];
    float4 tint [[user(color)]];
};

vertex RasterVertex varying_position(device const float4 *vertices [[buffer(0)]],
                                      device const float4 *colors [[buffer(1)]],
                                      device const float4 *coordinates [[buffer(2)]],
                                      uint index [[vertex_id]]) {
    RasterVertex out;
    out.position = vertices[index];
    out.tint = colors[index];
    out.uv = coordinates[index].xy;
    return out;
}

fragment float4 varying_color(RasterFragment in [[stage_in]]) {
    return in.tint;
}

fragment float4 varying_textured(texture2d<float> image [[texture(0)]],
                                  sampler filtering [[sampler(0)]],
                                  RasterFragment in [[stage_in]]) {
    return image.sample(filtering, in.uv) * in.tint;
}

struct AttributeInput {
    float3 position [[attribute(0)]];
    float4 color [[attribute(3)]];
    float2 offset [[attribute(7)]];
};
struct AttributeOutput {
    float4 position [[position]];
    float4 color [[user(attribute_color)]];
};
vertex AttributeOutput attribute_position(AttributeInput input [[stage_in]]) {
    AttributeOutput result;
    result.position = float4(input.position.xy + input.offset, input.position.z, 1.0);
    result.color = input.color;
    return result;
}
fragment float4 attribute_color(AttributeOutput input [[stage_in]]) { return input.color; }

// Dynamic loop, cyclic values and switch joins; buffer access still uses the
// invocation's own element. This kernel is also authored as AIR below.
kernel void control_flow(device const uint *input [[buffer(0)]],
                         device uint *output [[buffer(1)]],
                         uint i [[thread_position_in_grid]]) {
    uint a = input[i], b = a ^ 165u;
    for (uint n = 0u; n < ((i & 7u) + 1u); ++n) {
        uint previous = a;
        a = b;
        b = previous;
    }
    switch (i & 3u) {
        case 0u: a += 11u; break;
        case 1u: a ^= 17u; break;
        default: a += b; break;
    }
    if ((i & 1u) == 0u) { output[i] = a; return; }
    output[i] = a + b;
}

// Emit one X row from the last Y/Z plane; all compute coordinates are observed.
kernel void workgroup_coordinates(device uint *output [[buffer(0)]],
    uint3 gridID [[thread_position_in_grid]],
    uint3 localID [[thread_position_in_threadgroup]],
    uint3 groupID [[threadgroup_position_in_grid]],
    uint3 localSize [[threads_per_threadgroup]],
    uint3 groupCount [[threadgroups_per_grid]],
    uint3 gridSize [[threads_per_grid]],
    uint linearID [[thread_index_in_threadgroup]]) {
    if (gridID.y != gridSize.y-1u || gridID.z != gridSize.z-1u) return;
    output[gridID.x] = gridID.x*1u + gridID.y*3u + gridID.z*5u + localID.x*7u + localID.y*11u + localID.z*13u + groupID.x*17u + groupID.y*19u + groupID.z*23u + localSize.x*29u + localSize.y*31u + localSize.z*37u + groupCount.x*41u + groupCount.y*43u + groupCount.z*47u + gridSize.x*53u + gridSize.y*59u + gridSize.z*61u + linearID*67u;
}

struct Parameters {
    uint width;
    uint height;
    uint sourceOffset;
    uint count;
    float3 tint;
    float gain;
};
// Ordinary row-major indexing and an inline constant block with float3 padding.
kernel void buffer_indexing(device const uint *input [[buffer(0)]],
    device uint *output [[buffer(1)]], constant Parameters &parameters [[buffer(2)]],
    uint3 id [[thread_position_in_grid]]) {
    if (id.x>=parameters.width || id.y>=parameters.height || id.z!=0u) return;
    uint index=id.y*parameters.width+id.x;
    if (index>=parameters.count) return;
    output[index]=input[parameters.sourceOffset+index]+uint(parameters.tint.y*parameters.gain);
}

struct FragmentParameters {
    uint width;
    uint height;
    uint outputOffset;
    uint paletteOffset;
    float4 tint;
};
// Fragment slots are independent of vertex slots, including buffer(0).
fragment float4 buffer_color(float4 position [[position]],
    device const float4 *colors [[buffer(0)]],
    constant FragmentParameters &parameters [[buffer(1)]],
    device uint *output [[buffer(7)]]) {
    uint x=uint(position.x), y=uint(position.y);
    if (x>=parameters.width || y>=parameters.height) return float4(0.0);
    uint index=y*parameters.width+x;
    output[parameters.outputOffset+index]=index+17u;
    return colors[parameters.paletteOffset+index]*parameters.tint;
}

// A one-dimensional reduction using actual workgroup-local storage.
kernel void threadgroup_reduce(device const uint *input [[buffer(0)]],
    device uint *output [[buffer(1)]], uint lane [[thread_index_in_threadgroup]],
    uint group [[threadgroup_position_in_grid]], uint count [[threads_per_threadgroup]]) {
    threadgroup uint scratch[64];
    scratch[lane]=input[group*count+lane];
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lane==0u) {
        uint sum=0u;
        for (uint n=0u;n<min(count,64u);++n) sum+=scratch[n];
        output[group]=sum;
    }
}

kernel void atomic_counter(device atomic_uint *counter [[buffer(0)]],
    device uint *output [[buffer(1)]], uint id [[thread_position_in_grid]],
    uint lane [[thread_index_in_threadgroup]], uint group [[threadgroup_position_in_grid]],
    uint groups [[threadgroups_per_grid]], uint count [[threads_per_threadgroup]]) {
    threadgroup atomic_uint localCounter;
    if (lane==0u) atomic_store_explicit(&localCounter,0u,memory_order_relaxed);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint localTicket=atomic_fetch_add_explicit(&localCounter,1u,memory_order_relaxed);
    uint ticket=atomic_fetch_add_explicit(&counter[0],1u,memory_order_relaxed);
    output[id*2u]=ticket;output[id*2u+1u]=localTicket;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lane==0u) output[groups*count*2u+group]=atomic_load_explicit(&localCounter,memory_order_relaxed);
}

kernel void atomic_values(device atomic_int *values [[buffer(0)]],
    device uint *output [[buffer(1)]], uint id [[thread_position_in_grid]]) {
    atomic_store_explicit(&values[id],int(id)+16,memory_order_relaxed);
    int minimum=atomic_fetch_min_explicit(&values[id],-7,memory_order_relaxed);
    int maximum=atomic_fetch_max_explicit(&values[id],-3,memory_order_relaxed);
    int expected=0;
    bool failed=atomic_compare_exchange_weak_explicit(&values[id],&expected,9,memory_order_relaxed,memory_order_relaxed);
    bool success=atomic_compare_exchange_strong_explicit(&values[id],&expected,int(id)+4,memory_order_relaxed,memory_order_relaxed);
    output[id*5u]=uint(minimum);output[id*5u+1u]=uint(maximum);output[id*5u+2u]=uint(expected);
    output[id*5u+3u]=uint(atomic_load_explicit(&values[id],memory_order_relaxed));
    output[id*5u+4u]=(failed?1u:0u)|(success?2u:0u);
}

// Sparse threadgroup slots and buffer(0) use independent binding namespaces.
kernel void dynamic_threadgroup(device uint *output [[buffer(0)]],
    threadgroup uint *scratch [[threadgroup(0)]],
    threadgroup float3 *vectors [[threadgroup(3)]],
    threadgroup atomic_int *counter [[threadgroup(7)]],
    uint id [[thread_position_in_grid]], uint lane [[thread_index_in_threadgroup]],
    uint group [[threadgroup_position_in_grid]], uint groups [[threadgroups_per_grid]],
    uint count [[threads_per_threadgroup]]) {
    threadgroup uint seeds[3];
    if (lane==0u) {
        seeds[0]=group+11u;seeds[1]=count;seeds[2]=5u;
        atomic_store_explicit(&counter[0],-7,memory_order_relaxed);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    scratch[lane]=id*3u+seeds[0];
    vectors[lane]=float3(float(lane),float(seeds[1]),float(seeds[2]));
    scratch[lane]+=uint(vectors[lane].x+vectors[lane].y+vectors[lane].z);
    int ticket=atomic_fetch_add_explicit(&counter[0],1,memory_order_relaxed);
    output[id*2u]=scratch[lane];output[id*2u+1u]=uint(ticket+7);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lane==0u) {
        uint sum=0u;
        for (uint n=0u;n<count;++n) sum+=scratch[n];
        output[groups*count*2u+group*2u]=sum;
        output[groups*count*2u+group*2u+1u]=uint(atomic_load_explicit(&counter[0],memory_order_relaxed));
    }
}

fragment float4 mip_textured(texture2d<float> image [[texture(0)]],
    sampler filtering [[sampler(0)]], float4 pixel [[position]]) {
    return image.sample(filtering,pixel.xy/float2(8.0),level(pixel.y*0.25));
}

// Each native dispatch binds nonoverlapping source/destination mip planes.
// A540 filtering decodes sRGB before interpolation; the image helper encodes
// RGB for storage and keeps alpha linear. The same kernel handles UNorm/NPOT.
kernel void mipmap_rgba8(texture2d<float> input [[texture(0)]],
    texture2d<float,access::write> output [[texture(7)]], sampler filtering [[sampler(0)]],
    uint index [[thread_position_in_grid]]) {
    uint width=output.get_width(),height=output.get_height();
    if (index>=width*height) return;
    uint2 pixel=uint2(index%width,index/width);
    float2 uv=(float2(pixel)+float2(0.5))/float2(float(width),float(height));
    output.write(input.sample(filtering,uv,level(0.0)),pixel);
}

kernel void image_transform(texture2d<float,access::read> input [[texture(0)]],
    texture2d<float,access::write> output [[texture(7)]], constant uint &level [[buffer(0)]],
    uint2 id [[thread_position_in_grid]]) {
    if(id.x>=output.get_width(level) || id.y>=output.get_height(level))return;
    float4 color=input.read(id,level);
    output.write(float4(color.b,color.g,1.0-color.r,color.a),id,level);
}

// Compute sampler and texture indices are independent; output stays an image.
kernel void sample_texture(texture2d<float> source [[texture(5)]],sampler filtering [[sampler(2)]],
    texture2d<float,access::write> destination [[texture(7)]],constant float &lod [[buffer(0)]],
    uint2 id [[thread_position_in_grid]]) {
    if(id.x>=destination.get_width() || id.y>=destination.get_height())return;
    float2 uv=(float2(id)+float2(0.5))/float2(float(destination.get_width()),float(destination.get_height()));
    destination.write(source.sample(filtering,uv,level(lod)),id);
}

kernel void sample_gradient(texture2d<float> source [[texture(5)]],sampler filtering [[sampler(2)]],
    texture2d<float,access::write> destination [[texture(7)]],constant float2 &dx [[buffer(0)]],constant float2 &dy [[buffer(1)]],
    uint2 id [[thread_position_in_grid]]) {
    if(id.x>=destination.get_width() || id.y>=destination.get_height())return;
    float2 uv=(float2(id)+float2(0.5))/float2(float(destination.get_width()),float(destination.get_height()))*2.0-0.5;
    destination.write(source.sample(filtering,uv,gradient2d(dx,dy)),id);
}
fragment float4 gradient_textured(texture2d<float> source [[texture(0)]],sampler filtering [[sampler(0)]],float4 pixel [[position]]) {
    return source.sample(filtering,pixel.xy/float2(16.0),gradient2d(float2(0.25,0.0),float2(0.0,0.25)));
}

// Array indices are separate from 2D coordinates and mip levels.
kernel void array_copy(texture2d_array<float,access::read> source [[texture(0)]],
    texture2d_array<float,access::write> destination [[texture(7)]],constant uint &lod [[buffer(0)]],
    uint3 id [[thread_position_in_grid]]) {
    if(id.x>=destination.get_width(lod) || id.y>=destination.get_height(lod) || id.z>=destination.get_array_size())return;
    destination.write(source.read(id.xy,id.z,lod),id.xy,id.z,lod);
}
fragment float4 array_textured(texture2d_array<float> source [[texture(0)]],sampler filtering [[sampler(0)]],
    constant uint2 &sliceAndLod [[buffer(0)]],float4 pixel [[position]]) {
    return source.sample(filtering,pixel.xy/float2(16.0),sliceAndLod.x,level(float(sliceAndLod.y)));
}

vertex RasterVertex sampled_vertex(device const float4 *vertices [[buffer(0)]],
    texture2d<float> image [[texture(0)]],sampler filtering [[sampler(0)]],constant float &lod [[buffer(1)]],
    uint index [[vertex_id]]) {
    RasterVertex out;
    out.position=vertices[index];out.uv=(vertices[index].xy+float2(1.0))*0.5;
    out.tint=image.sample(filtering,out.uv,level(lod));
    out.position.z=out.position.z+out.tint.r*0.125;
    return out;
}
vertex RasterVertex sampled_array_vertex(device const float4 *vertices [[buffer(0)]],
    texture2d_array<float> image [[texture(0)]],sampler filtering [[sampler(0)]],constant uint2 &sliceAndLod [[buffer(1)]],
    uint index [[vertex_id]]) {
    RasterVertex out;
    out.position=vertices[index];out.uv=(vertices[index].xy+float2(1.0))*0.5;
    out.tint=image.sample(filtering,out.uv,sliceAndLod.x,level(float(sliceAndLod.y)));
    out.position.z=out.position.z+out.tint.r*0.125;
    return out;
}

// Sparse texture and sampler slots are independent in each graphics stage.
vertex RasterVertex multitextured_vertex(device const float4 *vertices [[buffer(0)]],
    texture2d<float> color [[texture(1)]],texture2d_array<float> detail [[texture(6)]],
    sampler base [[sampler(2)]],sampler filtering [[sampler(7)]],constant uint2 &control [[buffer(1)]],uint index [[vertex_id]]) {
    RasterVertex out;out.position=vertices[index];out.uv=(out.position.xy+float2(1.0))*0.5;
    out.tint=(color.sample(base,out.uv)+color.sample(filtering,out.uv,level(float(control.y)))+
        detail.sample(filtering,out.uv,control.x,gradient2d(float2(0.125,0.0),float2(0.0,0.125))))*0.25;
    out.position.z=out.position.z+out.tint.r*0.125;
    return out;
}
fragment float4 multitextured_fragment(RasterVertex in [[stage_in]],texture2d<float> color [[texture(6)]],
    texture2d<float> detail [[texture(1)]],sampler base [[sampler(7)]],sampler filtering [[sampler(2)]],constant float &lod [[buffer(0)]]) {
    return (color.sample(base,in.uv)+color.sample(filtering,in.uv,level(lod))+
        detail.sample(base,in.uv,gradient2d(float2(0.125,0.0),float2(0.0,0.125))))*in.tint*0.25;
}

fragment float4 cube_textured(texturecube<float> image [[texture(3)]],sampler filtering [[sampler(2)]],constant float4 &direction [[buffer(0)]]) {
    return image.sample(filtering,direction.xyz,level(direction.w));
}
fragment float4 cube_array_textured(texturecube_array<float> image [[texture(6)]],sampler filtering [[sampler(7)]],
    constant float4 &direction [[buffer(0)]],constant uint &cubeIndex [[buffer(1)]]) {
    return image.sample(filtering,direction.xyz,cubeIndex,level(direction.w));
}
vertex RasterVertex cube_sampled_vertex(device const float4 *vertices [[buffer(0)]],texturecube<float> image [[texture(3)]],
    sampler filtering [[sampler(2)]],constant float4 &direction [[buffer(1)]],uint index [[vertex_id]]) {
    RasterVertex out;out.position=vertices[index];out.uv=vertices[index].xy;
    out.tint=image.sample(filtering,direction.xyz,gradientcube(float3(0.125,0.0,0.0),float3(0.0,0.125,0.0)));
    out.position.z=out.position.z+out.tint.r*0.125;return out;
}
kernel void cube_sample(texturecube_array<float> source [[texture(6)]],sampler filtering [[sampler(2)]],
    device float4 *out [[buffer(0)]],constant float4 &direction [[buffer(1)]],constant uint &cubeIndex [[buffer(2)]],uint index [[thread_position_in_grid]]) {
    out[index]=source.sample(filtering,direction.xyz,cubeIndex,level(direction.w));
}

// Face and cube element are distinct indices for direct image operations.
kernel void cube_copy(texturecube<float,access::read> source [[texture(0)]],texturecube<float,access::write> destination [[texture(7)]],
    constant uint &lod [[buffer(0)]],uint3 id [[thread_position_in_grid]]) {
    if(id.x>=destination.get_width(lod) || id.y>=destination.get_height(lod))return;
    destination.write(source.read(id.xy,id.z,lod),id.xy,id.z,lod);
}
kernel void cube_array_copy(texturecube_array<float,access::read> source [[texture(0)]],texturecube_array<float,access::write> destination [[texture(7)]],
    constant uint4 &control [[buffer(0)]],uint3 id [[thread_position_in_grid]]) {
    if(id.x>=destination.get_width(control.z) || id.y>=destination.get_height(control.z))return;
    destination.write(source.read(id.xy,id.z,control.x,control.z),id.xy,id.z,control.y,control.z);
}
kernel void cube_image_queries(texturecube<float,access::read> image [[texture(3)]],texturecube_array<float,access::read> images [[texture(6)]],device uint *out [[buffer(0)]]) {
    out[0]=image.get_width();out[1]=image.get_height(1u);out[2]=image.get_num_mip_levels();
    out[3]=images.get_width();out[4]=images.get_height(1u);out[5]=images.get_num_mip_levels();out[6]=images.get_array_size();
}
kernel void cube_image_update(texturecube_array<float,access::read_write> image [[texture(3)]],constant uint2 &control [[buffer(0)]],
    constant float4 &scale [[buffer(1)]],uint3 id [[thread_position_in_grid]]) {
    image.write(image.read(id.xy,id.z,control.x,control.y)*scale,id.xy,id.z,control.x,control.y);
}

// Graphics reads share resource slots with sampling but need no sampler.
vertex RasterVertex fetch_vertex(device const float4 *vertices [[buffer(0)]],texture2d<float,access::read> image [[texture(6)]],
    constant uint4 &control [[buffer(1)]],uint index [[vertex_id]]) {
    RasterVertex out;out.position=vertices[index];out.uv=vertices[index].xy;
    out.tint=image.read(control.xy,control.w);
    out.position.z=out.position.z+out.tint.r/float(image.get_width(control.w)+image.get_height(control.w)+image.get_num_mip_levels()+1u);
    return out;
}
fragment float4 fetch_array_fragment(texture2d_array<float,access::read> image [[texture(1)]],texture2d<float> targetInfo [[texture(7)]],
    constant uint4 &control [[buffer(0)]]) {
    return image.read(control.xy,control.z,control.w)+float4(float(targetInfo.get_width()),float(targetInfo.get_height()),
        float(image.get_array_size()),float(image.get_num_mip_levels()))/65536.0;
}
fragment float4 fetch_cube_fragment(texturecube<float,access::read> image [[texture(3)]],constant uint4 &control [[buffer(0)]]) {
    return image.read(control.xy,control.z,control.w)+float4(float(image.get_width(control.w)),float(image.get_height(control.w)),
        float(image.get_num_mip_levels()),0.0)/65536.0;
}
vertex RasterVertex fetch_cube_array_vertex(device const float4 *vertices [[buffer(0)]],texturecube_array<float> image [[texture(6)]],
    sampler filtering [[sampler(7)]],constant uint4 &control [[buffer(1)]],constant uint &lod [[buffer(2)]],
    constant float4 &direction [[buffer(3)]],uint index [[vertex_id]]) {
    RasterVertex out;out.position=vertices[index];out.uv=vertices[index].xy;
    out.tint=(image.read(control.xy,control.z,control.w,lod)+image.sample(filtering,direction.xyz,control.w,level(float(lod))))*0.5;
    out.position.z=out.position.z+out.tint.r/float(image.get_width(lod)+image.get_height(lod)+image.get_array_size()+image.get_num_mip_levels()+1u);
    return out;
}
fragment float4 texture_queries_fragment(texture2d<float> plane [[texture(0)]],texture2d_array<float> layers [[texture(1)]],
    texturecube<float> cube [[texture(3)]],texturecube_array<float> cubes [[texture(7)]],constant uint &lod [[buffer(0)]]) {
    return float4(float(plane.get_width(lod)+plane.get_height(lod)),float(layers.get_width(lod)+layers.get_array_size()),
        float(cube.get_height(lod)+cube.get_num_mip_levels()),float(cubes.get_width(lod)+cubes.get_array_size()+cubes.get_num_mip_levels()));
}

// Compiler-owned samplers need no application sampler object. The same state
// may be used by both LOD modes; native submission allocates both descriptors.
constant sampler colorFiltering(coord::normalized,filter::linear,mip_filter::linear,address::repeat,lod_clamp(0.25f,4.0f));
kernel void constant_sample(texture2d<float> image [[texture(5)]],device float4 *out [[buffer(0)]],
    constant float4 &coordinate [[buffer(1)]],uint index [[thread_position_in_grid]]) {
    constexpr sampler nearest(coord::normalized,filter::nearest,address::clamp_to_edge);
    out[index]=image.sample(nearest,coordinate.xy,level(coordinate.w));
}
fragment float4 constant_sample_fragment(texture2d<float> image [[texture(6)]],sampler external [[sampler(7)]],
    constant float4 &coordinate [[buffer(0)]]) {
    float4 color=image.sample(colorFiltering,coordinate.xy)+image.sample(colorFiltering,coordinate.xy,level(coordinate.w));
    { constexpr sampler colorFiltering(filter::nearest,address::clamp_to_zero);
      color=color+image.sample(colorFiltering,coordinate.xy,level(coordinate.w)); }
    return (color+image.sample(colorFiltering,coordinate.xy,level(coordinate.w))+image.sample(external,coordinate.xy))*0.2;
}
vertex RasterVertex constant_cube_vertex(device const float4 *vertices [[buffer(0)]],texturecube<float> image [[texture(3)]],
    constant float4 &direction [[buffer(1)]],uint index [[vertex_id]]) {
    constexpr sampler filtering(filter::linear,mip_filter::linear,address::clamp_to_edge,max_anisotropy(4));
    RasterVertex out;out.position=vertices[index];out.uv=vertices[index].xy;
    out.tint=image.sample(filtering,direction.xyz,gradientcube(float3(0.125,0.0,0.0),float3(0.0,0.125,0.0)));
    out.position.z=out.position.z+out.tint.r*0.125;return out;
}
fragment float4 constant_array_fragment(texture2d_array<float> image [[texture(1)]],constant float4 &coordinate [[buffer(0)]],constant uint &slice [[buffer(1)]]) {
    constexpr sampler filtering(filter::linear,mip_filter::nearest,s_address::mirrored_repeat,t_address::clamp_to_edge,r_address::clamp_to_edge);
    return image.sample(filtering,coordinate.xy,slice)+image.sample(filtering,coordinate.xy,slice,level(coordinate.w));
}

// Sparse color indices are attachment identities, independent of struct order.
struct FragmentColors {float4 base [[color(0)]];float4 normal [[color(3)]];float4 material [[color(7)]];};
fragment FragmentColors mrt_fragment(float4 position [[position]],constant float4 &control [[buffer(0)]]) {
    FragmentColors out;out.base=control;out.normal=float4(position.x/64.0,position.y/64.0,0.25,1.0);out.material=control.wzyx;
    if(position.x<control.x){out.material=out.material*0.5;return out;}return out;
}
struct LastColor {float4 value [[color(7)]];};
fragment LastColor color_only_fragment(texture2d<float> image [[texture(5)]],constant float4 &coordinate [[buffer(0)]]) {
    constexpr sampler filtering(filter::linear,mip_filter::linear,address::clamp_to_edge);
    LastColor out;out.value=image.sample(filtering,coordinate.xy,level(coordinate.w));return out;
}
struct AllColors {
    float4 a [[color(0)]];float4 b [[color(1)]];float4 c [[color(2)]];float4 d [[color(3)]];
    float4 e [[color(4)]];float4 f [[color(5)]];float4 g [[color(6)]];float4 h [[color(7)]];
};
fragment AllColors all_colors_fragment(constant float4 &value [[buffer(0)]]) {
    AllColors out;out.a=value;out.b=value.bgra;out.c=value.wzyx;out.d=value*0.5;
    out.e=value+float4(0.125);out.f=value*0.25;out.g=float4(1.0)-value;out.h=value+float4(0.25);return out;
}

// Replacement depth is an independent scalar export; no color target is
// required by a depth prepass. The nil-fragment pipeline uses the first entry.
fragment void raster_depth_only() {}
struct FragmentDepth {float4 color [[color(0)]];float depth [[depth(any)]];};
fragment FragmentDepth depth_fragment(float4 position [[position]],constant float4 &control [[buffer(0)]]) {
    FragmentDepth out;out.color=control;out.depth=control.w+position.x/4096.0;return out;
}
struct DepthOnly {float depth [[depth(less)]];};
fragment DepthOnly depth_only_fragment(float4 position [[position]]) {
    DepthOnly out;out.depth=position.z*0.5;return out;
}
struct DiscardDepth {float depth [[depth(any)]];float4 color [[color(7)]];};
fragment DiscardDepth discard_depth_fragment(float4 position [[position]],device atomic_uint *counter [[buffer(0)]],
    device uint *values [[buffer(1)]],texture2d<float> image [[texture(5)]]) {
    uint lane=uint(position.y)*64u+uint(position.x);
    values[lane]=11u;atomic_fetch_add_explicit(counter,1u,memory_order_relaxed);
    if(position.x<32.0)discard_fragment();
    // These derivatives still need lanes discarded above. Only surviving
    // lanes can perform the following external writes and atomic increment.
    constexpr sampler filtering(filter::linear,address::clamp_to_edge);
    float4 sampled=image.sample(filtering,position.xy/64.0);
    float slope=dfdx(position.x)+dfdy(position.y)+fwidth(position.z);
    values[lane]=22u;atomic_fetch_add_explicit(&counter[1],1u,memory_order_relaxed);
    DiscardDepth out;out.depth=position.z;out.color=sampled+float4(slope/16.0);return out;
}
fragment void discard_only_fragment(float4 position [[position]]) {
    if(position.x<32.0)discard_fragment();
}

// Resolve in linear color space. Existing image helpers perform sRGB/BGRA
// decode before accumulation and encode once at the single-sample output.
kernel void resolve_color(texture2d_ms<float,access::read> source [[texture(0)]],texture2d<float,access::write> target [[texture(7)]],
    uint2 index [[thread_position_in_grid]]) {
    float4 total=float4(0.0);uint count=source.get_num_samples();
    for(uint sample=0u;sample<count;++sample)total=total+source.read(index,sample);
    target.write(total/float(count),index);
}
fragment float4 multisample_fragment(texture2d_ms<float> source [[texture(5)]],constant uint4 &control [[buffer(0)]]) {
    return source.read(control.xy,control.z)+float4(float(source.get_num_samples()),float(source.get_width()),float(source.get_height()),0.0)/65536.0;
}
vertex RasterVertex multisample_vertex(device const float4 *vertices [[buffer(0)]],texture2d_ms<float> source [[texture(2)]],
    constant uint4 &control [[buffer(1)]],uint index [[vertex_id]]) {
    RasterVertex out;out.position=vertices[index];out.uv=vertices[index].xy;out.tint=source.read(control.xy,control.z);return out;
}
