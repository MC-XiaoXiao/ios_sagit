// SPDX-License-Identifier: BSD-2-Clause
// Native clear-path comparison, including storage guards and fallback cases.
#pragma once
#include <array>
#include <vector>

@protocol QcomRenderClearDiagnostics
- (BOOL)qcomRenderClearEnabled;
- (BOOL)qcomSetRenderClearEnabled:(BOOL)enabled;
@end

namespace RenderClearDiagnostic {
struct Format {MTLPixelFormat metal;unsigned bytes;const char *name;};
static constexpr Format formats[]={
    {MTLPixelFormatRGBA8Unorm,4,"rgba8"},{MTLPixelFormatBGRA8Unorm,4,"bgra8"},
    {MTLPixelFormatRGBA8Unorm_sRGB,4,"rgba8srgb"},{MTLPixelFormatBGRA8Unorm_sRGB,4,"bgra8srgb"},
    {MTLPixelFormatR8Unorm,1,"r8"},{MTLPixelFormatRG8Unorm,2,"rg8"},
    {MTLPixelFormatRGBA16Float,8,"rgba16f"},{MTLPixelFormatRG32Float,8,"rg32f"}};
static std::array<uint8_t,8> pixel(Format format,MTLClearColor color) {
    double c[]={color.red,color.green,color.blue,color.alpha};std::array<uint8_t,8> result{};
    if(format.metal==MTLPixelFormatRG32Float) {
        const float rg[]={float(c[0]),float(c[1])};memcpy(result.data(),rg,8);return result;
    }
    if(format.metal==MTLPixelFormatRGBA16Float) {
        const __fp16 rgba[]={__fp16(c[0]),__fp16(c[1]),__fp16(c[2]),__fp16(c[3])};memcpy(result.data(),rgba,8);return result;
    }
    for(unsigned i=0;i<4;++i) {
        double value=std::max(0.0,std::min(1.0,c[i]));
        if(i<3 && (format.metal==MTLPixelFormatRGBA8Unorm_sRGB || format.metal==MTLPixelFormatBGRA8Unorm_sRGB))
            value=value<=0.0031308?value*12.92:1.055*std::pow(value,1.0/2.4)-0.055;
        result[i]=uint8_t(value*255+0.5);
    }
    if(format.metal==MTLPixelFormatBGRA8Unorm || format.metal==MTLPixelFormatBGRA8Unorm_sRGB)std::swap(result[0],result[2]);
    return result;
}
static bool equal(const uint8_t *actual,const std::vector<uint8_t> &expected,const char *phase) {
    for(size_t i=0;i<expected.size();++i)if(actual[i]!=expected[i]) {
        fprintf(stderr,"RENDER_CLEAR mismatch phase=%s offset=%lu actual=%u expected=%u\n",phase,(unsigned long)i,actual[i],expected[i]);return false;
    }
    return true;
}
static bool linear(id<MTLDevice> device,id<MTLCommandQueue> queue,Format format,NSUInteger width,NSUInteger height,
                   MTLClearColor color,bool partial,unsigned samples,bool unretained,unsigned &tests,uint64_t &checked) {
    const NSUInteger offset=256,pitch=(width*format.bytes+255)&~NSUInteger(255),total=offset+pitch*height+256;
    const NSUInteger clearWidth=partial?width-3:width,clearHeight=partial?height-2:height;
    auto storage=[device newBufferWithLength:total options:MTLResourceStorageModeShared];
    auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format.metal width:width height:height mipmapped:NO];
    d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
    auto texture=[storage newTextureWithDescriptor:d offset:offset bytesPerRow:pitch];
    id<MTLTexture> msaa=nil;
    if(samples>1) {
        d.storageMode=MTLStorageModePrivate;d.textureType=MTLTextureType2DMultisample;d.sampleCount=samples;
        msaa=[device newTextureWithDescriptor:d];
    }
    if(!storage.contents || !texture || (samples>1 && !msaa))return false;
    auto expected=std::vector<uint8_t>(total,0xa5);auto value=pixel(format,color);
    for(NSUInteger y=0;y<clearHeight;++y)for(NSUInteger x=0;x<clearWidth;++x)
        memcpy(expected.data()+offset+y*pitch+x*format.bytes,value.data(),format.bytes);
    std::vector<uint8_t> baseline;double elapsed[2]={};
    for(unsigned route=0;route<2;++route) {
        if(![(id<QcomRenderClearDiagnostics>)device qcomSetRenderClearEnabled:route])return false;
        memset(storage.contents,0xa5,total);
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=msaa?:texture;pass.colorAttachments[0].loadAction=MTLLoadActionClear;
        pass.colorAttachments[0].storeAction=msaa?MTLStoreActionMultisampleResolve:MTLStoreActionStore;
        pass.colorAttachments[0].resolveTexture=msaa?texture:nil;pass.colorAttachments[0].clearColor=color;
        pass.renderTargetWidth=clearWidth;pass.renderTargetHeight=clearHeight;
        auto cb=unretained?[queue commandBufferWithUnretainedReferences]:[queue commandBuffer];
        const auto begin=CACurrentMediaTime();
        @autoreleasepool {auto encoder=[cb renderCommandEncoderWithDescriptor:pass];if(!encoder)return false;[encoder endEncoding];}
        if(!completed(cb))return false;elapsed[route]=(CACurrentMediaTime()-begin)*1000;
        const auto *actual=static_cast<const uint8_t *>(storage.contents);
        if(!equal(actual,expected,route?"render":"compute"))return false;
        if(!route)baseline.assign(actual,actual+total);
        else if(!equal(actual,baseline,"route-parity"))return false;
        checked+=total;++tests;
    }
    printf("RENDER_CLEAR format=%s size=%lux%lu partial=%u samples=%u unretained=%u compute_ms=%.3f render_ms=%.3f checked_bytes=%lu guards=exact parity=exact\n",
        format.name,(unsigned long)width,(unsigned long)height,partial,samples,unretained,elapsed[0],elapsed[1],(unsigned long)(2*total));fflush(stdout);return true;
}
static bool subresources(id<MTLDevice> device,id<MTLCommandQueue> queue,unsigned &tests,uint64_t &checked) {
    const Format format=formats[7];const auto color=MTLClearColorMake(0,1,0,1);const auto value=pixel(format,color);
    auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format.metal width:34 height:18 mipmapped:YES];
    d.textureType=MTLTextureType2DArray;d.arrayLength=2;d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageRenderTarget;
    auto texture=[device newTextureWithDescriptor:d];if(!texture)return false;
    for(unsigned route=0;route<2;++route) {
        if(![(id<QcomRenderClearDiagnostics>)device qcomSetRenderClearEnabled:route])return false;
        for(NSUInteger level=0;level<d.mipmapLevelCount;++level)for(NSUInteger slice=0;slice<2;++slice) {
            NSUInteger width=std::max(NSUInteger(1),d.width>>level),height=std::max(NSUInteger(1),d.height>>level);
            std::vector<uint8_t> fill(width*height*8,0xa5);
            [texture replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:level slice:slice withBytes:fill.data() bytesPerRow:width*8 bytesPerImage:fill.size()];
        }
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];auto a=pass.colorAttachments[3];
        a.texture=texture;a.level=1;a.slice=1;a.loadAction=MTLLoadActionClear;a.storeAction=MTLStoreActionStore;a.clearColor=color;
        pass.renderTargetWidth=14;pass.renderTargetHeight=7;
        auto cb=[queue commandBufferWithUnretainedReferences];
        @autoreleasepool {auto encoder=[cb renderCommandEncoderWithDescriptor:pass];if(!encoder)return false;[encoder endEncoding];}
        if(!completed(cb))return false;
        for(NSUInteger level=0;level<d.mipmapLevelCount;++level)for(NSUInteger slice=0;slice<2;++slice) {
            NSUInteger width=std::max(NSUInteger(1),d.width>>level),height=std::max(NSUInteger(1),d.height>>level);
            std::vector<uint8_t> expected(width*height*8,0xa5),actual(expected.size());
            if(level==1 && slice==1)for(NSUInteger y=0;y<7;++y)for(NSUInteger x=0;x<14;++x)memcpy(expected.data()+(y*width+x)*8,value.data(),8);
            [texture getBytes:actual.data() bytesPerRow:width*8 bytesPerImage:actual.size() fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:level slice:slice];
            if(!equal(actual.data(),expected,"mip-array-slot3"))return false;checked+=actual.size();
        }
        ++tests;
    }
    puts("RENDER_CLEAR mip=1 slice=1 slot=3 partial=14x7 untouched_subresources=exact");return true;
}
}
static int renderClearTest(id<MTLDevice> device) {
    if(![device respondsToSelector:@selector(qcomSetRenderClearEnabled:)] ||
        ![(id<QcomRenderClearDiagnostics>)device qcomSetRenderClearEnabled:NO])return 1;
    struct Restore {id<MTLDevice> device;~Restore(){[(id<QcomRenderClearDiagnostics>)device qcomSetRenderClearEnabled:YES];}} restore{device};
    auto queue=[device newCommandQueue];if(!queue)return 1;unsigned tests=0;uint64_t checked=0;
    using namespace RenderClearDiagnostic;
    const MTLClearColor colors[]={MTLClearColorMake(0,0,0,1),MTLClearColorMake(1,0,1,0),MTLClearColorMake(0,1,1,1),
        MTLClearColorMake(1,1,1,1),MTLClearColorMake(0.25,0.5,0.75,1),MTLClearColorMake(-0.0,1,0,1)};
    for(auto format:formats)for(unsigned c=0;c<6;++c)for(unsigned partial=0;partial<2;++partial)
        @autoreleasepool {if(!linear(device,queue,format,17,9,colors[c],partial,1,c&1,tests,checked))return 1;}
    for(unsigned f:{1u,6u,7u})for(unsigned partial=0;partial<2;++partial)
        @autoreleasepool {if(!linear(device,queue,formats[f],1082,67,colors[2],partial,1,true,tests,checked))return 1;}
    for(unsigned f:{1u,7u})for(unsigned c=0;c<2;++c)
        @autoreleasepool {if(!linear(device,queue,formats[f],1080,1920,colors[c?2:0],false,1,false,tests,checked))return 1;}
    for(unsigned samples:{2u,4u})for(unsigned f:{1u,6u,7u})
        @autoreleasepool {if(!linear(device,queue,formats[f],17,9,colors[2],false,samples,false,tests,checked))return 1;}
    if(!subresources(device,queue,tests,checked))return 1;
    printf("RENDER_CLEAR_PASS tests=%u checked_bytes=%llu compute_render_parity=exact fallback_fraction_signed_zero_msaa=exact\n",tests,(unsigned long long)checked);return 0;
}
