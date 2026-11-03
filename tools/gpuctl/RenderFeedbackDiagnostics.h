// SPDX-License-Identifier: BSD-2-Clause
#pragma once

static int renderFeedbackTest(id<MTLDevice> device) {
    NSError *error=nil;
    NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 feedback_vertex(uint id [[vertex_id]]) { return float4(float2((id<<1)&2,id&2)*2.0-1.0,0.0,1.0); }\n"
        "fragment float4 feedback_fragment(float4 p [[position]], texture2d<float,access::read> t [[texture(0)]]) { return float4(1.0-t.read(uint2(p.xy)).rgb,1.0); }\n";
    auto library=[device newLibraryWithSource:source options:nil error:&error];
    auto pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[library newFunctionWithName:@"feedback_vertex"];
    pd.fragmentFunction=[library newFunctionWithName:@"feedback_fragment"];pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    __attribute__((objc_precise_lifetime)) id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];
    auto queue=[device newCommandQueue];
    if(!pipeline || !queue){fprintf(stderr,"Feedback pipeline: %s\n",error.description.UTF8String);return 1;}
    unsigned cases=0;
    for(unsigned unretained=0;unretained<2;++unretained)for(unsigned view=0;view<2;++view)
    for(unsigned draws=1;draws<=2;++draws) @autoreleasepool {
        constexpr NSUInteger width=32,height=16,pitch=256,offset=256;
        auto backing=[device newBufferWithLength:offset+pitch*height+256 options:MTLResourceStorageModeShared];
        if(!backing.contents)return 1;memset(backing.contents,0xa5,backing.length);
        auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
        td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;
        __attribute__((objc_precise_lifetime)) id<MTLTexture> target=[backing newTextureWithDescriptor:td offset:offset bytesPerRow:pitch];
        __attribute__((objc_precise_lifetime)) id<MTLTexture> sampled=view?[target newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm]:target;
        if(!target || !sampled)return 1;
        auto cb=unretained?[queue commandBufferWithUnretainedReferences]:[queue commandBuffer];
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=target;
        pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.colorAttachments[0].clearColor=MTLClearColorMake(0,1,0,1);
        @autoreleasepool {
            auto encoder=[cb renderCommandEncoderWithDescriptor:pass];if(!encoder)return 1;
            [encoder setRenderPipelineState:pipeline];[encoder setFragmentTexture:sampled atIndex:0];
            for(unsigned i=0;i<draws;++i) {
                [encoder memoryBarrierWithScope:MTLBarrierScopeTextures afterStages:MTLRenderStageFragment beforeStages:MTLRenderStageFragment];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            [encoder endEncoding];
        }
        if(!completed(cb))return 1;
        auto bytes=static_cast<const uint8_t *>(backing.contents);unsigned bad=0;
        for(NSUInteger at=0;at<backing.length;++at) {
            const bool pixel=at>=offset && at<offset+pitch*height && (at-offset)%pitch<width*4;
            uint8_t expected=0xa5;
            if(pixel){const unsigned channel=(at-offset)%4;expected=channel==3?255:(channel==1?!(draws&1):(draws&1))?255:0;}
            bad+=bytes[at]!=expected;
        }
        printf("RENDER_FEEDBACK unretained=%u view=%u draws=%u bad_bytes=%u guards=exact\n",unretained,view,draws,bad);
        if(bad)return 1;++cases;
    }
    printf("RENDER_FEEDBACK cases=%u passed=1\n",cases);return 0;
}
