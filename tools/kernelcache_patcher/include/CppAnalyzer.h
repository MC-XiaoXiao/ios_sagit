#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <Macho.h>

struct RecoveredClass {
    std::string name;
    std::string super_name;
    uint64_t meta_object = 0;
    uint64_t super_meta = 0;
    uint64_t vtable = 0;
    uint64_t meta_vtable = 0;
    uint64_t registration_call = 0;
    std::vector<uint64_t> registration_calls;
    uint32_t instance_size = 0;
    std::vector<uint64_t> slots;
};

struct RecoveredSymbol {
    std::string name;
    uint64_t address = 0;
};

struct CppAnalyzerStats {
    size_t registrations = 0;
    size_t classes_with_vtables = 0;
    size_t named_virtual_slots = 0;
    size_t kext_template_slots = 0;
    size_t structural_symbols = 0;
    size_t reference_symbols = 0;
    size_t injected_symbols = 0;
};

namespace cpp_analyzer_detail {

bool find_instance_vtable(const Macho& image, const std::string& class_name,
    uint64_t meta_object, uint64_t super_meta, uint64_t& vtable);

// Validates the single-constructor layout emitted by
// OSDefineMetaClassAndAbstractStructors and returns its destructor stubs.
bool find_abstract_structor_layout(const Macho& image, uint64_t constructor,
    uint64_t& d2, uint64_t& d1, uint64_t& d0);

} // namespace cpp_analyzer_detail

// Recovers OSMetaClass registrations and instance vtables from a stripped
// arm64 kernelcache. A same-version unstripped image is used only as a symbol
// and vtable-slot template; all recovered addresses belong to the stripped
// source image.
class CppClassAnalyzer {
public:
    CppClassAnalyzer(Macho& source, const Macho& symbol_template);

    void add_kext_template(Macho& kext);
    void add_symbol_reference(Macho& kext);
    CppAnalyzerStats recover();
    const std::vector<RecoveredClass>& classes() const { return classes_; }
    const std::vector<RecoveredSymbol>& symbols() const { return symbols_; }

private:
    Macho& source_;
    const Macho& template_;
    std::vector<Macho*> kext_templates_;
    std::vector<Macho*> symbol_references_;
    std::vector<RecoveredClass> classes_;
    std::vector<RecoveredSymbol> symbols_;

    void add_symbol(uint64_t address, const std::string& name, CppAnalyzerStats& stats);
};
