#include <CppAnalyzer.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include <capstone/capstone.h>

namespace {

struct RegisterValue {
    bool valid = false;
    uint64_t value = 0;
};

static int register_index(unsigned reg)
{
    if (reg >= ARM64_REG_W0 && reg <= ARM64_REG_W30)
        return static_cast<int>(reg - ARM64_REG_W0);
    if (reg >= ARM64_REG_X0 && reg <= ARM64_REG_X28)
        return static_cast<int>(reg - ARM64_REG_X0);
    if (reg == ARM64_REG_X29)
        return 29;
    if (reg == ARM64_REG_X30)
        return 30;
    return -1;
}

static bool is_32_bit_register(unsigned reg)
{
    return reg >= ARM64_REG_W0 && reg <= ARM64_REG_W30;
}

static uint64_t canonical_pointer(const Macho& image, uint64_t pointer)
{
    if (image.is_mapped_address(pointer))
        return pointer;
    uint64_t canonical = pointer & 0x0000ffffffffffffULL;
    if (canonical & (1ULL << 47))
        canonical |= 0xffff000000000000ULL;
    return image.is_mapped_address(canonical) ? canonical : pointer;
}

static bool valid_class_name(const std::string& name)
{
    if (name.empty() || name.size() > 127)
        return false;
    if (!(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_'))
        return false;
    return std::all_of(name.begin() + 1, name.end(), [](char byte) {
        unsigned char value = static_cast<unsigned char>(byte);
        return std::isalnum(value) || byte == '_' || byte == ':';
    });
}

static bool simple_class_name(const std::string& name)
{
    return name.find(':') == std::string::npos && valid_class_name(name);
}

static std::string itanium_type(const std::string& name)
{
    return std::to_string(name.size()) + name;
}

static uint64_t branch_target(const Macho& image, uint64_t address)
{
    for (unsigned depth = 0; depth < 4; ++depth) {
        uint32_t instruction = 0;
        if (!image.read_u32_vm(address, instruction) || (instruction & 0x7c000000U) != 0x14000000U)
            break;
        int64_t displacement = static_cast<int64_t>(instruction & 0x03ffffffU);
        if (displacement & (1 << 25))
            displacement |= ~static_cast<int64_t>(0x03ffffffU);
        address = static_cast<uint64_t>(static_cast<int64_t>(address) + (displacement << 2));
    }
    return address;
}

static uint64_t direct_branch_target(uint64_t address, uint32_t instruction)
{
    int64_t displacement = static_cast<int64_t>(instruction & 0x03ffffffU);
    if (displacement & (1 << 25))
        displacement |= ~static_cast<int64_t>(0x03ffffffU);
    return static_cast<uint64_t>(static_cast<int64_t>(address) + (displacement << 2));
}

static bool looks_like_meta_call(const uint8_t* segment_bytes, uint64_t call_offset)
{
    constexpr size_t kWindowInstructions = 24;
    uint64_t available = call_offset / sizeof(uint32_t);
    size_t before = static_cast<size_t>(std::min<uint64_t>(available, kWindowInstructions));
    bool class_page = false;
    bool class_offset = false;
    bool instance_size = false;
    for (size_t index = 1; index <= before; ++index) {
        uint32_t instruction = 0;
        memcpy(&instruction, segment_bytes + call_offset - index * sizeof(uint32_t), sizeof(instruction));
        // ADRP X1, page
        if ((instruction & 0x9f00001fU) == 0x90000001U)
            class_page = true;
        // ADD X1, X1, #imm
        if ((instruction & 0xffc003ffU) == 0x91000021U)
            class_offset = true;
        // MOV W3, #imm (MOVZ alias). Class instance sizes are emitted this way.
        if ((instruction & 0x7f80001fU) == 0x52800003U)
            instance_size = true;
    }
    return class_page && class_offset && instance_size;
}

static void evaluate_instruction(const Macho& image, const cs_insn& instruction,
    std::array<RegisterValue, 31>& registers)
{
    if (!instruction.detail)
        return;
    const cs_arm64& arm = instruction.detail->arm64;
    const char* mnemonic = instruction.mnemonic;

    if ((!strcmp(mnemonic, "adrp") || !strcmp(mnemonic, "adr")) && arm.op_count >= 2
        && arm.operands[0].type == ARM64_OP_REG && arm.operands[1].type == ARM64_OP_IMM) {
        int destination = register_index(arm.operands[0].reg);
        if (destination >= 0)
            registers[destination] = { true, static_cast<uint64_t>(arm.operands[1].imm) };
        return;
    }

    if (!strcmp(mnemonic, "add") && arm.op_count >= 3
        && arm.operands[0].type == ARM64_OP_REG && arm.operands[1].type == ARM64_OP_REG
        && arm.operands[2].type == ARM64_OP_IMM) {
        int destination = register_index(arm.operands[0].reg);
        int base = register_index(arm.operands[1].reg);
        if (destination >= 0) {
            if (base >= 0 && registers[base].valid)
                registers[destination] = { true, registers[base].value + static_cast<uint64_t>(arm.operands[2].imm) };
            else
                registers[destination] = {};
        }
        return;
    }

    if ((!strcmp(mnemonic, "mov") || !strcmp(mnemonic, "movz")) && arm.op_count >= 2
        && arm.operands[0].type == ARM64_OP_REG) {
        int destination = register_index(arm.operands[0].reg);
        if (destination < 0)
            return;
        if (arm.operands[1].type == ARM64_OP_IMM) {
            uint64_t value = static_cast<uint64_t>(arm.operands[1].imm);
            if (is_32_bit_register(arm.operands[0].reg))
                value &= 0xffffffffU;
            registers[destination] = { true, value };
        } else if (arm.operands[1].type == ARM64_OP_REG) {
            int source = register_index(arm.operands[1].reg);
            registers[destination] = source >= 0 ? registers[source] : RegisterValue {};
        } else {
            registers[destination] = {};
        }
        return;
    }

    if (!strcmp(mnemonic, "ldr") && arm.op_count >= 2 && arm.operands[0].type == ARM64_OP_REG) {
        int destination = register_index(arm.operands[0].reg);
        if (destination < 0)
            return;
        uint64_t address = 0;
        bool have_address = false;
        if (arm.operands[1].type == ARM64_OP_MEM) {
            int base = register_index(arm.operands[1].mem.base);
            if (base >= 0 && registers[base].valid) {
                address = registers[base].value + arm.operands[1].mem.disp;
                have_address = true;
            }
        } else if (arm.operands[1].type == ARM64_OP_IMM) {
            address = static_cast<uint64_t>(arm.operands[1].imm);
            have_address = true;
        }
        uint64_t value = 0;
        if (have_address && image.read_u64_vm(address, value)) {
            if (is_32_bit_register(arm.operands[0].reg))
                value &= 0xffffffffU;
            registers[destination] = { true, canonical_pointer(image, value) };
        } else {
            registers[destination] = {};
        }
        return;
    }

    // Calls clobber volatile argument/temporary registers. The caller will
    // evaluate a fresh window before every candidate OSMetaClass call.
    if (!strcmp(mnemonic, "bl") || !strcmp(mnemonic, "blr")) {
        for (size_t i = 0; i <= 18; ++i)
            registers[i] = {};
    }
}

using MetaReferences = std::unordered_map<uint64_t, std::vector<uint64_t>>;

static MetaReferences index_meta_references(const Macho& image,
    const std::unordered_set<uint64_t>& meta_objects)
{
    MetaReferences references;
    references.reserve(meta_objects.size());
    for (const segment_command_64_t* segment : image.segments()) {
        if (!segment->filesize || (segment->initprot & 4))
            continue;
        uint64_t end = segment->vmaddr + segment->filesize;
        for (uint64_t address = segment->vmaddr; address + sizeof(uint64_t) <= end; address += sizeof(uint64_t)) {
            uint64_t value = 0;
            if (!image.read_u64_vm(address, value))
                continue;
            value = canonical_pointer(image, value);
            if (meta_objects.count(value))
                references[value].push_back(address);
        }
    }
    return references;
}

static std::vector<uint64_t> read_vtable_slots(const Macho& image, uint64_t vtable);

static bool find_instance_vtable(const Macho& image, const MetaReferences& references,
    const std::string& class_name, uint64_t meta_object, uint64_t super_meta, uint64_t& vtable)
{
    const std::string expected_name = "__ZTV" + itanium_type(class_name);
    // Native exports are stronger evidence than adjacency to metaClass and
    // superClass globals. Modern caches can place a MetaClass vtable there.
    if (Symbol* exported = image.find_symbol_by_name(expected_name)) {
        if (exported->symbol_addr && !read_vtable_slots(image, exported->symbol_addr).empty()) {
            vtable = exported->symbol_addr;
            return true;
        }
    }
    auto found = references.find(meta_object);
    if (found == references.end())
        return false;
    for (uint64_t address : found->second) {
        if (Symbol* label = image.find_symbol_by_addr(address + 16)) {
            const std::string name = label->symbol_name ? label->symbol_name : "";
            if ((name.rfind("__ZTV", 0) == 0 || name.rfind("vtable for ", 0) == 0)
                && name != expected_name && name != "vtable for " + class_name)
                continue;
        }
        uint64_t parent = 0, offset_to_top = 1, type_info = 1, first_method = 0;
        if (!image.read_u64_vm(address + 8, parent)
            || !image.read_u64_vm(address + 16, offset_to_top)
            || !image.read_u64_vm(address + 24, type_info)
            || !image.read_u64_vm(address + 32, first_method))
            continue;
        parent = canonical_pointer(image, parent);
        first_method = canonical_pointer(image, first_method);
        if (super_meta && parent != super_meta)
            continue;
        if (offset_to_top || type_info || !image.is_executable_address(first_method))
            continue;
        vtable = address + 16;
        return true;
    }
    return false;
}

static std::vector<uint64_t> read_vtable_slots(const Macho& image, uint64_t vtable)
{
    std::vector<uint64_t> slots;
    for (size_t i = 0; i < 512; ++i) {
        uint64_t pointer = 0;
        if (!image.read_u64_vm(vtable + (i + 2) * sizeof(uint64_t), pointer))
            break;
        pointer = canonical_pointer(image, pointer);
        if (!image.is_executable_address(pointer))
            break;
        slots.push_back(pointer);
    }
    return slots;
}

static bool is_return(uint32_t instruction)
{
    return instruction == 0xd65f03c0U || instruction == 0xd65f0fffU;
}

static bool is_unconditional_branch(uint32_t instruction)
{
    return (instruction & 0x7c000000U) == 0x14000000U;
}

static bool is_breakpoint(uint32_t instruction)
{
    // BRK #imm16.  Abstract OSMetaClass structors use BRK stubs for D1/D0
    // because instances of the abstract class cannot be destroyed directly.
    return (instruction & 0xffe0001fU) == 0xd4200000U;
}

static uint64_t find_function_return(const Macho& image, uint64_t start, size_t max_instructions = 96)
{
    for (size_t index = 0; index < max_instructions; ++index) {
        uint32_t instruction = 0;
        uint64_t address = start + index * sizeof(uint32_t);
        if (!image.read_u32_vm(address, instruction))
            return 0;
        if (is_return(instruction))
            return address;
    }
    return 0;
}

static uint64_t decode_adrp(uint64_t address, uint32_t instruction)
{
    int64_t immediate = static_cast<int64_t>(((instruction >> 5) & 0x7ffffU) << 2
        | ((instruction >> 29) & 0x3U));
    if (immediate & (1 << 20))
        immediate |= ~static_cast<int64_t>((1 << 21) - 1);
    return static_cast<uint64_t>(static_cast<int64_t>(address & ~0xfffULL) + (immediate << 12));
}

static uint64_t find_function_start(const Macho& image, uint64_t reference)
{
    uint64_t boundary = reference;
    for (size_t index = 1; index <= 96 && reference >= index * 4; ++index) {
        uint64_t address = reference - index * 4;
        uint32_t instruction = 0;
        if (!image.read_u32_vm(address, instruction))
            break;
        if (is_return(instruction)) {
            boundary = address + 4;
            break;
        }
    }
    // OSMetaClass's destructor is commonly a one-instruction tail branch
    // between the registration function and the instance constructor.
    uint32_t instruction = 0;
    while (boundary < reference && image.read_u32_vm(boundary, instruction)
        && is_unconditional_branch(instruction))
        boundary += 4;
    return boundary;
}

using MaterializationIndex = std::unordered_map<uint64_t, std::vector<uint64_t>>;

static MaterializationIndex index_vtable_materializations(const Macho& image,
    const std::unordered_set<uint64_t>& wanted)
{
    MaterializationIndex result;
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> seen;
    for (const segment_command_64_t* segment : image.segments()) {
        if (!(segment->initprot & 4) || segment->filesize < 12)
            continue;
        uint64_t end = segment->vmaddr + segment->filesize;
        for (uint64_t address = segment->vmaddr; address + 12 <= end; address += 4) {
            uint32_t adrp = 0, add_page = 0, add_offset = 0;
            if (!image.read_u32_vm(address, adrp)
                || !image.read_u32_vm(address + 4, add_page)
                || !image.read_u32_vm(address + 8, add_offset)
                || (adrp & 0x9f000000U) != 0x90000000U
                || (add_page & 0xffc00000U) != 0x91000000U
                || (add_offset & 0xffc00000U) != 0x91000000U)
                continue;
            unsigned destination = adrp & 0x1fU;
            if ((add_page & 0x1fU) != destination || ((add_page >> 5) & 0x1fU) != destination
                || (add_offset & 0x1fU) != destination || ((add_offset >> 5) & 0x1fU) != destination)
                continue;
            uint64_t value = decode_adrp(address, adrp);
            value += (static_cast<uint64_t>((add_page >> 10) & 0xfffU)
                << (((add_page >> 22) & 1U) ? 12 : 0));
            value += (static_cast<uint64_t>((add_offset >> 10) & 0xfffU)
                << (((add_offset >> 22) & 1U) ? 12 : 0));
            if (!wanted.count(value))
                continue;
            uint64_t function = find_function_start(image, address);
            if (seen[value].insert(function).second)
                result[value].push_back(function);
        }
    }
    for (auto& entry : result)
        std::sort(entry.second.begin(), entry.second.end());
    return result;
}

static std::vector<const Symbol*> symbols_at(const Macho& image, uint64_t address)
{
    std::vector<const Symbol*> result;
    for (const Symbol* symbol : image.symbol_list) {
        if (symbol && symbol->symbol_name && symbol->symbol_addr == address)
            result.push_back(symbol);
    }
    return result;
}

} // namespace

bool cpp_analyzer_detail::find_instance_vtable(const Macho& image, const std::string& class_name,
    uint64_t meta_object, uint64_t super_meta, uint64_t& vtable)
{
    const auto references = index_meta_references(image, { meta_object });
    return ::find_instance_vtable(image, references, class_name, meta_object, super_meta, vtable);
}

bool cpp_analyzer_detail::find_abstract_structor_layout(const Macho& image,
    uint64_t constructor, uint64_t& d2, uint64_t& d1, uint64_t& d0)
{
    d2 = d1 = d0 = 0;
    uint64_t constructor_end = find_function_return(image, constructor);
    if (!constructor_end)
        return false;

    d2 = constructor_end + 4;
    d1 = d2 + 4;
    d0 = d1 + 4;
    uint32_t d2_instruction = 0, d1_instruction = 0, d0_instruction = 0;
    if (!image.read_u32_vm(d2, d2_instruction)
        || !image.read_u32_vm(d1, d1_instruction)
        || !image.read_u32_vm(d0, d0_instruction)
        || !is_unconditional_branch(d2_instruction)
        || !is_breakpoint(d1_instruction)
        || !is_breakpoint(d0_instruction)) {
        d2 = d1 = d0 = 0;
        return false;
    }
    return true;
}

CppClassAnalyzer::CppClassAnalyzer(Macho& source, const Macho& symbol_template)
    : source_(source)
    , template_(symbol_template)
{
}

void CppClassAnalyzer::add_kext_template(Macho& kext)
{
    kext_templates_.push_back(&kext);
}

void CppClassAnalyzer::add_symbol_reference(Macho& kext)
{
    symbol_references_.push_back(&kext);
}

void CppClassAnalyzer::add_symbol(uint64_t address, const std::string& name, CppAnalyzerStats& stats)
{
    if (!address || name.empty())
        return;
    Symbol* existing = source_.find_symbol_by_name(name);
    // Never replace an existing export or signature with a positional guess.
    if (existing && existing->symbol_addr)
        return;
    source_.add_symbol(address, name, true);
    symbols_.push_back({ name, address });
    ++stats.injected_symbols;
}

CppAnalyzerStats CppClassAnalyzer::recover()
{
    classes_.clear();
    symbols_.clear();
    CppAnalyzerStats stats;

    std::unordered_set<uint64_t> meta_constructors;
    for (const char* name : { "__ZN11OSMetaClassC1EPKcPKS_j", "__ZN11OSMetaClassC2EPKcPKS_j" }) {
        Symbol* symbol = source_.find_symbol_by_name(name);
        if (symbol && symbol->symbol_addr)
            meta_constructors.insert(symbol->symbol_addr);
    }

    csh handle = 0;
    if (cs_open(CS_ARCH_ARM64, static_cast<cs_mode>(CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN), &handle) != CS_ERR_OK)
        return stats;
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);

    std::unordered_set<uint64_t> seen_meta;
    std::unordered_map<uint64_t, size_t> class_by_meta;
    for (const segment_command_64_t* segment : source_.segments()) {
        if (!(segment->initprot & 4) || !segment->filesize || segment->fileoff > source_.file_size
            || segment->filesize > source_.file_size - segment->fileoff)
            continue;
        const uint8_t* segment_bytes = reinterpret_cast<const uint8_t*>(source_.file_buf + segment->fileoff);
        for (uint64_t offset = 0; offset + sizeof(uint32_t) <= segment->filesize; offset += sizeof(uint32_t)) {
            uint32_t raw_instruction = 0;
            memcpy(&raw_instruction, segment_bytes + offset, sizeof(raw_instruction));
            if ((raw_instruction & 0xfc000000U) != 0x94000000U)
                continue;
            uint64_t call_address = segment->vmaddr + offset;
            uint64_t target = direct_branch_target(call_address, raw_instruction);
            uint64_t resolved_target = branch_target(source_, target);
            bool known_constructor = meta_constructors.count(target) || meta_constructors.count(resolved_target);
            if (!known_constructor && !looks_like_meta_call(segment_bytes, offset))
                continue;

            constexpr size_t kWindowInstructions = 24;
            uint64_t available_before = offset / sizeof(uint32_t);
            size_t before = static_cast<size_t>(std::min<uint64_t>(available_before, kWindowInstructions));
            uint64_t window_offset = offset - before * sizeof(uint32_t);
            size_t window_size = (before + 1) * sizeof(uint32_t);
            cs_insn* instructions = nullptr;
            size_t count = cs_disasm(handle, segment_bytes + window_offset, window_size,
                segment->vmaddr + window_offset, 0, &instructions);
            if (!count || strcmp(instructions[count - 1].mnemonic, "bl")) {
                if (instructions)
                    cs_free(instructions, count);
                continue;
            }
            std::array<RegisterValue, 31> registers {};
            for (size_t cursor = 0; cursor + 1 < count; ++cursor)
                evaluate_instruction(source_, instructions[cursor], registers);
            if (!registers[0].valid || !registers[1].valid || !registers[2].valid || !registers[3].valid) {
                cs_free(instructions, count);
                continue;
            }

            uint64_t meta_object = canonical_pointer(source_, registers[0].value);
            uint64_t super_meta = canonical_pointer(source_, registers[2].value);
            uint64_t instance_size = registers[3].value;
            std::string class_name;
            if (!source_.is_mapped_address(meta_object)
                || (super_meta && !source_.is_mapped_address(super_meta))
                || !instance_size || instance_size > 0x100000
                || !source_.read_cstring_vm(registers[1].value, class_name)
                || !valid_class_name(class_name)) {
                cs_free(instructions, count);
                continue;
            }

            auto duplicate = class_by_meta.find(meta_object);
            if (duplicate != class_by_meta.end()) {
                classes_[duplicate->second].registration_calls.push_back(call_address);
                cs_free(instructions, count);
                continue;
            }

            RecoveredClass recovered;
            recovered.name = class_name;
            recovered.meta_object = meta_object;
            recovered.super_meta = super_meta;
            recovered.instance_size = static_cast<uint32_t>(instance_size);
            recovered.registration_call = call_address;
            recovered.registration_calls.push_back(call_address);
            classes_.push_back(std::move(recovered));
            seen_meta.insert(meta_object);
            class_by_meta.emplace(meta_object, classes_.size() - 1);
            cs_free(instructions, count);
        }
    }
    cs_close(&handle);

    MetaReferences meta_references = index_meta_references(source_, seen_meta);
    for (RecoveredClass& recovered : classes_) {
        find_instance_vtable(source_, meta_references, recovered.name, recovered.meta_object,
            recovered.super_meta, recovered.vtable);
        if (recovered.vtable)
            recovered.slots = read_vtable_slots(source_, recovered.vtable);
    }


    // Find all functions that materialize the instance vtable address point.
    // Concrete macro-generated classes normally have adjacent C2 and C1
    // constructors followed by D2/D1/D0.  Abstract OSMetaClass classes emit
    // only C2, followed by a D2 tail branch and BRK stubs for D1/D0.  Validate
    // both layouts so an abstract base constructor can be recovered without a
    // manual symbol table.
    std::unordered_set<uint64_t> vtable_address_points;
    for (const RecoveredClass& recovered : classes_) {
        if (recovered.vtable)
            vtable_address_points.insert(recovered.vtable + 16);
    }
    MaterializationIndex materializations = index_vtable_materializations(source_, vtable_address_points);
    for (RecoveredClass& recovered : classes_) {
        if (!recovered.vtable || !simple_class_name(recovered.name))
            continue;
        auto found_candidates = materializations.find(recovered.vtable + 16);
        if (found_candidates == materializations.end())
            continue;
        const std::vector<uint64_t>& candidates = found_candidates->second;
        std::string type = itanium_type(recovered.name);

        if (candidates.size() == 1) {
            uint64_t c2 = candidates.front();
            uint64_t d2 = 0, d1 = 0, d0 = 0;
            if (cpp_analyzer_detail::find_abstract_structor_layout(source_, c2, d2, d1, d0)) {
                for (const auto& symbol : std::array<std::pair<uint64_t, std::string>, 4> {{
                         { c2, "__ZN" + type + "C2EPK11OSMetaClass" },
                         { d2, "__ZN" + type + "D2Ev" },
                         { d1, "__ZN" + type + "D1Ev" },
                         { d0, "__ZN" + type + "D0Ev" },
                     }}) {
                    size_t before = stats.injected_symbols;
                    add_symbol(symbol.first, symbol.second, stats);
                    if (stats.injected_symbols != before)
                        ++stats.structural_symbols;
                }
                continue;
            }
        }

        for (size_t index = 0; index + 1 < candidates.size(); ++index) {
            uint64_t c2 = candidates[index];
            uint64_t c2_end = find_function_return(source_, c2);
            uint64_t c1 = candidates[index + 1];
            uint64_t c1_end = find_function_return(source_, c1);
            uint32_t d2_instruction = 0, d1_instruction = 0;
            uint64_t d2 = c1_end + 4;
            uint64_t d1 = d2 + 4;
            if (!c2_end || !c1_end || c2_end + 4 != c1
                || !source_.read_u32_vm(d2, d2_instruction)
                || !source_.read_u32_vm(d1, d1_instruction)
                || !is_unconditional_branch(d2_instruction)
                || !is_unconditional_branch(d1_instruction))
                continue;
            for (const auto& symbol : std::array<std::pair<uint64_t, std::string>, 5> {{
                     { c2, "__ZN" + type + "C2EPK11OSMetaClass" },
                     { c1, "__ZN" + type + "C1EPK11OSMetaClass" },
                     { d2, "__ZN" + type + "D2Ev" },
                     { d1, "__ZN" + type + "D1Ev" },
                     { d1 + 4, "__ZN" + type + "D0Ev" },
                 }}) {
                size_t before = stats.injected_symbols;
                add_symbol(symbol.first, symbol.second, stats);
                if (stats.injected_symbols != before)
                    ++stats.structural_symbols;
            }
            break;
        }
    }

    std::unordered_map<uint64_t, std::string> names_by_meta;
    for (const RecoveredClass& recovered : classes_)
        names_by_meta.emplace(recovered.meta_object, recovered.name);
    for (RecoveredClass& recovered : classes_) {
        auto parent = names_by_meta.find(recovered.super_meta);
        if (parent != names_by_meta.end())
            recovered.super_name = parent->second;
    }

    stats.registrations = classes_.size();
    for (RecoveredClass& recovered : classes_) {
        if (!recovered.vtable || !simple_class_name(recovered.name))
            continue;
        ++stats.classes_with_vtables;
        std::string type = itanium_type(recovered.name);
        std::string vtable_name = "__ZTV" + type;
        add_symbol(recovered.vtable, vtable_name, stats);
        add_symbol(recovered.vtable - 16, "__ZN" + type + "9metaClassE", stats);
        add_symbol(recovered.vtable - 8, "__ZN" + type + "10superClassE", stats);
        add_symbol(recovered.meta_object, "__ZN" + type + "10gMetaClassE", stats);

        Symbol* target_vtable_symbol = template_.find_symbol_by_name(vtable_name);
        if (!target_vtable_symbol || !target_vtable_symbol->symbol_addr)
            continue;
        std::vector<uint64_t> target_slots = read_vtable_slots(template_, target_vtable_symbol->symbol_addr);
        // A different virtual count is direct layout evidence. Do not invent
        // method names by index across that boundary (e.g. CONFIG_DTRACE).
        if (recovered.slots.size() != target_slots.size())
            continue;
        size_t common = std::min(recovered.slots.size(), target_slots.size());
        for (size_t slot = 0; slot < common; ++slot) {
            for (const Symbol* symbol : symbols_at(template_, target_slots[slot])) {
                add_symbol(recovered.slots[slot], symbol->symbol_name, stats);
                ++stats.named_virtual_slots;
            }
        }
    }


    // A locally-built child Kext carries external relocations for every
    // inherited virtual method in its own vtable. Use those relocations as a
    // slot-name template for a stripped parent class (for example BCM2837IO ->
    // AppleARMIO), even when that parent does not exist in the local kernel.
    std::unordered_map<std::string, RecoveredClass*> classes_by_gmeta;
    for (RecoveredClass& recovered : classes_) {
        if (simple_class_name(recovered.name)) {
            std::string type = itanium_type(recovered.name);
            std::string name = "__ZN" + type + "10gMetaClassE";
            auto existing = classes_by_gmeta.find(name);
            if (existing == classes_by_gmeta.end() || (existing->second->slots.empty() && !recovered.slots.empty()))
                classes_by_gmeta[name] = &recovered;
        }
    }
    for (Macho* kext : kext_templates_) {
        if (!kext || !kext->header)
            continue;
        auto* dysymtab = reinterpret_cast<dysymtab_command*>(kext->find_command(LC_DYSYMTAB));
        if (!dysymtab || dysymtab->extreloff > kext->file_size
            || static_cast<uint64_t>(dysymtab->nextrel) * sizeof(relocation_info) > kext->file_size - dysymtab->extreloff)
            continue;
        const auto* relocations = reinterpret_cast<const relocation_info*>(kext->file_buf + dysymtab->extreloff);

        for (const Symbol* child_vtable : kext->symbol_list) {
            if (!child_vtable || !child_vtable->symbol_name || !child_vtable->symbol_addr
                || strncmp(child_vtable->symbol_name, "__ZTV", 5)
                || strstr(child_vtable->symbol_name, "9MetaClassE"))
                continue;

            const char* parent_gmeta_name = nullptr;
            for (uint32_t index = 0; index < dysymtab->nextrel; ++index) {
                if (!relocations[index].r_extern
                    || static_cast<uint64_t>(static_cast<uint32_t>(relocations[index].r_address)) != child_vtable->symbol_addr - 8
                    || relocations[index].r_symbolnum >= kext->symbol_count)
                    continue;
                const Symbol* symbol = kext->symbol_list[relocations[index].r_symbolnum];
                parent_gmeta_name = symbol ? symbol->symbol_name : nullptr;
                break;
            }
            if (!parent_gmeta_name)
                continue;
            auto parent = classes_by_gmeta.find(parent_gmeta_name);
            if (parent == classes_by_gmeta.end() || parent->second->slots.empty())
                continue;

            uint64_t vtable_end = child_vtable->symbol_addr + (parent->second->slots.size() + 2) * sizeof(uint64_t);
            for (const Symbol* next : kext->symbol_list) {
                if (next && next->symbol_addr > child_vtable->symbol_addr
                    && next->symbol_addr < vtable_end && next->symbol_name
                    && !strncmp(next->symbol_name, "__ZTV", 5))
                    vtable_end = next->symbol_addr;
            }
            uint64_t first_slot = child_vtable->symbol_addr + 2 * sizeof(uint64_t);
            for (uint32_t index = 0; index < dysymtab->nextrel; ++index) {
                if (!relocations[index].r_extern || relocations[index].r_symbolnum >= kext->symbol_count)
                    continue;
                uint64_t address = static_cast<uint32_t>(relocations[index].r_address);
                if (address < first_slot || address >= vtable_end || (address - first_slot) % sizeof(uint64_t))
                    continue;
                size_t slot = static_cast<size_t>((address - first_slot) / sizeof(uint64_t));
                if (slot >= parent->second->slots.size())
                    continue;
                const Symbol* symbol = kext->symbol_list[relocations[index].r_symbolnum];
                if (!symbol || !symbol->symbol_name)
                    continue;
                add_symbol(parent->second->slots[slot], symbol->symbol_name, stats);
                ++stats.kext_template_slots;
            }
        }
    }


    // Only transfer reference symbols that are actually undefined by a custom
    // Kext.  A nearby symbol is accepted when it sits within 0x40 bytes of a
    // same-name class gMetaClass in the reference image.  This recovers small
    // class-owned globals (AppleARMFunctionNew/Call are at -0x10/-0x8) while
    // never copying an address from a different kernel/Kext build.
    std::unordered_set<std::string> required_symbols;
    for (Macho* kext : kext_templates_) {
        if (!kext)
            continue;
        auto* dysymtab = reinterpret_cast<dysymtab_command*>(kext->find_command(LC_DYSYMTAB));
        if (!dysymtab)
            continue;
        uint64_t end = static_cast<uint64_t>(dysymtab->iundefsym) + dysymtab->nundefsym;
        end = std::min<uint64_t>(end, kext->symbol_list.size());
        for (uint64_t index = dysymtab->iundefsym; index < end; ++index) {
            const Symbol* symbol = kext->symbol_list[index];
            if (symbol && symbol->symbol_name)
                required_symbols.insert(symbol->symbol_name);
        }
    }
    for (Macho* reference : symbol_references_) {
        if (!reference)
            continue;

        std::unordered_set<uint64_t> reference_address_points;
        std::unordered_map<std::string, uint64_t> reference_vtables;
        for (const RecoveredClass& recovered : classes_) {
            if (!recovered.vtable || !simple_class_name(recovered.name))
                continue;
            std::string vtable_name = "__ZTV" + itanium_type(recovered.name);
            Symbol* vtable = reference->find_symbol_by_name(vtable_name);
            if (!vtable || !vtable->symbol_addr)
                continue;
            reference_vtables.emplace(recovered.name, vtable->symbol_addr);
            reference_address_points.insert(vtable->symbol_addr + 16);
        }
        MaterializationIndex reference_materializations =
            index_vtable_materializations(*reference, reference_address_points);

        for (const Symbol* symbol : reference->symbol_list) {
            if (!symbol || !symbol->symbol_name || !symbol->symbol_addr
                || !required_symbols.count(symbol->symbol_name))
                continue;
            const std::string symbol_name = symbol->symbol_name;
            for (RecoveredClass& recovered : classes_) {
                if (!recovered.vtable || !simple_class_name(recovered.name))
                    continue;

                // Map a required non-virtual function by its adjacency to a
                // class constructor that writes the vtable.  Use an index from
                // the end of the constructor list so added/removed earlier
                // constructors do not shift the match.
                auto reference_vtable = reference_vtables.find(recovered.name);
                auto source_candidates = materializations.find(recovered.vtable + 16);
                if (reference_vtable != reference_vtables.end()
                    && source_candidates != materializations.end()) {
                    auto reference_candidates = reference_materializations.find(reference_vtable->second + 16);
                    if (reference_candidates != reference_materializations.end()) {
                        const std::vector<uint64_t>& candidates = reference_candidates->second;
                        const std::vector<uint64_t>& source_list = source_candidates->second;

                        // Constructors are themselves vtable materializers.  In
                        // particular, OSDefineMetaClassAndAbstractStructors emits
                        // one C2 and no C1.  Map a named reference constructor
                        // directly to the corresponding stripped candidate when
                        // both images have the same unambiguous layout.
                        std::string type = itanium_type(recovered.name);
                        std::string c1 = "__ZN" + type + "C1EPK11OSMetaClass";
                        std::string c2 = "__ZN" + type + "C2EPK11OSMetaClass";
                        if (source_list.size() == candidates.size()
                            && (symbol_name == c1 || symbol_name == c2)) {
                            auto candidate = std::find(candidates.begin(), candidates.end(), symbol->symbol_addr);
                            if (candidate != candidates.end()) {
                                size_t index = static_cast<size_t>(candidate - candidates.begin());
                                size_t before = stats.injected_symbols;
                                add_symbol(source_list[index], symbol_name, stats);
                                if (stats.injected_symbols != before)
                                    ++stats.reference_symbols;
                            }
                        }

                        if (source_.find_symbol_by_name(symbol_name))
                            break;

                        for (size_t index = 0; index < candidates.size(); ++index) {
                            uint64_t candidate_end = find_function_return(*reference, candidates[index]);
                            if (!candidate_end || candidate_end + 4 != symbol->symbol_addr)
                                continue;
                            size_t reverse_index = candidates.size() - 1 - index;
                            if (reverse_index >= source_list.size())
                                break;
                            uint64_t source_function = source_list[source_list.size() - 1 - reverse_index];
                            uint64_t source_end = find_function_return(source_, source_function);
                            if (!source_end)
                                break;
                            size_t before = stats.injected_symbols;
                            add_symbol(source_end + 4, symbol->symbol_name, stats);
                            if (stats.injected_symbols != before)
                                ++stats.reference_symbols;
                            break;
                        }
                    }
                }

                if (source_.find_symbol_by_name(symbol->symbol_name))
                    break;
                std::string gmeta = "__ZN" + itanium_type(recovered.name) + "10gMetaClassE";
                Symbol* reference_gmeta = reference->find_symbol_by_name(gmeta);
                if (!reference_gmeta || !reference_gmeta->symbol_addr)
                    continue;
                int64_t delta = static_cast<int64_t>(symbol->symbol_addr)
                    - static_cast<int64_t>(reference_gmeta->symbol_addr);
                if (delta < -0x40 || delta > 0x40)
                    continue;
                size_t before = stats.injected_symbols;
                add_symbol(static_cast<uint64_t>(static_cast<int64_t>(recovered.meta_object) + delta),
                    symbol->symbol_name, stats);
                if (stats.injected_symbols != before)
                    ++stats.reference_symbols;
                break;
            }
        }
    }
    return stats;
}
