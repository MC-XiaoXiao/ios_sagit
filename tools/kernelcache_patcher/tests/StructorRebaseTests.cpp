#include <StructorRebase.h>

#include <iostream>
#include <limits>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition << '\n'; \
        return 1; \
    } \
} while (false)

int main()
{
    uint64_t init_entries[] = { 0x43f0, 0x4870, 0x4cc4, 0x5120 };
    size_t invalid_index = 0;
    CHECK(patcher_detail::rebase_structor_entries(init_entries, 4,
        0x4000, 0x4000, 0xfffffff006fc1000, &invalid_index));
    CHECK(invalid_index == std::numeric_limits<size_t>::max());
    CHECK(init_entries[0] == 0xfffffff006fc13f0);
    CHECK(init_entries[1] == 0xfffffff006fc1870);
    CHECK(init_entries[2] == 0xfffffff006fc1cc4);
    CHECK(init_entries[3] == 0xfffffff006fc2120);

    uint64_t invalid_entries[] = { 0x43f0, 0x3ff8 };
    CHECK(!patcher_detail::rebase_structor_entries(invalid_entries, 2,
        0x4000, 0x4000, 0xfffffff006fc1000, &invalid_index));
    CHECK(invalid_index == 1);
    CHECK(invalid_entries[0] == 0x43f0);
    CHECK(invalid_entries[1] == 0x3ff8);

    // Missing init or term sections are valid independently.
    CHECK(patcher_detail::rebase_structor_entries(nullptr, 0,
        0x4000, 0x4000, 0xfffffff006fc1000));
    return 0;
}
