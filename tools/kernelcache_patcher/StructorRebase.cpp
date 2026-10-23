#include <StructorRebase.h>

#include <limits>

namespace patcher_detail {

bool rebase_structor_entries(uint64_t* entries, size_t count,
    uint64_t source_vmaddr, uint64_t source_vmsize,
    uint64_t target_vmaddr, size_t* invalid_index)
{
    if (invalid_index)
        *invalid_index = std::numeric_limits<size_t>::max();
    if ((!entries && count) || !source_vmsize)
        return count == 0;

    for (size_t i = 0; i < count; ++i) {
        const uint64_t address = entries[i];
        if (address < source_vmaddr || address - source_vmaddr >= source_vmsize
            || target_vmaddr > std::numeric_limits<uint64_t>::max() - (address - source_vmaddr)) {
            if (invalid_index)
                *invalid_index = i;
            return false;
        }
    }

    for (size_t i = 0; i < count; ++i)
        entries[i] = target_vmaddr + (entries[i] - source_vmaddr);
    return true;
}

} // namespace patcher_detail
