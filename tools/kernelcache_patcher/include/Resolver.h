#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

#include <Macho.h>

struct ResolverStats {
    size_t named_symbols = 0;
    size_t matched_vtables = 0;
    size_t rejected_vtables = 0;
    size_t matched_vtable_slots = 0;
    size_t ambiguous_addresses = 0;
};

struct VtableMatch {
    std::string name;
    uint64_t source_address = 0;
    uint64_t target_address = 0;
    std::vector<uint64_t> target_slots;
};

struct VtableRejection {
    std::string name;
    std::string reason;
    size_t slot = 0; // Zero-based function slot, excluding the two ABI header words.
    size_t source_slots = 0;
    size_t target_slots = 0;
    uint64_t source_address = 0;
    uint64_t target_address = 0;
    uint64_t source_pointer = 0;
    uint64_t target_pointer = 0;
    uint64_t expected_pointer = 0;
    std::string source_symbol;
    std::string target_symbol;
};

// Resolves addresses in a stripped release kernel to the corresponding
// addresses in the locally-built kernel. Named symbols are used when present;
// C++ virtual functions additionally use same-class/same-slot vtable matching.
class LinkResolver {
public:
    LinkResolver(const Macho& source, const Macho& target);

    ResolverStats build();
    bool resolve(uint64_t source_address, uint64_t& target_address) const;
    const ResolverStats& stats() const { return stats_; }
    const std::vector<VtableMatch>& vtables() const { return vtables_; }
    const std::vector<VtableRejection>& rejections() const { return rejections_; }

private:
    const Macho& source_;
    const Macho& target_;
    std::unordered_map<uint64_t, uint64_t> mappings_;
    std::unordered_set<uint64_t> ambiguous_;
    ResolverStats stats_;
    std::vector<VtableMatch> vtables_;
    std::vector<VtableRejection> rejections_;

    void add_mapping(uint64_t source_address, uint64_t target_address);
    uint64_t normalize_pointer(const Macho& image, uint64_t pointer) const;
    size_t map_vtable(uint64_t source_vtable, uint64_t target_vtable, VtableMatch& match);
};
