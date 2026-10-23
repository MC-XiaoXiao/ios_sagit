#include <Patcher.h>
#include <iostream>
#include <CodeData.h>
#include <AppleMathTables.inc>
#include <cstring>
#include <array>

int main()
{
    {
    // A real clock rate pointer can precede __cstring in __TEXT. The old
    // string-boundary heuristic left its low local address (0x1128) intact.
    uint8_t text[0x1200] = {}, source[32] = {}, output[32] = {};
    uint64_t pointer = 0x1128, scalar = 0x1128;
    memcpy(source, &pointer, 8); memcpy(source + 8, &scalar, 8);
    memset(output, 0x5a, sizeof(output));
    patcher_detail::LocalRebaseSegment maps[] = {
        {0, sizeof(text), sizeof(text), 0xfffffff006700000ULL, text, text},
        {0xc000, 64, sizeof(source), 0xfffffff008400000ULL, source, output}
    };
    relocation_info records[2] = {};
    records[0].r_address = 0xc000; records[0].r_length = 3; records[0].r_symbolnum = 1;
    if (!patcher_detail::rebase_local_pointers(records, 1, maps, 2, 0)) return 50;
    uint64_t result; memcpy(&result, output, 8);
    if (result != 0xfffffff006701128ULL || output[8] != 0x5a) return 51;
    // A target in zero-fill memory is valid; an unbacked relocation site is not.
    pointer = 0xc030; memcpy(source, &pointer, 8);
    if (!patcher_detail::rebase_local_pointers(records, 1, maps, 2, 0)) return 52;
    memcpy(&result, output, 8); if (result != 0xfffffff008400030ULL) return 53;
    records[1] = records[0]; records[1].r_address = 0xc01c;
    memset(output, 0x5a, sizeof(output)); size_t invalid = SIZE_MAX;
    if (patcher_detail::rebase_local_pointers(records, 2, maps, 2, 0, &invalid) || invalid != 1 || output[0] != 0x5a) return 54;
    records[1].r_address = 0xc008; records[1].r_type = 2;
    if (patcher_detail::rebase_local_pointers(records, 2, maps, 2, 0) || output[0] != 0x5a) return 55;
    records[1] = records[0]; records[1].r_symbolnum = 0;
    if (!patcher_detail::rebase_local_pointers(records, 2, maps, 2, 0)) return 56;
    }

    // A full source page with a page-aligned affine translation needs no
    // PAGEOFF12 edits, including all byte offsets through an opaque branch.
    uint64_t relocated = 0;
    if (!patcher_detail::rebase_uniform_page(0x12000, 0x10780, 0x4bd20,
            0x80780, relocated) || relocated != 0x82000) return 40;
    for (uint64_t offset = 0; offset < 4096; ++offset)
        if (relocated + offset != 0x80780 + (0x12000 + offset - 0x10780)) return 41;
    // Prefix/suffix fragments, a shifted page offset and overflowing output
    // must retain the strict path-sensitive relocation checks.
    for (const auto& invalid : std::vector<std::array<uint64_t,4>>{
            {0x10000,0x10780,0x4bd20,0x80780},
            {0x12000,0x10780,0x1fff,0x80780},
            {0x12000,0x10780,0x4bd20,0x80a00},
            {0x12004,0x10780,0x4bd20,0x80780},
            {0x12000,0x10780,0x4bd20,UINT64_MAX-0x1000}}) {
        relocated = 0xabc;
        if (patcher_detail::rebase_uniform_page(invalid[0],invalid[1],invalid[2],invalid[3],relocated)
            || relocated != 0xabc) return 42;
    }
    // Actual bundle-name suffixes that previously looked like local code
    // pointers. Check packed native kmod layouts at both 4/8-byte alignments.
    for (uint64_t kmod : {0x1000ULL,0x1004ULL}) {
        for (uint64_t offset = 0; offset < 196; offset += 8) {
            bool expected = offset < 144 && offset + 8 > 16;
            if (patcher_detail::kmod_identity_word(kmod,kmod+offset) != expected) return 5;
        }
        if (patcher_detail::kmod_identity_word(0,kmod) || patcher_detail::kmod_identity_word(kmod,kmod-8)) return 6;
    }
    for (const char *name : {"org.darwin.driver.QcomSPMI","org.darwin.driver.QcomMMCC"}) {
        uint8_t kmod[200] = {}; std::strcpy(reinterpret_cast<char *>(kmod+16),name);
        uint8_t original[200]; std::memcpy(original,kmod,sizeof(kmod));
        for (unsigned offset = 0; offset < 196; offset += 8) {
            uint64_t value; std::memcpy(&value,kmod+offset,8);
            if (!patcher_detail::kmod_identity_word(0x1000,0x1000+offset) && value >= 0x4000 && value < 0x8000) {
                value += 0xfffffff006f00000ULL; std::memcpy(kmod+offset,&value,8);
            }
        }
        if (std::memcmp(original,kmod,sizeof(kmod))) return 7;
    }
    std::vector<uint8_t> literal(kMathExp128, kMathExp128 + sizeof(kMathExp128));
    auto pools = find_verified_code_data(literal.data(), literal.size(), "com.apple.kec.Libm");
    if (pools != std::vector<std::pair<size_t, size_t>>{{0, sizeof(kMathExp128)}})
        return 2;
    if (!find_verified_code_data(literal.data(), literal.size(), "org.example.code").empty())
        return 3;
    literal[100] ^= 1;
    if (!find_verified_code_data(literal.data(), literal.size(), "com.apple.kec.Libm").empty())
        return 4;
    // A byte-bounded signed16 dispatch is structural, with no bundle/address
    // allowlist. Every entry must point to a real jump landing label.
    std::vector<uint8_t> dispatch_pool(0x240, 0);
    const uint32_t code[] = {0x10000210, 0x92401cd1, 0x78b17a11, 0x8b110211, 0xd61f0220};
    memcpy(dispatch_pool.data(), code, sizeof(code));
    uint32_t landing = 0xd503249f;
    memcpy(dispatch_pool.data() + 0x20, &landing, 4);
    memcpy(dispatch_pool.data() + 0x30, &landing, 4);
    for (unsigned n = 0; n < 256; ++n) {
        int16_t displacement = n % 2 ? -16 : -32;
        memcpy(dispatch_pool.data() + 0x40 + n * 2, &displacement, 2);
    }
    const char* unrelated = "org.example.unrelated-driver";
    if (find_verified_code_data(dispatch_pool.data(), dispatch_pool.size(), unrelated)
        != std::vector<std::pair<size_t, size_t>>{{0x40, 512}}) return 14;
    if (!find_verified_code_data(dispatch_pool.data(), dispatch_pool.size() - 4, unrelated).empty()) return 15;
    dispatch_pool[0x40] = 4; dispatch_pool[0x41] = 0; // points into the data table
    if (!find_verified_code_data(dispatch_pool.data(), dispatch_pool.size(), unrelated).empty()) return 16;
    // The live ADRP register reaches a consumer through a backward branch,
    // including a branch to instruction zero. A dead linear fallthrough must
    // not supply a different displacement.
    const uint32_t words[] = {
        0xf94082a0, // ldr x0, [x21, #0x100]
        0xd65f03c0, // ret
        0x90000015, // adrp x21, page
        0x17fffffd, // b -12 -> instruction 0
        0xf940c2a1, // unreachable ldr x1, [x21, #0x180]
        0xd65f03c0,
    };
    std::vector<size_t> consumers;
    if (!patcher_detail::test_adrp_consumers(words, 6, 2, consumers)
        || consumers != std::vector<size_t> { 0 }) {
        std::cerr << "backward ADRP consumer was missed or dead fallthrough was included\n";
        return 1;
    }
    // A bounded switch must visit cases that use the page register even when
    // the first case overwrites it. The table contains signed ADR-relative
    // offsets, exactly as in the native Sandbox policy parser.
    uint32_t dispatch[] = {
        0x90000018, 0x91010318, // adrp/add x24, table at 0x1040
        0x9000001b,             // adrp x27, page
        0x7100097f, 0x54000168, // cmp w11,#2; b.hi fallback
        0x1000000d, 0xb8ab7b0e, // adr x13,self; ldrsw x14,[x24,x11,lsl#2]
        0x8b0e01ad, 0xd61f01a0, // add x13,x13,x14; br x13
        0xaa1f03fb, 0xd65f03c0, // case0: mov x27,xzr; ret
        0xf9408360, 0xd65f03c0, // case1: ldr x0,[x27,#0x100]; ret
        0xf9408361, 0xd65f03c0, // case2: ldr x1,[x27,#0x100]; ret
        0xd65f03c0,             // fallback: ret
        0x10, 0x18, 0x20,       // table: case0/1/2 - anchor
    };
    consumers.clear();
    if (!patcher_detail::test_adrp_consumers(dispatch, 19, 2, consumers)
        || consumers != std::vector<size_t>{11, 13}) {
        std::cerr << "bounded switch lost live ADRP consumers\n";
        return 8;
    }
    // An out-of-image jump-table target must not be treated as fallthrough.
    dispatch[18] = 0x100000;
    consumers.clear();
    if (patcher_detail::test_adrp_consumers(dispatch, 19, 2, consumers)) return 9;
    dispatch[18] = 0x20;
    // Without a valid table bound, leave this indirect branch unresolved.
    dispatch[3] = 0x7104017f; // cmp w11,#256 (over the supported bound)
    consumers.clear();
    if (patcher_detail::test_adrp_consumers(dispatch, 19, 2, consumers)) return 10;
    // APFS copies its persistent DATA page to x23 but still reads a different
    // global through x27 before overwriting x27. Both page values survive BL.
    const uint32_t apfs_stats[] = {
        0x9000001b, // adrp x27, page
        0xf9400000 | ((0xb88 / 8) << 10) | (27 << 5),
        0xaa1b03f7, // mov x23, x27 (does not destroy x27)
        0xf945ef68, // ldr x8, [x27, #0xbd8]
        0x94000002, // bl (callee preserves x23 and x27)
        0xf9400000 | ((0xbe0 / 8) << 10) | (23 << 5),
        0xd65f03c0,
    };
    consumers.clear();
    if (!patcher_detail::test_adrp_consumers(apfs_stats, 7, 0, consumers)
        || consumers != std::vector<size_t>{1, 3, 5}) {
        std::cerr << "APFS page copy lost the original register or a different global offset\n";
        return 11;
    }
    const uint32_t caller_saved[] = {
        0x90000008, // adrp x8, page
        0xf9408100, // ldr x0, [x8, #0x100]
        0x94000002, // bl destroys x8
        0xf940c101, // unrelated ldr x1, [x8, #0x180]
        0xd65f03c0,
    };
    consumers.clear();
    if (!patcher_detail::test_adrp_consumers(caller_saved, 5, 0, consumers)
        || consumers != std::vector<size_t>{1}) return 12;
    // A copied register can hold either the page or an object on different
    // paths. The object's field displacement must never become a relocation.
    const uint32_t merged_object[] = {
        0x9000001b, // adrp x27, page
        0xf9408360, // ldr x0, [x27, #0x100]
        0x35000061, // cbnz w1, instruction 5
        0xaa1b03f7, // mov x23, x27
        0x14000002, // b instruction 6
        0xaa0003f7, // mov x23, x0 (object pointer)
        0xaa1703fc, // mov x28, x23
        0xf9001f95, // str x21, [x28, #0x38] (object field)
        0xf945ef68, // ldr x8, [x27, #0xbd8] (still the page)
        0xd65f03c0,
    };
    consumers.clear();
    std::vector<size_t> uncertain;
    if (!patcher_detail::test_adrp_consumers(merged_object, 10, 0, consumers, &uncertain)
        || consumers != std::vector<size_t>{1, 8}
        || uncertain != std::vector<size_t>{7}) return 13;
    const uint32_t reloaded_page[] = {
        0x90000007, // adrp x7, page
        0xb4000080, // cbz x0, instruction 5
        0xaa0e03e7, // mov x7, x14 (unrelated arithmetic value)
        0x90000007, // adrp x7, the same page
        0x14000002, // b instruction 6
        0xd503201f,
        0x3dc058f3, // ldr q19, [x7, #0x160]
        0xd65f03c0,
    };
    consumers.clear(); uncertain.clear();
    if (!patcher_detail::test_adrp_consumers(reloaded_page, 8, 0, consumers, &uncertain)
        || consumers != std::vector<size_t>{6} || !uncertain.empty()) return 14;
    const uint32_t independent_page[] = {
        0x90000007, 0x3dc058f3, // adrp x7; ldr q19,[x7,#0x160]
        0x90000007, 0x3dc05cf3, // independent same-page ADRP and #0x170
        0xd65f03c0,
    };
    consumers.clear();
    if (!patcher_detail::test_adrp_consumers(independent_page, 5, 0, consumers)
        || consumers != std::vector<size_t>{1}) return 15;
    // IONetworkingFamily tail-calls through a loaded pointer without a stack
    // frame. A later same-page ADRP is a new definition: its live register
    // must not make the earlier, already consumed definition fail at BR.
    const uint32_t frameless_tail[] = {
        0x90000008, 0xb941f908, // adrp x8; ldr w8,[x8,#0x1f8]
        0x34000068, 0x52800000, 0xd65f03c0, // cbz; mov w0,#0; ret
        0x90000008, 0xf940f101, // adrp x8; ldr x1,[x8,#0x1e0]
        0x90000008, 0xf940e500, // adrp x8; ldr x0,[x8,#0x1c8]
        0xd61f0020, // br x1
    };
    for (size_t start : {size_t(0), size_t(5), size_t(7)}) {
        consumers.clear();
        if (!patcher_detail::test_adrp_consumers(frameless_tail, 10, start, consumers)
            || consumers != std::vector<size_t>{start + 1}) return 17;
    }
    // Arithmetic that changes the target is not a global-pointer tail call.
    uint32_t computed_tail[] = {0x90000008, 0xf940f101, 0x8b000021, 0xd61f0020};
    consumers.clear();
    if (patcher_detail::test_adrp_consumers(computed_tail, 4, 0, consumers)) return 18;
    // Stack guard failure stubs are noreturn. Model an exact ADRP/LDR/BR
    // through a real mapped pointer, and reject an unproven or changed slot.
    std::vector<char> image_bytes(0x4000, 0);
    auto* header = reinterpret_cast<mach_header_64_t*>(image_bytes.data());
    header->magic = MH_MAGIC_64; header->ncmds = 2;
    header->sizeofcmds = 2 * sizeof(segment_command_64_t);
    auto* text = reinterpret_cast<segment_command_64_t*>(header + 1);
    text->cmd = LC_SEGMENT_64; text->cmdsize = sizeof(*text);
    std::strcpy(text->segname, "__TEXT_EXEC");
    text->vmaddr = text->fileoff = 0x1000; text->vmsize = text->filesize = 0x1000; text->initprot = 5;
    auto* data = text + 1;
    data->cmd = LC_SEGMENT_64; data->cmdsize = sizeof(*data);
    std::strcpy(data->segname, "__DATA_CONST");
    data->vmaddr = data->fileoff = 0x3000; data->vmsize = data->filesize = 0x1000; data->initprot = 1;
    const uint32_t guard_stub[] = {0xd0000010, 0xf9401610, 0xd61f0200};
    std::memcpy(image_bytes.data() + 0x1800, guard_stub, sizeof(guard_stub));
    uint64_t guard_pointer = 0x1900;
    std::memcpy(image_bytes.data() + 0x3028, &guard_pointer, 8);
    Macho image(image_bytes.data(), static_cast<uint32_t>(image_bytes.size()));
    image.format_macho(); image.add_symbol(0x1900, "___stack_chk_fail");
    uint32_t guard_path[] = {0x90000019, 0xb9410328, 0x940001fe, 0xd61f01e0};
    consumers.clear();
    if (!patcher_detail::test_adrp_consumers(guard_path, 4, 0, consumers, nullptr, &image)
        || consumers != std::vector<size_t>{1}) return 19;
    consumers.clear();
    if (patcher_detail::test_adrp_consumers(guard_path, 4, 0, consumers)) return 20;
    guard_pointer = 0x1904;
    std::memcpy(image_bytes.data() + 0x3028, &guard_pointer, 8);
    consumers.clear();
    if (patcher_detail::test_adrp_consumers(guard_path, 4, 0, consumers, nullptr, &image)) return 21;
    // A boolean pointer table has two edges even when one immediately kills
    // the live page. The other edge still consumes the original DATA page.
    uint32_t boolean_dispatch[] = {
        0x9000000a, 0xf9430d40, // adrp x10; ldr x0,[x10,#0x618]
        0xd000000b, 0x9104016b, // adrp/add x11, pointer slot at 0x3100
        0xeb0d003f, 0x1a9f17e8, // cmp x1,x13; cset w8,eq
        0x52800589, 0x1b097d08, // mov w9,#44; mul w8,w8,w9
        0x521e0508, 0xf9400169, // eor w8,w8,#12; ldr x9,[x11]
        0xd1003129, 0xf9002be9, // sub x9,x9,#12; str x9,[sp,#0x50]
        0xf8685928, 0xd1002108, 0xd61f0100, // ldr/sub/br x8
        0xaa1f03ea, 0xd65f03c0, // case1: kill x10; ret
        0xf9430d41, 0xd65f03c0, // case0: ldr x1,[x10,#0x618]; ret
    };
    uint64_t table_pointer = 0x320c, case0 = 0x104c, case1 = 0x1044;
    std::memcpy(image_bytes.data() + 0x3100, &table_pointer, 8);
    std::memcpy(image_bytes.data() + 0x3260, &case0, 8);
    std::memcpy(image_bytes.data() + 0x3300, &case1, 8);
    consumers.clear();
    if (!patcher_detail::test_adrp_consumers(boolean_dispatch, 19, 0, consumers, nullptr, &image)
        || consumers != std::vector<size_t>{1, 17}) return 43;
    consumers.clear();
    if (patcher_detail::test_adrp_consumers(boolean_dispatch, 19, 0, consumers)) return 44;
    // Neither a writable table nor one valid edge can prove the other edge.
    data->initprot = 3;
    consumers.clear();
    if (patcher_detail::test_adrp_consumers(boolean_dispatch, 19, 0, consumers, nullptr, &image)) return 45;
    data->initprot = 1;
    case0 = 0x1908;
    std::memcpy(image_bytes.data() + 0x3260, &case0, 8);
    consumers.clear();
    if (patcher_detail::test_adrp_consumers(boolean_dispatch, 19, 0, consumers, nullptr, &image)) return 46;
    case0 = 0x104c;
    std::memcpy(image_bytes.data() + 0x3260, &case0, 8);
    boolean_dispatch[6] = 0x52802009; // mov w9,#256, beyond the bounded table
    consumers.clear();
    if (patcher_detail::test_adrp_consumers(boolean_dispatch, 19, 0, consumers, nullptr, &image)) return 47;
    boolean_dispatch[6] = 0x52800589;
    boolean_dispatch[4] = 0xaa0003eb; // mov x11,x0 destroys the proven slot address
    consumers.clear();
    if (patcher_detail::test_adrp_consumers(boolean_dispatch, 19, 0, consumers, nullptr, &image)) return 48;
    boolean_dispatch[4] = 0xeb0d003f;
    table_pointer = 0x3ffc;
    std::memcpy(image_bytes.data() + 0x3100, &table_pointer, 8);
    consumers.clear();
    if (patcher_detail::test_adrp_consumers(boolean_dispatch, 19, 0, consumers, nullptr, &image)) return 49;
    return 0;
}
