#pragma once

#include <cstddef>
#include <cstdint>

namespace patcher_detail {

// Rebases a table of C++ constructor/destructor function pointers from one
// __TEXT_EXEC mapping to another. Validation happens before modification so a
// malformed entry cannot leave the table partially rebased.
bool rebase_structor_entries(uint64_t* entries, size_t count,
    uint64_t source_vmaddr, uint64_t source_vmsize,
    uint64_t target_vmaddr, size_t* invalid_index = nullptr);

} // namespace patcher_detail
