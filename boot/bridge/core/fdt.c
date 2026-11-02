// SPDX-License-Identifier: BSD-2-Clause
#include "bridge.h"
#ifndef BRIDGE_MIN_PHYS
#define BRIDGE_MIN_PHYS 0xa0000000ULL
#endif
struct region {
    uint64_t base, size;
};
struct frame {
    unsigned int ac, sc, pac, psc, reserved, memory;
    const unsigned char *reg;
    uint32_t regsize;
    unsigned int fb, width, height, stride, format, disabled;
};
static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t be64(const unsigned char *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
static int eq(const unsigned char *p, const char *s, uint32_t length) {
    uint32_t i = 0;
    while (s[i] && i < length && p[i] == s[i])
        i++;
    return i < length && !s[i] && !p[i];
}
static int add(struct region *r, unsigned int *count, uint64_t base, uint64_t size) {
    if (*count >= 128 || size > ~0ULL - base)
        return -1;
    if (size)
        r[(*count)++] = (struct region){base, size};
    return 0;
}
static int add_reg(struct region *r, unsigned int *count, const struct frame *f) {
    if (!f->reg || f->pac < 1 || f->pac > 2 || f->psc < 1 || f->psc > 2)
        return -1;
    unsigned int width = 4 * (f->pac + f->psc);
    if (f->regsize % width)
        return -1;
    for (unsigned int i = 0; i < f->regsize; i += width) {
        uint64_t base = f->pac == 2 ? be64(f->reg + i) : be32(f->reg + i);
        uint64_t size = f->psc == 2 ? be64(f->reg + i + f->pac * 4) : be32(f->reg + i + f->pac * 4);
        if (add(r, count, base, size))
            return -1;
    }
    return 0;
}
// Parse only standard FDT memory metadata; never infer RAM from a phone model.
static int fdt_scan(const void *blob, uint64_t needed, uint64_t image_base, uint64_t image_end,
                    uint64_t *arena, struct boot_args *video) {
    const unsigned char *b = blob;
    if (!b || be32(b) != 0xd00dfeed)
        return -1;
    uint32_t total = be32(b + 4), off = be32(b + 8), strings = be32(b + 12), reserve = be32(b + 16),
             slen = be32(b + 32), tlen = be32(b + 36);
    if (total < 40 || total > 0x200000 || off > total || tlen > total - off || strings > total ||
        slen > total - strings || reserve > total)
        return -2;
    struct region banks[128], blocked[128];
    unsigned int nb = 0, nr = 0;
    if (add(blocked, &nr, (uintptr_t)b, total) || image_end < image_base ||
        add(blocked, &nr, image_base, image_end - image_base))
        return -3;
    int terminated = 0;
    for (uint32_t p = reserve; p <= total && total - p >= 16; p += 16) {
        uint64_t base = be64(b + p), size = be64(b + p + 8);
        if (!base && !size) {
            terminated = 1;
            break;
        }
        if (add(blocked, &nr, base, size))
            return -4;
    }
    if (!terminated)
        return -4;
    struct frame stack[32];
    int depth = -1;
    uint32_t stop = off + tlen;
    uint64_t initrd_start = 0, initrd_end = 0;
    while (off <= stop && stop - off >= 4) {
        uint32_t token = be32(b + off);
        off += 4;
        if (token == 1) {
            uint32_t start = off;
            while (off < stop && b[off])
                off++;
            if (off == stop || depth == 31)
                return -5;
            unsigned int inherited = depth >= 0 ? stack[depth].reserved : 0;
            unsigned int pac = depth >= 0 ? stack[depth].ac : 2,
                         psc = depth >= 0 ? stack[depth].sc : 1;
            stack[++depth] =
                (struct frame){.ac = 2, .sc = 1, .pac = pac, .psc = psc, .reserved = inherited};
            if (eq(b + start, "reserved-memory", off - start + 1))
                stack[depth].reserved = 1;
            off = (off + 4) & ~3U;
        } else if (token == 2) {
            if (depth < 0)
                return -6;
            struct frame *f = &stack[depth];
            if (f->fb && !f->disabled && f->reg) {
                if (add_reg(blocked, &nr, f))
                    return -8;
                if (video && f->format && f->regsize == (f->pac + f->psc) * 4) {
                    uint64_t base = f->pac == 2 ? be64(f->reg) : be32(f->reg);
                    uint64_t size =
                        f->psc == 2 ? be64(f->reg + f->pac * 4) : be32(f->reg + f->pac * 4);
                    (void)bridge_set_video(video, base, size, f->width, f->height, f->stride);
                }
            }
            if (f->memory && add_reg(banks, &nb, f))
                return -7;
            if (f->reserved && f->reg && add_reg(blocked, &nr, f))
                return -8;
            depth--;
        } else if (token == 3) {
            if (depth < 0 || stop - off < 8)
                return -9;
            uint32_t len = be32(b + off), name = be32(b + off + 4);
            off += 8;
            if (len > stop - off || name >= slen)
                return -10;
            const unsigned char *key = b + strings + name, *value = b + off;
            uint32_t kl = slen - name;
            if (eq(key, "#address-cells", kl)) {
                if (len != 4)
                    return -11;
                stack[depth].ac = be32(value);
            } else if (eq(key, "#size-cells", kl)) {
                if (len != 4)
                    return -11;
                stack[depth].sc = be32(value);
            } else if (eq(key, "device_type", kl) && eq(value, "memory", len))
                stack[depth].memory = 1;
            else if (eq(key, "compatible", kl) && eq(value, "simple-framebuffer", len))
                stack[depth].fb = 1;
            else if (eq(key, "status", kl) && eq(value, "disabled", len))
                stack[depth].disabled = 1;
            else if (eq(key, "format", kl))
                stack[depth].format = eq(value, "x8r8g8b8", len) || eq(value, "a8r8g8b8", len);
            else if (eq(key, "width", kl)) {
                if (len != 4)
                    return -11;
                stack[depth].width = be32(value);
            } else if (eq(key, "height", kl)) {
                if (len != 4)
                    return -11;
                stack[depth].height = be32(value);
            } else if (eq(key, "stride", kl)) {
                if (len != 4)
                    return -11;
                stack[depth].stride = be32(value);
            } else if (eq(key, "reg", kl)) {
                stack[depth].reg = value;
                stack[depth].regsize = len;
            } else if (eq(key, "linux,initrd-start", kl)) {
                if (len != 4 && len != 8)
                    return -12;
                initrd_start = len == 8 ? be64(value) : be32(value);
            } else if (eq(key, "linux,initrd-end", kl)) {
                if (len != 4 && len != 8)
                    return -12;
                initrd_end = len == 8 ? be64(value) : be32(value);
            }
            off = (off + len + 3) & ~3U;
        } else if (token == 9) {
            if (depth != -1)
                return -13;
            terminated = 2;
            break;
        } else if (token != 4)
            return -14;
    }
    if (terminated != 2)
        return -15;
    if (!needed)
        return video && video->video[0] ? 0 : -19;
    if (!nb || (needed & 0x1fffff))
        return -15;
    if (initrd_end &&
        (initrd_end < initrd_start || add(blocked, &nr, initrd_start, initrd_end - initrd_start)))
        return -16;
    for (unsigned int i = 0; i < nb; i++) {
        uint64_t start = banks[i].base > BRIDGE_MIN_PHYS ? banks[i].base : BRIDGE_MIN_PHYS;
        if (start > ~0ULL - 0x1fffff)
            continue;
        start = (start + 0x1fffff) & ~0x1fffffULL;
        uint64_t end = banks[i].base + banks[i].size;
        while (start <= end && needed <= end - start) {
            uint64_t next = start;
            for (unsigned int j = 0; j < nr; j++)
                if (start < blocked[j].base + blocked[j].size && blocked[j].base < start + needed) {
                    uint64_t e = blocked[j].base + blocked[j].size;
                    if (e > ~0ULL - 0x1fffff)
                        return -17;
                    e = (e + 0x1fffff) & ~0x1fffffULL;
                    if (e > next)
                        next = e;
                }
            if (next == start) {
                *arena = start;
                return 0;
            }
            start = next;
        }
    }
    return -18;
}

int fdt_find_arena(const void *blob, uint64_t needed, uint64_t image_base, uint64_t image_end,
                   uint64_t *arena) {
    if (!needed || !arena)
        return -1;
    return fdt_scan(blob, needed, image_base, image_end, arena, 0);
}
int fdt_get_video(const void *blob, struct boot_args *ba) { return fdt_scan(blob, 0, 0, 0, 0, ba); }
