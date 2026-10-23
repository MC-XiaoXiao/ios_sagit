// SPDX-License-Identifier: BSD-2-Clause
#include <metal_stdlib>
using namespace metal;

// One invocation owns one destination word. The width/stride layout keeps
// linear texture row padding untouched, including IOSurface imports.
kernel void blit_copy_words(device const uint *source [[buffer(0)]],
                            device uint *destination [[buffer(1)]],
                            constant uint4 &layout [[buffer(2)]],
                            uint i [[thread_position_in_grid]]) {
    if (i >= layout.x * layout.y) return;
    uint row = i / layout.x, column = i % layout.x;
    destination[row * layout.w + column] = source[row * layout.z + column];
}

kernel void blit_fill_words(device uint *destination [[buffer(0)]],
                            constant uint4 &layout [[buffer(1)]],
                            uint i [[thread_position_in_grid]]) {
    if (i >= layout.x) return;
    destination[i] = layout.w;
}

struct BlitByteParameters {
    uint width;
    uint rows;
    uint sourceStride;
    uint destinationStride;
    uint sourceBias;
    uint destinationBias;
    uint pattern;
    uint reserved;
};

kernel void blit_copy_bytes(device const uint *source [[buffer(0)]],
                            device uint *destination [[buffer(1)]],
                            constant BlitByteParameters &p [[buffer(2)]],
                            uint i [[thread_position_in_grid]]) {
    uint word = i * 4u;
    if (word >= p.destinationBias) {
        uint position = word - p.destinationBias;
        uint row = position / p.destinationStride, column = position % p.destinationStride;
        if (row < p.rows && column + 4u <= p.width) {
            uint offset = p.sourceBias + row * p.sourceStride + column;
            uint shift = (offset & 3u) * 8u;
            uint value = source[offset / 4u] >> shift;
            if (shift != 0u) value |= source[offset / 4u + 1u] << (32u - shift);
            destination[i] = value;
            return;
        }
    }
    uint mask = 0u, value = 0u;
    // A word can span several narrow rows. Assigning words globally avoids
    // competing byte stores when two rows share the same destination word.
    for (uint lane = 0u; lane < 4u; ++lane) {
        uint position = word + lane;
        if (position >= p.destinationBias) {
            position -= p.destinationBias;
            uint row = position / p.destinationStride, column = position % p.destinationStride;
            if (row < p.rows && column < p.width) {
                uint offset = p.sourceBias + row * p.sourceStride + column;
                uint byteValue = (source[offset / 4u] >> ((offset & 3u) * 8u)) & 255u;
                mask |= 255u << (lane * 8u);
                value |= byteValue << (lane * 8u);
            }
        }
    }
    if (mask == 0xffffffffu) destination[i] = value;
    else if (mask != 0u) destination[i] = (destination[i] & ~mask) | value;
}

kernel void blit_fill_bytes(device uint *destination [[buffer(0)]],
                            constant BlitByteParameters &p [[buffer(1)]],
                            uint i [[thread_position_in_grid]]) {
    uint word = i * 4u, end = p.destinationBias + p.width;
    uint first = word < p.destinationBias ? p.destinationBias - word : 0u;
    uint last = end > word ? min(4u, end - word) : 0u;
    if (first >= last) return;
    uint mask = (0xffffffffu << (first * 8u)) & (0xffffffffu >> ((4u - last) * 8u));
    if (mask == 0xffffffffu) destination[i] = p.pattern;
    else destination[i] = (destination[i] & ~mask) | (p.pattern & mask);
}

struct BlitRowParameters {
    uint width;
    uint rows;
    uint stride;
    uint bias;
    uint pattern;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};
struct BlitPairRowParameters {
    uint width;
    uint rows;
    uint stride;
    uint low;
    uint high;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};
kernel void blit_fill_pairs(device uint *destination [[buffer(0)]],
                           constant BlitPairRowParameters &p [[buffer(1)]],
                           uint i [[thread_position_in_grid]]) {
    uint words=p.width/4u;
    uint row=i/words,column=i%words;
    if(row>=p.rows)return;
    destination[row*(p.stride/4u)+column]=(column&1u)!=0u?p.high:p.low;
}

kernel void blit_fill_rows(device uint *destination [[buffer(0)]],
                           constant BlitRowParameters &p [[buffer(1)]],
                           uint i [[thread_position_in_grid]]) {
    // Linear textures have aligned row starts. A thread owns one active
    // word of one row, so row padding does not consume fill invocations.
    if (p.bias == 0u && (p.rows == 1u || (p.stride & 3u) == 0u)) {
        uint words = (p.width + 3u) / 4u;
        uint row = i / words, column = i % words;
        if (row >= p.rows) return;
        uint count = min(4u, p.width - column * 4u);
        uint mask = 0xffffffffu >> ((4u - count) * 8u);
        uint index = row * (p.stride / 4u) + column;
        if (mask == 0xffffffffu) destination[index] = p.pattern;
        else destination[index] = (destination[index] & ~mask) | (p.pattern & mask);
        return;
    }
    // Nonaligned layouts can place two rows in the same word. Assign each
    // destination word globally and merge its lanes exactly once.
    uint word = i * 4u, mask = 0u, value = 0u;
    for (uint lane = 0u; lane < 4u; ++lane) {
        uint position = word + lane;
        if (position >= p.bias) {
            position -= p.bias;
            uint row = position / p.stride, column = position % p.stride;
            if (row < p.rows && column < p.width) {
                uint byteValue = (p.pattern >> ((column & 3u) * 8u)) & 255u;
                mask |= 255u << (lane * 8u);
                value |= byteValue << (lane * 8u);
            }
        }
    }
    if (mask == 0xffffffffu) destination[i] = value;
    else if (mask != 0u) destination[i] = (destination[i] & ~mask) | value;
}

// Native admission proves 16-byte binding/width/stride alignment, freezes
// the canonical layout and checks this exact 2D dispatch. No row division,
// masked read-modify-write or per-word pattern selection is necessary here.
kernel void blit_fill_rows_2d(device uint4 *destination [[buffer(0)]],
                              constant BlitRowParameters &p [[buffer(1)]],
                              uint2 position [[thread_position_in_grid]]) {
    if (position.x >= p.width / 16u || position.y >= p.rows) return;
    destination[position.y * (p.stride / 16u) + position.x] = uint4(p.pattern);
}
kernel void blit_fill_pairs_2d(device uint4 *destination [[buffer(0)]],
                               constant BlitPairRowParameters &p [[buffer(1)]],
                               uint2 position [[thread_position_in_grid]]) {
    if (position.x >= p.width / 16u || position.y >= p.rows) return;
    destination[position.y * (p.stride / 16u) + position.x] = uint4(p.low,p.high,p.low,p.high);
}

// Common compositor attachments include a two-pixel border (1082 pixels).
// Keep their final one to three words separate from the vector store; neither
// a padding write nor a read-modify-write of an adjacent texel is necessary.
kernel void blit_fill_rows_2d_tail(device uint *destination [[buffer(0)]],
                                  constant BlitRowParameters &p [[buffer(1)]],
                                  uint2 position [[thread_position_in_grid]]) {
    if (position.y >= p.rows) return;
    uint column = position.x * 4u, base = position.y * (p.stride / 4u) + column;
    if (column * 4u < p.width) destination[base] = p.pattern;
    if ((column + 1u) * 4u < p.width) destination[base + 1u] = p.pattern;
    if ((column + 2u) * 4u < p.width) destination[base + 2u] = p.pattern;
    if ((column + 3u) * 4u < p.width) destination[base + 3u] = p.pattern;
}
kernel void blit_fill_pairs_2d_tail(device uint *destination [[buffer(0)]],
                                   constant BlitPairRowParameters &p [[buffer(1)]],
                                   uint2 position [[thread_position_in_grid]]) {
    if (position.y >= p.rows) return;
    uint column = position.x * 4u, base = position.y * (p.stride / 4u) + column;
    if (column * 4u < p.width) destination[base] = p.low;
    if ((column + 1u) * 4u < p.width) destination[base + 1u] = p.high;
    if ((column + 2u) * 4u < p.width) destination[base + 2u] = p.low;
    if ((column + 3u) * 4u < p.width) destination[base + 3u] = p.high;
}
