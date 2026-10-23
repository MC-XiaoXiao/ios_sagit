// SPDX-License-Identifier: BSD-2-Clause
#include <metal_stdlib>
using namespace metal;
// Real GPU producer for the native dependency diagnostic. All half payloads
// are assembled as integer bits; the consumer performs actual half math.
kernel void constant_window_producer(device uint *output [[buffer(0)]],
    constant uint *phase [[buffer(1)]],uint index [[thread_position_in_grid]]) {
    if(index<56u) {
        uint value=0xa5a5a5a5u;
        if(index>=44u && index<54u) {
            uint first=(index-44u)*2u;
            uint a=(first/4u+first%4u+phase[0])%4u;
            uint b=(a+1u)%4u;
            uint low=a==0u?0x2800u:a==1u?0x2c00u:a==2u?0x2e00u:0x3000u;
            uint high=b==0u?0x2800u:b==1u?0x2c00u:b==2u?0x2e00u:0x3000u;
            value=low|(high<<16u);
        }
        output[index]=value;
    }
}
