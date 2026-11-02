// SPDX-License-Identifier: BSD-2-Clause
#include "bridge.h"
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *a = d;
    const unsigned char *b = s;
    while (n--)
        *a++ = *b++;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *a = d;
    const unsigned char *b = s;
    if ((uintptr_t)a > (uintptr_t)b && (uintptr_t)a - (uintptr_t)b < n) {
        while (n) { --n; a[n] = b[n]; }
    } else {
        while (n--) *a++ = *b++;
    }
    return d;
}
void *memset(void *d, int c, size_t n) {
    unsigned char *a = d;
    while (n--)
        *a++ = (unsigned char)c;
    return d;
}
static int eq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}
static uint32_t u32(const unsigned char *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint64_t u64(const unsigned char *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
int macho_info(const void *blob, size_t size, struct kernel_info *k) {
    const unsigned char *b = blob;
    size_t off = 32, stop;
    uint64_t low = ~0ULL, high = 0, pc = 0;
    if (size < 32 || u32(b) != 0xfeedfacf || u32(b + 4) != 0x100000c || u32(b + 12) != 2)
        return -1;
    if (u32(b + 20) > size - 32 || u32(b + 16) > 4096)
        return -2;
    stop = 32 + u32(b + 20);
    for (uint32_t i = 0; i < u32(b + 16); i++) {
        if (off > stop || stop - off < 8)
            return -3;
        uint32_t cmd = u32(b + off), len = u32(b + off + 4);
        if (len < 8 || (len & 7) || len > stop - off)
            return -4;
        if (cmd == 0x19) {
            if (len < 72)
                return -5;
            uint64_t va = u64(b + off + 24), vs = u64(b + off + 32), fo = u64(b + off + 40),
                     fs = u64(b + off + 48);
            if (fs > vs || fo > size || fs > size - fo || vs > ~0ULL - va)
                return -6;
            if (vs) {
                if (va < low)
                    low = va;
                if (va + vs > high)
                    high = va + vs;
            }
        } else if (cmd == 5) {
            if (len < 288 || u32(b + off + 8) != 6 || u32(b + off + 12) != 68)
                return -7;
            pc = u64(b + off + 272);
        }
        off += len;
    }
    if (off != stop || low == ~0ULL || high <= low || pc < low || pc >= high)
        return -8;
    // Match the linked virtual offset to a 2 MiB aligned physical arena.
    k->virt = low & ~0x1fffffULL;
    k->end = high;
    k->entry = pc;
    if (high - k->virt > 0x08000000)
        return -9;
    // Reject overlaps and an entry outside executable, file-backed data.
    int executable = 0;
    off = 32;
    for (uint32_t i = 0; i < u32(b + 16); i++) {
        uint32_t len = u32(b + off + 4);
        if (u32(b + off) == 0x19 && u64(b + off + 32)) {
            uint64_t va = u64(b + off + 24), vs = u64(b + off + 32), fs = u64(b + off + 48);
            if (pc >= va && pc - va < fs && (u32(b + off + 60) & 4))
                executable = 1;
            size_t other = off + len;
            for (uint32_t j = i + 1; j < u32(b + 16); j++) {
                if (u32(b + other) == 0x19 && u64(b + other + 32)) {
                    uint64_t ov = u64(b + other + 24), os = u64(b + other + 32);
                    if (va < ov + os && ov < va + vs)
                        return -10;
                }
                other += u32(b + other + 4);
            }
        }
        off += len;
    }
    return executable ? 0 : -11;
}
int macho_load(const void *blob, size_t size, const struct kernel_info *k, void *arena,
               size_t capacity) {
    struct kernel_info checked;
    if (macho_info(blob, size, &checked) || checked.virt != k->virt || checked.end != k->end ||
        checked.entry != k->entry || k->end - k->virt > capacity)
        return -1;
    const unsigned char *b = blob;
    size_t off = 32;
    for (uint32_t i = 0; i < u32(b + 16); i++) {
        if (u32(b + off) == 0x19 && u64(b + off + 32)) {
            uint64_t va = u64(b + off + 24), vs = u64(b + off + 32), fo = u64(b + off + 40),
                     fs = u64(b + off + 48);
            void *d = (unsigned char *)arena + (va - k->virt);
            memset(d, 0, vs);
            memcpy(d, b + fo, fs);
        }
        off += u32(b + off + 4);
    }
    return 0;
}
// This parser only walks our build-validated Apple DT, never untrusted FDT.
static unsigned char *walk(unsigned char *p, const char *wanted, const char *prop, void **result,
                           uint32_t *length) {
    uint32_t np = u32(p), nc = u32(p + 4);
    p += 8;
    const char *name = 0;
    unsigned char *first = p;
    for (uint32_t i = 0; i < np; i++) {
        if (eq((char *)p, "name"))
            name = (char *)p + 36;
        p += 36 + ((u32(p + 32) + 3) & ~3U);
    }
    if (name && eq(name, wanted))
        for (uint32_t i = 0; i < np; i++) {
            uint32_t n = u32(first + 32);
            if (eq((char *)first, prop)) {
                *result = first + 36;
                *length = n;
            }
            first += 36 + ((n + 3) & ~3U);
        }
    for (uint32_t i = 0; i < nc; i++)
        p = walk(p, wanted, prop, result, length);
    return p;
}
void *dt_property(void *tree, const char *node, const char *prop, uint32_t *n) {
    void *r = 0;
    *n = 0;
    walk(tree, node, prop, &r, n);
    return r;
}
void *dt_node_property(void *node, const char *prop, uint32_t *length) {
    unsigned char *p = node;
    uint32_t np = u32(p);
    p += 8;
    for (uint32_t i = 0; i < np; i++) {
        uint32_t n = u32(p + 32);
        if (eq((char *)p, prop)) {
            *length = n;
            return p + 36;
        }
        p += 36 + ((n + 3) & ~3U);
    }
    *length = 0;
    return 0;
}
static unsigned char *find_phandle(unsigned char *p, uint32_t phandle, void **result) {
    uint32_t np = u32(p), nc = u32(p + 4), n;
    void *id = dt_node_property(p, "AAPL,phandle", &n);
    if (id && n == 4 && u32(id) == phandle)
        *result = p;
    p += 8;
    for (uint32_t i = 0; i < np; i++)
        p += 36 + ((u32(p + 32) + 3) & ~3U);
    for (uint32_t i = 0; i < nc; i++)
        p = find_phandle(p, phandle, result);
    return p;
}
void *dt_find_phandle(void *tree, uint32_t phandle) {
    void *result = 0;
    find_phandle(tree, phandle, &result);
    return result;
}
static unsigned char *patch_cpus(unsigned char *p, uint64_t mpidr, unsigned int *bootfound) {
    uint32_t np = u32(p), nc = u32(p + 4), n;
    char *type = dt_node_property(p, "device_type", &n);
    if (type && n == 4 && eq(type, "cpu")) {
        void *reg = dt_node_property(p, "reg", &n);
        uint64_t id = ~0ULL;
        if (reg && n == 4)
            id = u32(reg);
        else if (reg && n == 8)
            id = u64(reg);
        char *state = dt_node_property(p, "state", &n);
        if (!state || n != 8)
            return 0;
        int running = (mpidr & 0xffffff) == id;
        if (running)
            (*bootfound)++;
        memcpy(state, running ? "running" : "waiting", 8);
    }
    p += 8;
    for (uint32_t i = 0; i < np; i++)
        p += 36 + ((u32(p + 32) + 3) & ~3U);
    for (uint32_t i = 0; i < nc; i++) {
        p = patch_cpus(p, mpidr, bootfound);
        if (!p)
            return 0;
    }
    return p;
}
static uint64_t align_up(uint64_t n, uint64_t a) { return (n + a - 1) & ~(a - 1); }
static int patch(void *tree, const char *node, const char *prop, const void *value, uint32_t size) {
    uint32_t n;
    void *p = dt_property(tree, node, prop, &n);
    if (!p || n != size)
        return -1;
    memcpy(p, value, n);
    return 0;
}
int bridge_set_platform_identity(void *tree, uint32_t serial, uint32_t chip) {
    uint32_t serial_size = 0, chip_size = 0;
    void *serial_property = dt_property(tree, "chosen", "unique-chip-id", &serial_size);
    void *chip_property = dt_property(tree, "chosen", "chip-id", &chip_size);
    if (!serial || serial == UINT32_MAX || !chip || chip == UINT32_MAX ||
        !serial_property || serial_size != 8 || !chip_property || chip_size != 4 ||
        u64(serial_property) || u32(chip_property))
        return -1;
    // Qualcomm supplies a 32-bit hardware serial. Zero-extension preserves
    // its value in Apple's 64-bit DT field; neither input is a secret key.
    uint64_t unique_chip = serial;
    memcpy(serial_property, &unique_chip, sizeof(unique_chip));
    memcpy(chip_property, &chip, sizeof(chip));
    return 0;
}
int bridge_prepare(void *arena, size_t capacity, const void *kernel, size_t ksize, const void *dt,
                   size_t dtsize, const void *trust_cache, size_t tcsize, uint64_t mpidr, struct boot_args **result,
                   struct kernel_info *k) {
    if (macho_info(kernel, ksize, k))
        return -1;
    uint64_t pa = (uintptr_t)arena;
    if ((pa & 0x1fffff) || capacity < 0x4000000 || capacity > 0x40000000)
        return -2;
    if (dtsize > BRIDGE_MAX_DEVICE_TREE_SIZE || tcsize > 0x1000000 || (tcsize && !trust_cache))
        return -3;
    uint64_t dtoff = align_up(k->end - k->virt, 0x4000);
    uint64_t tcoff = align_up(dtoff + dtsize, 0x4000);
    uint64_t baoff = align_up(tcoff + tcsize, 0x4000);
    // Keep panic RAM below topOfKernelData so XNU's VM never allocates it.
    uint64_t panicoff = align_up(baoff + sizeof(struct boot_args), 0x4000);
    const uint64_t panicsize = 0x100000;
    uint64_t topoff = panicoff + panicsize;
    if (topoff > capacity)
        return -3;
    if (macho_load(kernel, ksize, k, arena, capacity))
        return -4;
    void *tree = (unsigned char *)arena + dtoff;
    memcpy(tree, dt, dtsize);
    uint64_t range[2] = {pa, capacity};
    if (patch(tree, "chosen", "dram-base", &range[0], 8) ||
        patch(tree, "chosen", "dram-size", &range[1], 8) || patch(tree, "memory", "reg", range, 16))
        return -5;
    range[0] = pa;
    range[1] = k->end - k->virt;
    if (patch(tree, "memory-map", "Kernel", range, 16))
        return -6;
    range[0] = pa + dtoff;
    range[1] = dtsize;
    if (patch(tree, "memory-map", "DeviceTree", range, 16))
        return -6;
    uint32_t tc_property_size;
    void *tc_property = dt_property(tree, "memory-map", "TrustCache", &tc_property_size);
    if (tcsize) {
        range[0] = pa + tcoff;
        range[1] = tcsize;
        if (patch(tree, "memory-map", "TrustCache", range, 16))
            return -6;
        memcpy((unsigned char *)arena + tcoff, trust_cache, tcsize);
    } else if (tc_property) {
        // A stale DT pointer must never refer back into the EFI loader.
        return -6;
    }
    range[0] = pa + baoff;
    range[1] = sizeof(struct boot_args);
    if (patch(tree, "memory-map", "BootArgs", range, 16))
        return -6;
    range[0] = pa + panicoff;
    range[1] = panicsize;
    uint32_t panic_length = (uint32_t)panicsize;
    if (patch(tree, "pram", "reg", range, 16) ||
        patch(tree, "chosen", "embedded-panic-log-size", &panic_length, 4) ||
        patch(tree, "memory-map", "PanicLog", range, 16))
        return -6;
    memset((unsigned char *)arena + panicoff, 0, panicsize);
    unsigned int bootfound = 0;
    if (!patch_cpus(tree, mpidr, &bootfound) || bootfound != 1)
        return -8;
    struct boot_args *ba = (void *)((unsigned char *)arena + baoff);
    memset(ba, 0, sizeof(*ba));
    ba->Revision = 2;
    ba->Version = 2;
    ba->virtBase = k->virt;
    ba->physBase = pa;
    ba->memSize = capacity;
    ba->memSizeActual = capacity;
    ba->topOfKernelData = pa + topoff;
    ba->deviceTreeP = k->virt + dtoff;
    ba->deviceTreeLength = dtsize;
    uint32_t cmdlen;
    void *cmd = dt_property(tree, "chosen", "bootargs", &cmdlen);
    if (!cmd || !cmdlen || cmdlen > sizeof(ba->CommandLine) || ((char *)cmd)[cmdlen - 1])
        return -9;
    memcpy(ba->CommandLine, cmd, cmdlen);
    *result = ba;
    return 0;
}

int bridge_set_video(struct boot_args *ba, uint64_t base, uint64_t size, uint32_t width,
                     uint32_t height, uint32_t stride) {
    if (!base || (base & 3) || width < 8 || height < 16 || width > 16384 || height > 16384 ||
        stride < (uint64_t)width * 4 || (stride & 3) || (uint64_t)stride * height > size ||
        size > 0x20000000 || size > ~0ULL - base)
        return -1;
    ba->video[0] = base | 1; // XNU's low bit marks a physical framebuffer address.
    ba->video[1] = 0;        // text console, no graphical boot progress overlay
    ba->video[2] = stride;
    ba->video[3] = width;
    ba->video[4] = height;
    ba->video[5] = 32;
    return 0;
}
