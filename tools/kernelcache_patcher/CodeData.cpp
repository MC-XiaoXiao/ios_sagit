#include <CodeData.h>
#include <cstring>
#include <algorithm>
#include <AppleMathTables.inc>

std::vector<std::pair<size_t, size_t>> find_verified_code_data(const uint8_t* bytes, size_t size, const char* bundle)
{
    auto result = find_sha256_tables(bytes, size);
    // Algorithmic constant pools are recognized by complete mathematical
    // content, independently of a bundle name, address or firmware version.
    std::vector<std::vector<uint8_t>> patterns(3);
    for (unsigned n = 0; n <= 64; ++n) {
        uint64_t mask = n == 64 ? UINT64_MAX : (UINT64_C(1) << n) - 1;
        for (unsigned byte = 0; byte < 8; ++byte) patterns[0].push_back(mask >> (byte * 8));
    }
    for (unsigned distance = 0; distance < 16; ++distance)
        for (unsigned lane = 0; lane < 32; ++lane)
            patterns[1].push_back(distance ? lane % distance : 0);
    for (unsigned distance = 0; distance < 16; ++distance)
        patterns[1].push_back(!distance ? 32 :
            ((distance == 4 || distance == 8) ? 16 : (32 / distance) * distance));
    for (unsigned n = 32; n; --n) patterns[2].push_back(n);
    for (uint32_t value : {UINT32_C(65521), static_cast<uint32_t>(((UINT64_C(1) << 47) + 65520) / 65521)})
        for (unsigned byte = 0; byte < 4; ++byte) patterns[2].push_back(value >> (byte * 8));
    for (const auto& pattern : patterns)
        for (size_t offset = 0; offset + pattern.size() <= size; offset += 4)
            if (!memcmp(bytes + offset, pattern.data(), pattern.size()))
                result.emplace_back(offset, pattern.size());

    auto word = [&](size_t offset) { uint32_t w; memcpy(&w, bytes + offset, 4); return w; };
    // A byte-masked opcode indexes a signed-halfword table, followed by
    // ADD(table, offset)/BR. Verify all 256 entries target BTI jump labels.
    // The ADR reference and bounded dispatch prove the whole table is data.
    for (size_t i = 0; i + 16 <= size; i += 4) {
        uint32_t bound = word(i), load = word(i + 4), add = word(i + 8), branch = word(i + 12);
        if ((bound & 0xfffffc00) != 0x92401c00 && (bound & 0xfffffc00) != 0x12001c00) continue;
        unsigned index = bound & 31, base = (load >> 5) & 31;
        if (index == 31 || base == 31 || index == base || (load & 0xffe0fc00) != 0x78a07800
            || (load & 31) != index || ((load >> 16) & 31) != index
            || (add & 0xffe0fc00) != 0x8b000000 || (add & 31) != index
            || ((add >> 5) & 31) != base || ((add >> 16) & 31) != index
            || branch != (0xd61f0000 | (index << 5))) continue;
        for (size_t back = i; back >= 4 && i - back < 256;) {
            back -= 4;
            uint32_t adr = word(back);
            if ((adr & 31) != base) continue;
            if ((adr & 0x9f000000) != 0x10000000) break;
            int64_t immediate = ((adr >> 29) & 3) | (((adr >> 5) & 0x7ffff) << 2);
            if (immediate & (1 << 20)) immediate -= 1 << 21;
            int64_t table = static_cast<int64_t>(back) + immediate;
            if (table < 0 || (table & 3) || static_cast<uint64_t>(table) + 512 > size) break;
            bool valid = true;
            for (unsigned n = 0; n < 256; ++n) {
                int16_t displacement; memcpy(&displacement, bytes + table + n * 2, 2);
                int64_t target = table + displacement;
                if (target < 0 || target >= table || (target & 3)
                    || (word(target) != 0xd503249f && word(target) != 0xd50324df)) { valid = false; break; }
            }
            const std::pair<size_t, size_t> span {static_cast<size_t>(table), 512};
            if (valid && std::find(result.begin(), result.end(), span) == result.end()) result.push_back(span);
            break;
        }
    }
    if (!bundle || strcmp(bundle, "com.apple.kec.Libm"))
        return result;
    for (const auto& pattern : kMathConstantPatterns) {
        if (pattern.size > size)
            continue;
        for (size_t offset = 0; offset <= size - pattern.size; offset += 4)
            if (!memcmp(bytes + offset, pattern.bytes, pattern.size))
                result.emplace_back(offset, pattern.size);
    }
    return result;
}

std::vector<std::pair<size_t, size_t>> find_sha256_tables(const uint8_t* bytes, size_t size)
{
    // FIPS 180-4 constants, also in XNU osfmk/corecrypto/ccsha256_K.c.
    static const uint32_t k[] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    std::vector<std::pair<size_t, size_t>> result;
    for (size_t offset = 0; offset + sizeof(k) <= size; offset += 4)
        if (!memcmp(bytes + offset, k, sizeof(k)))
            result.emplace_back(offset, sizeof(k));
    return result;
}
