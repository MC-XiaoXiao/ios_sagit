#include <Resolver.h>

#include <cstring>
#include <iostream>
#include <vector>

namespace {

struct TestImage {
    std::vector<char> bytes;
    Macho macho;

    TestImage(uint64_t data_base, uint64_t text_base, uint64_t function0, uint64_t function1)
        : bytes(0x2000, 0)
        , macho(bytes.data(), static_cast<uint32_t>(bytes.size()))
    {
        auto* header = reinterpret_cast<mach_header_64_t*>(bytes.data());
        header->magic = MH_MAGIC_64;
        header->ncmds = 2;
        header->sizeofcmds = 2 * sizeof(segment_command_64_t);

        auto* data = reinterpret_cast<segment_command_64_t*>(header + 1);
        data->cmd = LC_SEGMENT_64;
        data->cmdsize = sizeof(*data);
        memcpy(data->segname, "__DATA_CONST", 12);
        data->vmaddr = data_base;
        data->vmsize = 0x1000;
        data->fileoff = 0;
        data->filesize = 0x1000;
        data->initprot = 1;

        auto* text = data + 1;
        text->cmd = LC_SEGMENT_64;
        text->cmdsize = sizeof(*text);
        memcpy(text->segname, "__TEXT_EXEC", 11);
        text->vmaddr = text_base;
        text->vmsize = 0x1000;
        text->fileoff = 0x1000;
        text->filesize = 0x1000;
        text->initprot = 5;

        uint64_t* vtable = reinterpret_cast<uint64_t*>(bytes.data() + 0x100);
        vtable[0] = 0;
        vtable[1] = 0;
        vtable[2] = function0;
        vtable[3] = function1;
        vtable[4] = 0;

        macho.format_macho();
        macho.add_symbol(data_base + 0x100, "__ZTV4Test");
    }
};

} // namespace

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition << '\n'; \
        return 1; \
    } \
} while (false)

int main()
{
    TestImage source(0x1000, 0x2000, 0x2000, 0x2004);
    TestImage target(0x5000, 0x6000, 0x6000, 0x6004);
    source.macho.add_symbol(0x2000, "__ZN4Test1fEv");
    target.macho.add_symbol(0x6000, "__ZN4Test1fEv");

    LinkResolver resolver(source.macho, target.macho);
    ResolverStats stats = resolver.build();
    CHECK(stats.matched_vtables == 1);
    CHECK(stats.matched_vtable_slots == 2);
    uint64_t result = 0;
    CHECK(resolver.resolve(0x1100, result) && result == 0x5100);
    CHECK(resolver.resolve(0x2004, result) && result == 0x6004);

    // A named anchor in a different slot rejects unsafe positional matching.
    TestImage shifted_target(0x9000, 0xa000, 0xa004, 0xa000);
    shifted_target.macho.add_symbol(0xa000, "__ZN4Test1fEv");
    LinkResolver rejected(source.macho, shifted_target.macho);
    stats = rejected.build();
    CHECK(stats.matched_vtables == 0);
    CHECK(stats.rejected_vtables == 1);
    CHECK(rejected.rejections().size() == 1);
    CHECK(rejected.rejections()[0].reason == "named-anchor-mismatch");
    CHECK(rejected.rejections()[0].slot == 0);
    CHECK(rejected.rejections()[0].expected_pointer == 0xa000);
    CHECK(rejected.rejections()[0].target_pointer == 0xa004);
    CHECK(!rejected.resolve(0x2004, result));
    CHECK(!rejected.resolve(0x1100, result));

    // Never map an equal prefix when the target has extra virtual methods.
    TestImage longer_target(0xb000, 0xc000, 0xc000, 0xc004);
    reinterpret_cast<uint64_t*>(longer_target.bytes.data() + 0x100)[4] = 0xc008;
    LinkResolver count_mismatch(source.macho, longer_target.macho);
    stats = count_mismatch.build();
    CHECK(stats.rejected_vtables == 1);
    CHECK(count_mismatch.rejections()[0].reason == "slot-count-mismatch");
    CHECK(count_mismatch.rejections()[0].source_slots == 2);
    CHECK(count_mismatch.rejections()[0].target_slots == 3);
    CHECK(!count_mismatch.resolve(0x2004, result));
    CHECK(!count_mismatch.resolve(0x1100, result));
    return 0;
}
