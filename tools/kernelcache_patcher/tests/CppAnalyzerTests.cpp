#include <CppAnalyzer.h>

#include <cstring>
#include <iostream>
#include <vector>

namespace {

struct TestImage {
    std::vector<char> bytes;
    Macho macho;

    TestImage()
        : bytes(0x2000, 0)
        , macho(bytes.data(), static_cast<uint32_t>(bytes.size()))
    {
        auto* header = reinterpret_cast<mach_header_64_t*>(bytes.data());
        header->magic = MH_MAGIC_64;
        header->ncmds = 2;
        header->sizeofcmds = 2 * sizeof(segment_command_64_t);

        auto* text = reinterpret_cast<segment_command_64_t*>(header + 1);
        text->cmd = LC_SEGMENT_64;
        text->cmdsize = sizeof(*text);
        memcpy(text->segname, "__TEXT_EXEC", 11);
        text->vmaddr = 0x1000;
        text->vmsize = 0x1000;
        text->fileoff = 0x1000;
        text->filesize = 0x1000;
        text->initprot = 5;

        auto* data = text + 1;
        data->cmd = LC_SEGMENT_64;
        data->cmdsize = sizeof(*data);
        memcpy(data->segname, "__DATA_CONST", 12);
        data->vmaddr = 0x3000;
        data->vmsize = 0x800;
        data->fileoff = 0x800;
        data->filesize = 0x800;
        data->initprot = 1;

        macho.format_macho();
    }

    void write(uint64_t address, uint32_t instruction)
    {
        memcpy(bytes.data() + address, &instruction, sizeof(instruction));
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
    TestImage image;
    constexpr uint64_t constructor = 0x1100;
    image.write(constructor, 0xd503201fU);      // nop
    image.write(constructor + 4, 0xd65f03c0U);  // ret
    image.write(constructor + 8, 0x14000000U);  // b . (D2 tail thunk)
    image.write(constructor + 12, 0xd4200020U); // brk #1 (D1)
    image.write(constructor + 16, 0xd4200040U); // brk #2 (D0)

    uint64_t d2 = 0, d1 = 0, d0 = 0;
    CHECK(cpp_analyzer_detail::find_abstract_structor_layout(
        image.macho, constructor, d2, d1, d0));
    CHECK(d2 == constructor + 8);
    CHECK(d1 == constructor + 12);
    CHECK(d0 == constructor + 16);

    image.write(constructor + 16, 0xd503201fU);
    CHECK(!cpp_analyzer_detail::find_abstract_structor_layout(
        image.macho, constructor, d2, d1, d0));
    CHECK(d2 == 0 && d1 == 0 && d0 == 0);

    // A MetaClass vtable can directly follow the instance's metaClass and
    // superClass globals in a modern cache. Do not mistake it for an instance.
    auto* words = reinterpret_cast<uint64_t*>(image.bytes.data() + 0x900);
    words[0] = 0x3500;
    words[1] = 0x3600;
    words[4] = 0x1100;
    auto* instance = reinterpret_cast<uint64_t*>(image.bytes.data() + 0xa00);
    instance[0] = 0x3500;
    instance[1] = 0x3600;
    instance[4] = 0x1120;
    image.macho.add_symbol(0x3110, "__ZTVN4Test9MetaClassE");
    uint64_t vtable = 0;
    CHECK(cpp_analyzer_detail::find_instance_vtable(image.macho, "Test", 0x3500, 0x3600, vtable));
    CHECK(vtable == 0x3210);

    // An exported instance vtable also wins when the neighboring candidate
    // has no name (the stripped MetaClass case).
    TestImage unnamed;
    memcpy(unnamed.bytes.data() + 0x900, image.bytes.data() + 0x900, 0x200);
    unnamed.macho.add_symbol(0x3210, "__ZTV4Test");
    CHECK(cpp_analyzer_detail::find_instance_vtable(unnamed.macho, "Test", 0x3500, 0x3600, vtable));
    CHECK(vtable == 0x3210);
    return 0;
}
