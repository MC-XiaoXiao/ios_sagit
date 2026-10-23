#pragma once

#include <KernelMacho.h>
#include <Resolver.h>

// Returns the number of unresolved external relocations. SIZE_MAX indicates
// an internal disassembler/assembler initialization failure.
size_t patch_kext_to_kernel(KernelMacho& y_kernel, KernelMacho& i_kernel, const LinkResolver* resolver = nullptr);
uint32_t patch_ios_kernel(KernelMacho& kernel, const char* patch_path);

namespace patcher_detail {
struct LocalRebaseSegment {
    uint64_t source, vm_size, file_size, destination;
    const uint8_t* input;
    uint8_t* output;
};
bool rebase_local_pointers(const relocation_info* entries, size_t count,
    const LocalRebaseSegment* segments, size_t segment_count, uint64_t image_base,
    size_t* invalid_index = nullptr);
bool kmod_identity_word(uint64_t kmod_address, uint64_t word_address);
bool rebase_uniform_page(uint64_t page, uint64_t source, uint64_t size,
    uint64_t destination, uint64_t& relocated_page);
bool test_adrp_consumers(const uint32_t* words, size_t count, size_t start,
    std::vector<size_t>& consumers, std::vector<size_t>* ambiguous = nullptr,
    const Macho* image = nullptr);
}
