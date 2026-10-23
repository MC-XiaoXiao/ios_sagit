// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <vector>

@protocol QcomGMEMDiagnostics
- (BOOL)qcomSetGMEMEnabled:(BOOL)enabled;
- (uint64_t)qcomGMEMPasses;
@end

static int gmemTest(id<MTLDevice> device) {
    auto control=(id<QcomGMEMDiagnostics>)device;
    if(![device respondsToSelector:@selector(qcomSetGMEMEnabled:)] || ![control qcomSetGMEMEnabled:NO])return 1;
    NSError *error=nil;
    NSString *source=@R"SHADER(
struct TileVertex {float4 position [[position]];};
vertex TileVertex tile_vertex(device const float4 *points [[buffer(0)]],uint i [[vertex_id]]) {
 TileVertex o;o.position=points[i];return o;
}
struct TileOutput {float4 value [[color(1)]];};
fragment TileOutput tile_producer(float4 p [[position]]) {
 TileOutput o;o.value=float4(float(uint(p.x)%8u)*0.125f,float(uint(p.y)%8u)*0.125f,0.5f,1.0f);return o;
}
fragment float4 tile_consumer(float4 p [[position]],float4 previous [[color(1)]]) {
 return float4(previous.x,previous.y,float(uint(p.x+p.y)%8u)*0.125f,previous.w);
}
)SHADER";
    auto library=[device newLibraryWithSource:source options:nil error:&error];
    auto queue=[device newCommandQueue];
    const NSUInteger width=1080,height=1920,offset=512;
    const NSUInteger pitches[]={4352,8704},sizes[]={offset+pitches[0]*height+512,offset+pitches[1]*height+512};
    const MTLPixelFormat formats[]={MTLPixelFormatBGRA8Unorm,MTLPixelFormatRGBA16Float};
    id<MTLBuffer> backing[2];id<MTLTexture> textures[2];
    for(unsigned i=0;i<2;++i) {
        backing[i]=[device newBufferWithLength:sizes[i] options:MTLResourceStorageModeShared];
        auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:formats[i] width:width height:height mipmapped:NO];
        td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
        textures[i]=[backing[i] newTextureWithDescriptor:td offset:offset bytesPerRow:pitches[i]];
        if(!textures[i] || !backing[i].contents)return 1;
    }
    auto pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[library newFunctionWithName:@"tile_vertex"];
    pd.colorAttachments[0].pixelFormat=formats[0];pd.colorAttachments[1].pixelFormat=formats[1];
    pd.fragmentFunction=[library newFunctionWithName:@"tile_producer"];
    auto producer=[device newRenderPipelineStateWithDescriptor:pd error:&error];
    pd.fragmentFunction=[library newFunctionWithName:@"tile_consumer"];
    auto consumer=[device newRenderPipelineStateWithDescriptor:pd error:&error];
    const float points[]={-1,-1,0,1, 3,-1,0,1, -1,3,0,1};
    auto vertex=[device newBufferWithBytes:points length:sizeof(points) options:MTLResourceStorageModeShared];
    if(!library || !queue || !producer || !consumer || !vertex) {
        fprintf(stderr,"GMEM setup: %s\n",error.localizedDescription.UTF8String);return 1;
    }
    for(unsigned phase=0;phase<3;++phase) {
        std::vector<uint8_t> reference[2];
        for(unsigned tile=0;tile<2;++tile) {
            if(![control qcomSetGMEMEnabled:tile])return 1;
            for(unsigned i=0;i<2;++i)memset(backing[i].contents,0xa5,sizes[i]);
            for(NSUInteger y=0;y<height;++y)for(NSUInteger x=0;x<width;++x) {
                const __fp16 pixel[]={__fp16(float((x/8)%8)*0.125f),__fp16(float((y/8)%8)*0.125f),__fp16(0.5f),__fp16(1)};
                memcpy(static_cast<uint8_t *>(backing[1].contents)+offset+y*pitches[1]+x*8,pixel,8);
            }
            auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
            for(unsigned i=0;i<2;++i) {
                pass.colorAttachments[i].texture=textures[i];
                pass.colorAttachments[i].loadAction=phase || !i?MTLLoadActionClear:MTLLoadActionLoad;
                pass.colorAttachments[i].storeAction=phase==2 && i?MTLStoreActionDontCare:MTLStoreActionStore;
                pass.colorAttachments[i].clearColor=i?MTLClearColorMake(0.25,0.75,0.5,1):MTLClearColorMake(0.125,0.375,0.625,0.875);
            }
            const auto before=[control qcomGMEMPasses];
            auto commands=[queue commandBuffer];auto render=[commands renderCommandEncoderWithDescriptor:pass];
            if(!render)return 1;
            [render setVertexBuffer:vertex offset:0 atIndex:0];
            if(phase==2) {
                [render setRenderPipelineState:producer];
                [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            [render setRenderPipelineState:consumer];
            [render setScissorRect:(MTLScissorRect){7,3,width-21,height-6}];
            [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[render endEncoding];
            printf("GMEM_TEST phase=%u tiled=%u begin\n",phase,tile);fflush(stdout);
            if(!completed(commands))return 1;
            if([control qcomGMEMPasses]-before!=tile){fputs("GMEM pass was not admitted as requested\n",stderr);return 1;}
            for(unsigned i=0;i<(phase==2?1u:2u);++i) {
                const auto *actual=static_cast<const uint8_t *>(backing[i].contents);
                if(!tile)reference[i].assign(actual,actual+sizes[i]);
                else {
                    unsigned pixels=0,padding=0;
                    const NSUInteger cpp=i?8:4;
                    for(NSUInteger b=0;b<sizes[i];++b)if(actual[b]!=reference[i][b]) {
                        const bool visible=b>=offset && b<offset+pitches[i]*height && (b-offset)%pitches[i]<width*cpp;
                        if(pixels+padding<8)fprintf(stderr,"GMEM mismatch phase=%u plane=%u byte=%lu tiled=%u bypass=%u visible=%u\n",phase,i,(unsigned long)b,actual[b],reference[i][b],visible);
                        if(visible)++pixels;else ++padding;
                    }
                    if(pixels || padding){fprintf(stderr,"GMEM difference plane=%u visible_bytes=%u padding_bytes=%u\n",i,pixels,padding);return 1;}
                }
            }
            printf("GMEM_TEST phase=%u tiled=%u completed=1\n",phase,tile);fflush(stdout);
        }
    }
    [control qcomSetGMEMEnabled:NO];
    puts("GMEM_TEST passed: 1080x1920 horizontal/vertical tiles, clear/load, BGRA store, readonly RGBA16F fetch, producer draw, global coordinates and guards match BYPASS");return 0;
}
