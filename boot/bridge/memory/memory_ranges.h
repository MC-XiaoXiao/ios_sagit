// SPDX-License-Identifier: BSD-2-Clause
#ifndef BRIDGE_MEMORY_RANGES_H
#define BRIDGE_MEMORY_RANGES_H
#include <stdint.h>
#define BOOT_MEMORY_MAX_RANGES 256
struct boot_memory_range { uint64_t start, end; };
/* Keep only complete 16 KiB VM pages; firmware descriptors use 4 KiB pages. */
static inline int boot_memory_add(struct boot_memory_range *ranges, unsigned *count,
                           uint64_t start, uint64_t end) {
    start = (start + 0x3fff) & ~0x3fffULL;
    end &= ~0x3fffULL;
    if (start >= end) return 0;
    unsigned first = 0;
    while (first < *count && ranges[first].end < start) first++;
    unsigned last = first;
    while (last < *count && ranges[last].start <= end) {
        if (ranges[last].start < start) start = ranges[last].start;
        if (ranges[last].end > end) end = ranges[last].end;
        last++;
    }
    unsigned remaining = *count - (last - first);
    if (remaining == BOOT_MEMORY_MAX_RANGES) return -1;
    if (last == first) {
        for (unsigned i = *count; i > first; i--) ranges[i] = ranges[i - 1];
    } else {
        for (unsigned i = last; i < *count; i++) ranges[first + 1 + i - last] = ranges[i];
    }
    struct boot_memory_range merged = {start, end};
    ranges[first] = merged;
    *count = remaining + 1;
    return 0;
}
/* Exclude every complete CPU page that contains retained DMA/page-table data. */
static inline int boot_memory_remove(struct boot_memory_range *ranges, unsigned *count,
                                     uint64_t address, uint64_t bytes) {
    if (!bytes || address > UINT64_MAX - bytes || address + bytes > UINT64_MAX - 0x3fff) return -1;
    const uint64_t start = address & ~0x3fffULL, end = (address + bytes + 0x3fff) & ~0x3fffULL;
    for (unsigned n = 0; n < *count;) {
        struct boot_memory_range old = ranges[n];
        if (start >= old.end || end <= old.start) { n++; continue; }
        if (start <= old.start && end >= old.end) {
            for (unsigned j = n + 1; j < *count; j++) ranges[j - 1] = ranges[j];
            --*count;
        } else if (start <= old.start) { ranges[n++].start = end; }
        else if (end >= old.end) { ranges[n++].end = start; }
        else {
            if (*count == BOOT_MEMORY_MAX_RANGES) return -1;
            for (unsigned j = *count; j > n + 1; j--) ranges[j] = ranges[j - 1];
            ranges[n].end = start;
            ranges[n + 1].start = end; ranges[n + 1].end = old.end;
            ++*count; n += 2;
        }
    }
    return 0;
}
#endif
