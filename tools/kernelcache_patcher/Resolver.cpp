#include <Resolver.h>

#include <algorithm>
#include <cxxabi.h>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

static bool is_vtable_symbol(const char* name)
{
    return name && (!strncmp(name, "vtable for ", 11) || !strncmp(name, "__ZTV", 5));
}

static std::string display_name(const char* name)
{
    if (!name)
        return {};
    if (!strncmp(name, "__ZTV", 5)) {
        int status = 0;
        char* demangled = abi::__cxa_demangle(name + 1, nullptr, nullptr, &status);
        if (status == 0 && demangled) {
            std::string result(demangled);
            free(demangled);
            return result;
        }
        free(demangled);
    }
    return name;
}

} // namespace

LinkResolver::LinkResolver(const Macho& source, const Macho& target)
    : source_(source)
    , target_(target)
{
}

void LinkResolver::add_mapping(uint64_t source_address, uint64_t target_address)
{
    if (!source_address || !target_address || ambiguous_.count(source_address))
        return;
    auto inserted = mappings_.emplace(source_address, target_address);
    if (!inserted.second && inserted.first->second != target_address) {
        mappings_.erase(inserted.first);
        ambiguous_.insert(source_address);
    }
}

uint64_t LinkResolver::normalize_pointer(const Macho& image, uint64_t pointer) const
{
    if (image.is_mapped_address(pointer))
        return pointer;

    // arm64e data pointers may carry PAC/high metadata. This normalization is
    // used only when the resulting canonical address belongs to the image.
    uint64_t canonical = pointer & 0x0000ffffffffffffULL;
    if (canonical & (1ULL << 47))
        canonical |= 0xffff000000000000ULL;
    return image.is_mapped_address(canonical) ? canonical : pointer;
}

size_t LinkResolver::map_vtable(uint64_t source_vtable, uint64_t target_vtable, VtableMatch& match)
{
    std::vector<std::pair<uint64_t, uint64_t>> slots;
    VtableRejection rejection;
    rejection.name = match.name;
    rejection.source_address = source_vtable;
    rejection.target_address = target_vtable;
    constexpr size_t kHeaderSlots = 2;
    constexpr size_t kMaxSlots = 512;
    auto count_slots = [&](const Macho& image, uint64_t base) {
        size_t count = 0;
        uint64_t pointer = 0;
        while (count < kMaxSlots
            && image.read_u64_vm(base + (kHeaderSlots + count) * sizeof(uint64_t), pointer)
            && image.is_executable_address(normalize_pointer(image, pointer)))
            ++count;
        return count;
    };
    rejection.source_slots = count_slots(source_, source_vtable);
    rejection.target_slots = count_slots(target_, target_vtable);
    if (rejection.source_slots != rejection.target_slots) {
        rejection.reason = "slot-count-mismatch";
        rejection.slot = std::min(rejection.source_slots, rejection.target_slots);
        rejections_.push_back(std::move(rejection));
        return 0;
    }
    for (size_t i = 0; i < kMaxSlots; ++i) {
        uint64_t source_pointer = 0;
        uint64_t target_pointer = 0;
        uint64_t source_slot = source_vtable + (kHeaderSlots + i) * sizeof(uint64_t);
        uint64_t target_slot = target_vtable + (kHeaderSlots + i) * sizeof(uint64_t);
        if (!source_.read_u64_vm(source_slot, source_pointer)
            || !target_.read_u64_vm(target_slot, target_pointer)) {
            rejection.reason = "unreadable-slot";
            break;
        }

        source_pointer = normalize_pointer(source_, source_pointer);
        target_pointer = normalize_pointer(target_, target_pointer);
        rejection.slot = i;
        rejection.source_pointer = source_pointer;
        rejection.target_pointer = target_pointer;
        if (!source_.is_executable_address(source_pointer)
            || !target_.is_executable_address(target_pointer)) {
            rejection.reason = "non-executable-slot";
            break;
        }

        // A named slot is a strong layout anchor. If it moved, the two class
        // layouts differ and unnamed same-index matches are unsafe.
        Symbol* source_symbol = source_.find_symbol_by_addr(source_pointer);
        Symbol* expected_target = source_symbol && source_symbol->symbol_name
            ? target_.find_symbol_by_name(source_symbol->symbol_name) : nullptr;
        if (expected_target && expected_target->symbol_addr != target_pointer) {
            rejection.reason = "named-anchor-mismatch";
            rejection.source_symbol = source_symbol->symbol_name;
            rejection.expected_pointer = expected_target->symbol_addr;
            if (Symbol* actual = target_.find_symbol_by_addr(target_pointer))
                rejection.target_symbol = actual->symbol_name ? actual->symbol_name : "";
            rejections_.push_back(std::move(rejection));
            return 0;
        }
        slots.emplace_back(source_pointer, target_pointer);
    }

    if (slots.empty()) {
        rejections_.push_back(std::move(rejection));
        return 0;
    }
    add_mapping(source_vtable, target_vtable);
    add_mapping(source_vtable + sizeof(uint64_t), target_vtable + sizeof(uint64_t));
    add_mapping(source_vtable + 2 * sizeof(uint64_t), target_vtable + 2 * sizeof(uint64_t));
    for (const auto& slot : slots) {
        add_mapping(slot.first, slot.second);
        match.target_slots.push_back(slot.second);
    }
    return slots.size();
}

ResolverStats LinkResolver::build()
{
    mappings_.clear();
    ambiguous_.clear();
    vtables_.clear();
    rejections_.clear();
    stats_ = {};

    for (const Symbol* source_symbol : source_.symbol_list) {
        if (!source_symbol || !source_symbol->symbol_name || !source_symbol->symbol_addr)
            continue;
        Symbol* target_symbol = target_.find_symbol_by_name(source_symbol->symbol_name);
        if (!target_symbol || !target_symbol->symbol_addr)
            continue;

        ++stats_.named_symbols;
        if (is_vtable_symbol(source_symbol->symbol_name)) {
            VtableMatch match;
            match.name = display_name(source_symbol->symbol_name);
            match.source_address = source_symbol->symbol_addr;
            match.target_address = target_symbol->symbol_addr;
            size_t slots = map_vtable(source_symbol->symbol_addr, target_symbol->symbol_addr, match);
            if (slots) {
                vtables_.push_back(std::move(match));
                ++stats_.matched_vtables;
                stats_.matched_vtable_slots += slots;
            } else
                ++stats_.rejected_vtables;
        } else
            add_mapping(source_symbol->symbol_addr, target_symbol->symbol_addr);
    }

    stats_.ambiguous_addresses = ambiguous_.size();
    return stats_;
}

bool LinkResolver::resolve(uint64_t source_address, uint64_t& target_address) const
{
    source_address = normalize_pointer(source_, source_address);
    auto found = mappings_.find(source_address);
    if (found == mappings_.end())
        return false;
    target_address = found->second;
    return true;
}
