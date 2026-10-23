
#include <map>
#include <set>
#include <unordered_set>
#include <cinttypes>
#include <limits>
#include <array>

#include <capstone/capstone.h>
#include <keystone/keystone.h>
#include <tinyxml2.h>

#include <Patcher.h>
#include <Plist.h>
#include <StructorRebase.h>
#include <CodeData.h>

bool patcher_detail::rebase_local_pointers(const relocation_info* entries, size_t count,
    const LocalRebaseSegment* segments, size_t segment_count, uint64_t image_base,
    size_t* invalid_index)
{
    if (invalid_index) *invalid_index = SIZE_MAX;
    if ((count && !entries) || (segment_count && !segments)) return false;
    struct Patch { uint8_t* site; uint64_t value; };
    std::vector<Patch> patches;
    for (size_t i = 0; i < count; ++i) {
        const auto& r = entries[i];
        auto fail = [&]() { if (invalid_index) *invalid_index = i; return false; };
        if (r.r_address < 0 || r.r_extern || r.r_pcrel || r.r_type != 0 || r.r_length != 3)
            return fail();
        if (!r.r_symbolnum) continue; // R_ABS is already an absolute value.
        if (uint64_t(r.r_address) > UINT64_MAX - image_base) return fail();
        const uint64_t address = image_base + uint64_t(r.r_address);
        const LocalRebaseSegment* site = nullptr;
        for (size_t n = 0; n < segment_count; ++n) {
            const auto& s = segments[n];
            if (address >= s.source && address - s.source <= s.file_size &&
                sizeof(uint64_t) <= s.file_size - (address - s.source)) { site = &s; break; }
        }
        if (!site || !site->input || !site->output) return fail();
        uint64_t value;
        memcpy(&value, site->input + address - site->source, sizeof(value));
        const LocalRebaseSegment* target = nullptr;
        for (size_t n = 0; n < segment_count; ++n) {
            const auto& s = segments[n];
            if (value >= s.source && value - s.source < s.vm_size) { target = &s; break; }
        }
        if (!target || value - target->source > UINT64_MAX - target->destination) return fail();
        patches.push_back({site->output + address - site->source,
            target->destination + value - target->source});
    }
    // Read the original file, not the heuristic pass's already rewritten data.
    // Explicit relocation sites distinguish pointers from scalar constants.
    for (const auto& p : patches) memcpy(p.site, &p.value, sizeof(p.value));
    return true;
}

// kmod_info name/version are fixed character arrays. Their padded suffix can
// be a small integer in a local Kext address range (e.g. "MI" = 0x494d).
// Never let the legacy data-pointer heuristic relocate these identity bytes.
bool patcher_detail::kmod_identity_word(uint64_t kmod_address, uint64_t word_address)
{
    if (!kmod_address || word_address < kmod_address) return false;
    const uint64_t offset = word_address - kmod_address;
    return offset < 144 && offset + sizeof(uint64_t) > 16;
}

bool patcher_detail::rebase_uniform_page(uint64_t page, uint64_t source, uint64_t size,
    uint64_t destination, uint64_t& relocated_page)
{
    // A complete page inside one affine mapping retains every PAGEOFF12
    // value when the translation is page aligned. This proof does not depend
    // on resolving an obfuscated branch that carries the ADRP register.
    constexpr uint64_t page_size = 4096;
    if ((page & (page_size - 1)) || page < source || size < page_size
        || page - source > size - page_size)
        return false;
    const uint64_t offset = page - source;
    if (destination > UINT64_MAX - offset
        || destination + offset > UINT64_MAX - (page_size - 1))
        return false;
    const uint64_t target = destination + offset;
    if (!target || (target & (page_size - 1)))
        return false;
    relocated_page = target;
    return true;
}

static bool resolve_kernel_reference(uint64_t source_address, KernelMacho& source_kernel,
    KernelMacho& target_kernel, const LinkResolver* resolver, uint64_t& target_address)
{
    if (resolver && resolver->resolve(source_address, target_address))
        return true;

    Symbol* source_symbol = source_kernel.find_symbol_by_addr(source_address);
    if (!source_symbol || !source_symbol->symbol_name)
        return false;
    Symbol* target_symbol = target_kernel.find_symbol_by_name(source_symbol->symbol_name);
    if (!target_symbol || !target_symbol->symbol_addr)
        return false;
    target_address = target_symbol->symbol_addr;
    return true;
}

static bool rebase_dependency_reference(Kext* kext, uint64_t source_address,
    uint64_t new_prelink_text_base, uint64_t new_prelink_text_exec_base,
    uint64_t new_prelink_data_base, uint64_t new_prelink_data_const_base,
    uint64_t& target_address)
{
    if (!kext)
        return false;
    for (Kext* dependency : kext->depends) {
        if (!dependency || dependency->is_from_file || !dependency->exec_macho)
            continue;
        struct SegmentBase { const char* name; uint64_t base; uint64_t offset; } segments[] = {
            { "__TEXT", new_prelink_text_base, dependency->text_off },
            { "__TEXT_EXEC", new_prelink_text_exec_base, dependency->text_exec_off },
            { "__DATA", new_prelink_data_base, dependency->data_off },
            { "__DATA_CONST", new_prelink_data_const_base, dependency->data_const_off },
        };
        for (const SegmentBase& entry : segments) {
            auto* segment = static_cast<segment_command_64_t*>(dependency->exec_macho->find_segment(entry.name));
            if (dependency->exec_macho->addr_in_segment(segment, source_address)) {
                target_address = source_address - segment->vmaddr + entry.base + entry.offset;
                return true;
            }
        }
    }
    return false;
}

static bool rebase_structor_section(Kext* kext, const section_64_t* section,
    const char* section_name, const segment_command_64_t* data_const_segment,
    const segment_command_64_t* text_exec_segment, void* prelink_data_const_buf,
    uint64_t new_prelink_text_exec_base)
{
    if (!section)
        return true;
    if (!kext || !data_const_segment || !text_exec_segment || !prelink_data_const_buf) {
        printf("Cannot rebase %s for %s: required segment is missing\n", section_name,
            kext && kext->kext_id ? kext->kext_id : "<unknown>");
        return false;
    }
    if (section->addr < data_const_segment->vmaddr
        || section->addr - data_const_segment->vmaddr > data_const_segment->filesize
        || section->size > data_const_segment->filesize - (section->addr - data_const_segment->vmaddr)
        || section->size % sizeof(uint64_t)) {
        printf("Cannot rebase %s for %s: malformed section range\n", section_name,
            kext->kext_id ? kext->kext_id : "<unknown>");
        return false;
    }

    auto* entries = reinterpret_cast<uint64_t*>(
        reinterpret_cast<uint64_t>(prelink_data_const_buf) + kext->data_const_off
        + section->addr - data_const_segment->vmaddr);
    size_t invalid_index = 0;
    if (!patcher_detail::rebase_structor_entries(entries,
            section->size / sizeof(uint64_t), text_exec_segment->vmaddr,
            text_exec_segment->vmsize,
            new_prelink_text_exec_base + kext->text_exec_off, &invalid_index)) {
        printf("Cannot rebase %s[%zu] for %s: address 0x%" PRIx64
               " is outside __TEXT_EXEC\n",
            section_name, invalid_index, kext->kext_id ? kext->kext_id : "<unknown>",
            entries[invalid_index]);
        return false;
    }
    return true;
}

// 得到单条指令的立即数
static uint64_t getSingleIMM(csh handle, const cs_insn* insn)
{
    int acount = cs_op_count(handle, insn, ARM64_OP_IMM);
    if (acount) {
        if (acount > 1)
            printf("getSingleIMM Immediate number more than one\n");
        for (int i = 1; i < acount + 1; ++i) {
            int index = cs_op_index(handle, insn, ARM64_OP_IMM, i);
            if (index >= 0)
                return insn->detail->arm64.operands[index].imm;
        }
    }
    return 0;
}

static void copy_segment_from_kext(Kext* kext, KernelMacho& i_kernel, const char* kext_segname, const char* kernel_segname, void* buf, uint64_t& off, uint64_t align)
{
    uint64_t copy_des = 0;
    uint64_t copy_src = 0;
    uint64_t copy_size = 0;

    segment_command_64_t* i_seg = (segment_command_64_t*)(i_kernel.find_segment(kernel_segname));

    segment_command_64_t* text_exec_seg = (segment_command_64_t*)(kext->exec_macho->find_segment(kext_segname));
    if (text_exec_seg) {
        if (kext->is_from_file) {
            copy_src = (uint64_t)kext->exec_macho->file_buf + text_exec_seg->fileoff;
        } else {
            copy_src = text_exec_seg->vmaddr;
            copy_src -= i_seg->vmaddr;
            copy_src += (uint64_t)i_kernel.file_buf + i_seg->fileoff;
        }

        copy_des = (uint64_t)buf + off;
        copy_size = text_exec_seg->filesize;
        // printf("Copy 0x%016llx -> 0x%016llx(0x%08x)\n", copy_src, off, copy_size);
        memset((void*)copy_des, 0, copy_size);
        memcpy((char*)copy_des, (char*)copy_src, copy_size);
        copy_size = ALIGN_UP(text_exec_seg->vmsize, align);
        off += copy_size;
    }
}

static void patch_seg_fileoff(segment_command_64_t* seg, uint64_t fileoff, uint64_t size)
{
    section_64_t* sect = (section_64_t*)((uint64_t)seg + sizeof(segment_command_64_t));
    for (uint32_t i = 0; i < seg->nsects; i++) {
        const uint32_t type = sect->flags & 0xff;
        if (type == 1 || type == 0xc || type == 0x12)
            sect->offset = 0; // Zerofill has no file bytes; never underflow 0-old_fileoff.
        else
            sect->offset = sect->offset - seg->fileoff + fileoff;
        sect = (section_64_t*)(sect + 1);
    }

    seg->fileoff = fileoff;
    seg->filesize = size;
}

static void patch_seg_vmbase(segment_command_64_t* seg, uint64_t vmaddr, uint64_t size)
{
    uint64_t old_vmbase = seg->vmaddr;
    seg->vmaddr = vmaddr;
    seg->vmsize = size;

    section_64_t* sect = (section_64_t*)((uint64_t)seg + sizeof(segment_command_64_t));
    for (uint32_t i = 0; i < seg->nsects; i++) {
        sect->addr = vmaddr + sect->addr - old_vmbase;
        sect = (section_64_t*)(sect + 1);
    }
}

static void patch_seg_base(segment_command_64_t* seg, uint64_t fileoff, uint64_t vmaddr, uint64_t size)
{
    patch_seg_vmbase(seg, vmaddr, size);
    seg->fileoff = fileoff;
    seg->filesize = size;
    // XNU's empty prelink placeholders have no permissions. The packed
    // segments must advertise the permissions needed by their children.
    const int protection = !strncmp(seg->segname, "__PLK_TEXT_EXEC", 16) ? 5
        : !strncmp(seg->segname, "__PRELINK_DATA", 16) ? 3 : 1;
    seg->maxprot = seg->initprot = protection;

    section_64_t* sect = (section_64_t*)((uint64_t)seg + sizeof(segment_command_64_t));
    sect->offset = fileoff;
    sect->size = size;
}

// Clang's bounded signed-offset switch: cmp/b.hi/adr/ldrsw/add/br.
// Resolve every edge instead of treating the first case as fallthrough, which
// can kill a live ADRP register before a different case uses it (Sandbox).
static bool switch_targets(csh handle, const cs_insn* insn, size_t end,
    size_t branch, std::vector<size_t>& targets)
{
    if (branch < 5) return false;
    const auto& cmp = insn[branch - 5];
    const auto& hi = insn[branch - 4];
    const auto& adr = insn[branch - 3];
    const auto& load = insn[branch - 2];
    const auto& add = insn[branch - 1];
    const auto& br = insn[branch];
    if (cmp.id != ARM64_INS_CMP || strcmp(hi.mnemonic, "b.hi")
        || adr.id != ARM64_INS_ADR || load.id != ARM64_INS_LDRSW
        || add.id != ARM64_INS_ADD || br.id != ARM64_INS_BR
        || !cmp.detail || !hi.detail || !adr.detail || !load.detail || !add.detail || !br.detail)
        return false;
    const auto& c = cmp.detail->arm64;
    const auto& a = adr.detail->arm64;
    const auto& l = load.detail->arm64;
    const auto& sum = add.detail->arm64;
    const auto& b = br.detail->arm64;
    if (c.op_count != 2 || c.operands[1].type != ARM64_OP_IMM
        || c.operands[1].imm < 0 || c.operands[1].imm > 255
        || a.op_count != 2 || a.operands[1].type != ARM64_OP_IMM
        || l.op_count != 2 || l.operands[1].type != ARM64_OP_MEM
        || l.operands[1].shift.type != ARM64_SFT_LSL || l.operands[1].shift.value != 2
        || sum.op_count != 3 || sum.operands[2].type != ARM64_OP_REG
        || b.op_count != 1 || b.operands[0].reg != a.operands[0].reg
        || sum.operands[0].reg != a.operands[0].reg
        || sum.operands[1].reg != a.operands[0].reg
        || sum.operands[2].reg != l.operands[0].reg
        || (c.operands[0].reg != l.operands[1].mem.index
            && c.operands[0].reg + 31 != l.operands[1].mem.index))
        return false;
    const auto base = l.operands[1].mem.base;
    // Only accept a preserved table base defined by adjacent ADRP/ADD.
    if (base < ARM64_REG_X19 || base > ARM64_REG_X28) return false;
    uint64_t table = 0;
    for (size_t i = branch - 3, searched = 0; i && searched < 64; ++searched) {
        --i;
        cs_regs reads, writes; uint8_t nr = 0, nw = 0;
        if (cs_regs_access(handle, &insn[i], reads, &nr, writes, &nw)) return false;
        bool defines = false;
        for (unsigned j = 0; j < nw; ++j)
            defines |= writes[j] == base || writes[j] + 31 == base;
        if (!defines) continue;
        if (!i || insn[i].id != ARM64_INS_ADD || insn[i-1].id != ARM64_INS_ADRP)
            return false;
        const auto& d = insn[i].detail->arm64;
        const auto& p = insn[i-1].detail->arm64;
        if (d.op_count != 3 || d.operands[0].reg != base || d.operands[1].reg != base
            || d.operands[2].type != ARM64_OP_IMM || d.operands[2].shift.value
            || p.operands[0].reg != base) return false;
        table = getSingleIMM(handle, &insn[i-1]) + d.operands[2].imm;
        break;
    }
    auto index = [&](uint64_t addr, size_t& result) {
        if (addr < insn[0].address || (addr - insn[0].address) % 4) return false;
        result = (addr - insn[0].address) / 4;
        return result < end && insn[result].address == addr && insn[result].size == 4;
    };
    size_t first = 0, fallback = 0;
    const size_t count = static_cast<size_t>(c.operands[1].imm) + 1;
    if (!index(table, first) || count > end - first
        || !index(getSingleIMM(handle, &hi), fallback)) return false;
    std::vector<size_t> edges {fallback};
    for (size_t i = 0; i < count; ++i) {
        int32_t displacement;
        memcpy(&displacement, insn[first+i].bytes, sizeof(displacement));
        size_t target = 0;
        if (!index(static_cast<uint64_t>(a.operands[1].imm + displacement), target)
            || !insn[target].id) return false;
        edges.push_back(target);
    }
    targets.swap(edges);
    return true;
}

// Track a page value, not a single register name. MOV leaves both registers
// live, and different consumers on one page require independent low offsets.
// APFS keeps its __DATA page in x27, copies it to x23, then still loads the
// statistics pointer through x27. Dropping that use turns a zone pointer into
// a statistics object and corrupts the adjacent zone during mountroot.
static int page_register(csh handle, uint32_t reg)
{
    const char* name = cs_reg_name(handle, reg);
    if (!name) return -1;
    if (!strcmp(name, "fp")) return 29;
    if (!strcmp(name, "lr")) return 30;
    if ((name[0] == 'x' || name[0] == 'w') && name[1] >= '0' && name[1] <= '9') {
        int number = atoi(name + 1);
        return number <= 30 ? number : -1;
    }
    return -1;
}

// A CSET-derived index selects exactly two entries in a read-only pointer
// table. Prove both destinations before following either edge; a writable
// table or an intervening definition of its address remains unresolved.
static bool boolean_table_targets(csh handle, const cs_insn* insn, size_t end,
    size_t branch, const Macho* image, std::vector<size_t>& targets)
{
    if (!image || branch < 8) return false;
    size_t base_sub = branch - 3;
    const bool spilled = insn[base_sub].id == ARM64_INS_STR;
    if (spilled) --base_sub;
    if (base_sub < 5) return false;
    const size_t start = base_sub - 5;
    for (size_t i = start; i <= branch; ++i)
        if (!insn[i].detail || insn[i].size != 4
            || insn[i].address != insn[start].address + (i - start) * 4) return false;
    const auto& set = insn[start].detail->arm64;
    const auto& mov = insn[start+1].detail->arm64;
    const auto& mul = insn[start+2].detail->arm64;
    const auto& eor = insn[start+3].detail->arm64;
    const auto& pointer = insn[start+4].detail->arm64;
    const auto& bias = insn[base_sub].detail->arm64;
    const auto& load = insn[branch-2].detail->arm64;
    const auto& offset = insn[branch-1].detail->arm64;
    const auto& br = insn[branch].detail->arm64;
    auto reg = [](const cs_arm64_op& operand, unsigned value) {
        return operand.type == ARM64_OP_REG && operand.reg == value;
    };
    auto immediate = [](const cs_arm64_op& operand, uint64_t maximum) {
        return operand.type == ARM64_OP_IMM && operand.imm >= 0
            && static_cast<uint64_t>(operand.imm) <= maximum && !operand.shift.value;
    };
    auto xreg = [&](unsigned value) {
        const char* name = cs_reg_name(handle, value);
        return page_register(handle, value) >= 0 && name && name[0] != 'w';
    };
    if (insn[start].id != ARM64_INS_CSET || set.op_count != 1
        || set.operands[0].type != ARM64_OP_REG
        || set.operands[0].reg < ARM64_REG_W0 || set.operands[0].reg > ARM64_REG_W30
        || insn[start+1].id != ARM64_INS_MOV || mov.op_count != 2
        || mov.operands[0].type != ARM64_OP_REG
        || mov.operands[0].reg < ARM64_REG_W0 || mov.operands[0].reg > ARM64_REG_W30
        || mov.operands[0].reg == set.operands[0].reg || !immediate(mov.operands[1], 255)
        || insn[start+2].id != ARM64_INS_MUL || mul.op_count != 3
        || !reg(mul.operands[0], set.operands[0].reg)
        || !reg(mul.operands[1], set.operands[0].reg)
        || !reg(mul.operands[2], mov.operands[0].reg)
        || insn[start+3].id != ARM64_INS_EOR || eor.op_count != 3
        || !reg(eor.operands[0], set.operands[0].reg)
        || !reg(eor.operands[1], set.operands[0].reg) || !immediate(eor.operands[2], 255)
        || insn[start+4].id != ARM64_INS_LDR || pointer.op_count != 2 || pointer.writeback
        || pointer.operands[0].type != ARM64_OP_REG || !xreg(pointer.operands[0].reg)
        || pointer.operands[1].type != ARM64_OP_MEM || pointer.operands[1].mem.disp
        || pointer.operands[1].mem.index != ARM64_REG_INVALID
        || !xreg(pointer.operands[1].mem.base)
        || insn[base_sub].id != ARM64_INS_SUB || bias.op_count != 3
        || !reg(bias.operands[0], pointer.operands[0].reg)
        || !reg(bias.operands[1], pointer.operands[0].reg) || !immediate(bias.operands[2], 4095)
        || insn[branch-2].id != ARM64_INS_LDR || load.op_count != 2 || load.writeback
        || load.operands[0].type != ARM64_OP_REG || !xreg(load.operands[0].reg)
        || load.operands[1].type != ARM64_OP_MEM || load.operands[1].mem.disp
        || load.operands[1].mem.base != pointer.operands[0].reg
        || load.operands[1].mem.index != set.operands[0].reg
        || load.operands[1].ext != ARM64_EXT_UXTW
        || load.operands[1].shift.type != ARM64_SFT_LSL || load.operands[1].shift.value != 3
        || insn[branch-1].id != ARM64_INS_SUB || offset.op_count != 3
        || !reg(offset.operands[0], load.operands[0].reg)
        || !reg(offset.operands[1], load.operands[0].reg) || !immediate(offset.operands[2], 4095)
        || insn[branch].id != ARM64_INS_BR || br.op_count != 1
        || !reg(br.operands[0], load.operands[0].reg)) return false;
    if (spilled) {
        const auto& store = insn[base_sub+1].detail->arm64;
        if (store.op_count != 2 || store.writeback
            || !reg(store.operands[0], pointer.operands[0].reg)
            || store.operands[1].type != ARM64_OP_MEM
            || store.operands[1].mem.base != ARM64_REG_SP
            || store.operands[1].mem.index != ARM64_REG_INVALID) return false;
    }
    const unsigned slot_reg = pointer.operands[1].mem.base;
    uint64_t slot = 0;
    bool found = false;
    for (size_t i = start + 4, searched = 0; i && searched < 64; ++searched) {
        --i;
        const auto& current = insn[i];
        if (!current.detail || cs_insn_group(handle, &current, CS_GRP_JUMP)
            || cs_insn_group(handle, &current, CS_GRP_CALL)
            || cs_insn_group(handle, &current, CS_GRP_RET)) return false;
        cs_regs reads, writes; uint8_t nr = 0, nw = 0;
        if (cs_regs_access(handle, &current, reads, &nr, writes, &nw)) return false;
        bool defines = false;
        for (unsigned n = 0; n < nw; ++n)
            defines |= page_register(handle, writes[n]) == page_register(handle, slot_reg);
        if (!defines) continue;
        if (!i || current.id != ARM64_INS_ADD || insn[i-1].id != ARM64_INS_ADRP
            || !insn[i-1].detail) return false;
        const auto& add = current.detail->arm64;
        const auto& page = insn[i-1].detail->arm64;
        if (add.op_count != 3 || !reg(add.operands[0], slot_reg) || !reg(add.operands[1], slot_reg)
            || !immediate(add.operands[2], 4095) || page.op_count != 2
            || !reg(page.operands[0], slot_reg) || page.operands[1].type != ARM64_OP_IMM
            || insn[i-1].address + 4 != current.address) return false;
        const uint64_t address = static_cast<uint64_t>(page.operands[1].imm);
        if (address > UINT64_MAX - static_cast<uint64_t>(add.operands[2].imm)) return false;
        slot = address + add.operands[2].imm;
        found = true;
        break;
    }
    if (!found) return false;
    auto read_pointer = [&](uint64_t address, uint64_t& value) {
        for (const auto* segment : image->segments()) {
            if ((segment->initprot & 2) || address < segment->vmaddr
                || segment->filesize < sizeof(value)
                || address - segment->vmaddr > segment->filesize - sizeof(value)) continue;
            return image->read_u64_vm(address, value);
        }
        return false;
    };
    uint64_t table = 0;
    if (!read_pointer(slot, table) || table < static_cast<uint64_t>(bias.operands[2].imm)) return false;
    table -= bias.operands[2].imm;
    std::vector<size_t> edges;
    for (uint64_t boolean : {UINT64_C(0), UINT64_C(1)}) {
        const uint64_t index = (boolean * mov.operands[1].imm) ^ eor.operands[2].imm;
        if (index > 255 || table > UINT64_MAX - index * 8) return false;
        uint64_t address = 0;
        if (!read_pointer(table + index * 8, address)
            || address < static_cast<uint64_t>(offset.operands[2].imm)) return false;
        address -= offset.operands[2].imm;
        if (!image->is_executable_address(address) || address < insn[0].address
            || (address - insn[0].address) % 4) return false;
        const uint64_t target = (address - insn[0].address) / 4;
        if (target >= end || insn[target].address != address || insn[target].size != 4
            || !insn[target].id) return false;
        edges.push_back(static_cast<size_t>(target));
    }
    targets.swap(edges);
    return true;
}

// A register loaded directly from a global pointer slot can be tail-called
// after preparing arguments. Unlike a switch, the target has no indexed load
// or arithmetic definition. Keep the scan bounded and reject intervening
// control flow or any other definition of the branch register.
static bool indirect_import_tail(csh handle, const cs_insn* insn, size_t branch)
{
    const auto& b = insn[branch].detail->arm64;
    if (b.op_count != 1 || b.operands[0].type != ARM64_OP_REG) return false;
    const int target = page_register(handle, b.operands[0].reg);
    if (target < 0) return false;
    for (size_t i = branch, searched = 0; i && searched < 8; ++searched) {
        --i;
        const auto& current = insn[i];
        if (!current.detail || cs_insn_group(handle, &current, CS_GRP_JUMP)
            || cs_insn_group(handle, &current, CS_GRP_CALL)
            || cs_insn_group(handle, &current, CS_GRP_RET)) return false;
        cs_regs reads, writes; uint8_t nr = 0, nw = 0;
        if (cs_regs_access(handle, &current, reads, &nr, writes, &nw)) return false;
        bool defines = false;
        for (unsigned n = 0; n < nw; ++n) defines |= page_register(handle, writes[n]) == target;
        if (!defines) continue;
        const auto& load = current.detail->arm64;
        if (!i || current.id != ARM64_INS_LDR || load.op_count != 2 || load.writeback
            || load.operands[0].reg != b.operands[0].reg
            || load.operands[1].type != ARM64_OP_MEM
            || load.operands[1].mem.index != ARM64_REG_INVALID
            || load.operands[1].mem.disp < 0 || load.operands[1].mem.disp >= 4096
            || insn[i-1].id != ARM64_INS_ADRP || !insn[i-1].detail) return false;
        const auto& page = insn[i-1].detail->arm64;
        return page.op_count == 2 && page.operands[0].reg == load.operands[1].mem.base;
    }
    return false;
}

static bool stack_failure_call(const Macho* image, uint64_t address)
{
    if (!image) return false;
    Symbol* failure = image->find_symbol_by_name("___stack_chk_fail");
    if (!failure || !failure->symbol_addr) return false;
    for (unsigned depth = 0; depth < 4; ++depth) {
        if (address == failure->symbol_addr) return true;
        uint32_t first = 0, load = 0, branch = 0;
        if (!image->read_u32_vm(address, first)) return false;
        if ((first & 0xfc000000U) == 0x14000000U) {
            int64_t offset = static_cast<int64_t>(first & 0x03ffffffU);
            if (offset & (1 << 25)) offset -= INT64_C(1) << 26;
            address += offset * 4;
            continue;
        }
        if ((first & 0x9f000000U) != 0x90000000U
            || !image->read_u32_vm(address + 4, load)
            || !image->read_u32_vm(address + 8, branch)
            || (load & 0xffc00000U) != 0xf9400000U
            || (branch & 0xfffffc1fU) != 0xd61f0000U
            || ((load >> 5) & 31) != (first & 31)
            || ((branch >> 5) & 31) != (load & 31)) return false;
        int64_t pages = ((first >> 29) & 3) | ((first >> 5) & 0x7ffff) << 2;
        if (pages & (1 << 20)) pages -= INT64_C(1) << 21;
        const uint64_t pointer = (address & ~UINT64_C(0xfff)) + pages * 4096
            + ((load >> 10) & 0xfff) * 8;
        if (!image->read_u64_vm(pointer, address)) return false;
        if (!image->is_mapped_address(address)) {
            uint64_t canonical = address & UINT64_C(0x0000ffffffffffff);
            if (canonical & (UINT64_C(1) << 47)) canonical |= UINT64_C(0xffff000000000000);
            if (!image->is_executable_address(canonical)) return false;
            address = canonical;
        }
    }
    return address == failure->symbol_addr;
}

static bool find_offs(csh handle, const cs_insn* insn, size_t start, size_t end,
    uint32_t reg, std::map<int, uint32_t>& indexs, uint32_t& off, std::set<int>& ambiguous,
    const Macho* image = nullptr)
{
    const int initial = page_register(handle, reg);
    if (initial < 0 || start >= end) return false;
    // Separate definite page bases from values that are pages on only some
    // paths. An uncertain displacement may not change: the same instruction
    // can address a global on one path and an object's field on another.
    // Value equality and definition provenance are distinct: a later ADRP of
    // the same page makes a merge definite, but its independent consumers must
    // not constrain the relocation of the ADRP currently being processed.
    using Pages = std::array<uint32_t, 3>; // definite page, possible page, origin
    std::map<size_t, Pages> incoming;
    std::vector<size_t> pending;
    auto offer = [&](size_t i, Pages masks) {
        if (i >= end) return;
        auto inserted = incoming.emplace(i, masks);
        if (inserted.second) pending.push_back(i);
        else {
            Pages merged {inserted.first->second[0] & masks[0],
                inserted.first->second[1] | masks[1], inserted.first->second[2] | masks[2]};
            if (merged != inserted.first->second) {
                inserted.first->second = merged;
                pending.push_back(i);
            }
        }
    };
    auto target_index = [&](uint64_t address, size_t& index) {
        if (address < insn[0].address || (address - insn[0].address) % 4) return false;
        uint64_t n = (address - insn[0].address) / 4;
        if (n >= end || insn[n].address != address) return false;
        index = static_cast<size_t>(n);
        return true;
    };
    offer(start + 1, {UINT32_C(1) << initial, UINT32_C(1) << initial, UINT32_C(1) << initial});
    while (!pending.empty()) {
        size_t i = pending.back();
        pending.pop_back();
        Pages masks = incoming[i];
        uint32_t mask = masks[0], possible = masks[1], origin = masks[2];
        const cs_insn& current = insn[i];
        if (!current.id || !current.detail) {
            if (origin) {
                printf("Undecoded instruction with live ADRP value at 0x%" PRIx64 "\n", current.address);
                return false;
            }
            continue;
        }
        const auto& a = current.detail->arm64;
        if (current.id == ARM64_INS_RET || current.id == ARM64_INS_ERET) continue;
        if (current.id == ARM64_INS_BL || current.id == ARM64_INS_BLR) {
            // The stack guard failure path has no return edge. Following its
            // fallthrough enters the next function with stale callee-saved
            // page registers. Require its exact symbol or a decoded GOT stub.
            if (current.id == ARM64_INS_BL && stack_failure_call(image, getSingleIMM(handle, &current)))
                continue;
            uint32_t preserved = ~(((UINT32_C(1) << 19) - 1) | (UINT32_C(1) << 30));
            offer(i + 1, {mask & preserved, possible & preserved, origin & preserved});
            continue;
        }
        if (current.id == ARM64_INS_BR) {
            std::vector<size_t> targets;
            const bool import_stub = indirect_import_tail(handle, insn, i);
            bool tail_call = false;
            if (i && insn[i - 1].detail) {
                const auto& previous = insn[i - 1].detail->arm64;
                tail_call = (insn[i - 1].id == ARM64_INS_ADD && previous.op_count == 3
                    && previous.operands[0].type == ARM64_OP_REG && previous.operands[0].reg == ARM64_REG_SP
                    && previous.operands[1].type == ARM64_OP_REG && previous.operands[1].reg == ARM64_REG_SP)
                    || (insn[i - 1].id == ARM64_INS_LDP && previous.writeback
                        && previous.operands[2].type == ARM64_OP_MEM && previous.operands[2].mem.base == ARM64_REG_SP);
            }
            if (import_stub || tail_call || !origin) continue;
            if (!switch_targets(handle, insn, end, i, targets)
                && !boolean_table_targets(handle, insn, end, i, image, targets)) {
                printf("Unknown indirect branch with live ADRP value at 0x%" PRIx64 "\n", current.address);
                return false;
            }
            for (size_t target : targets) offer(target, masks);
            continue;
        }
        if (cs_insn_group(handle, &current, CS_GRP_JUMP)) {
            size_t target = 0;
            for (int n = a.op_count - 1; n >= 0; --n) {
                if (a.operands[n].type == ARM64_OP_IMM) {
                    if (target_index(a.operands[n].imm, target)) offer(target, masks);
                    break;
                }
            }
            if (strcmp(current.mnemonic, "b")) offer(i + 1, masks);
            continue;
        }
        cs_regs reads, writes;
        uint8_t read_count = 0, write_count = 0;
        if (cs_regs_access(handle, &current, reads, &read_count, writes, &write_count)) return false;
        uint32_t next = mask, next_possible = possible, next_origin = origin;
        for (uint8_t n = 0; n < write_count; ++n) {
            int written = page_register(handle, writes[n]);
            if (written >= 0) {
                next &= ~(UINT32_C(1) << written);
                next_possible &= ~(UINT32_C(1) << written);
                next_origin &= ~(UINT32_C(1) << written);
            }
        }
        if (current.id == ARM64_INS_MOV && a.op_count == 2
            && a.operands[0].type == ARM64_OP_REG && a.operands[1].type == ARM64_OP_REG) {
            const char* destination = cs_reg_name(handle, a.operands[0].reg);
            int copy = page_register(handle, a.operands[0].reg);
            int source = page_register(handle, a.operands[1].reg);
            if (copy >= 0 && source >= 0 && destination && destination[0] == 'x') {
                if (mask & (UINT32_C(1) << source)) next |= UINT32_C(1) << copy;
                if (possible & (UINT32_C(1) << source)) next_possible |= UINT32_C(1) << copy;
                if (origin & (UINT32_C(1) << source)) next_origin |= UINT32_C(1) << copy;
            }
        }
        // A branch may reload the same page after using this register for
        // unrelated arithmetic. The new ADRP is a definite definition too;
        // treating it as an unknown overwrite makes the merge falsely ambiguous.
        if (current.id == ARM64_INS_ADRP && a.op_count == 2
            && a.operands[0].type == ARM64_OP_REG && a.operands[1].type == ARM64_OP_IMM
            && a.operands[1].imm == insn[start].detail->arm64.operands[1].imm) {
            int destination = page_register(handle, a.operands[0].reg);
            if (destination >= 0) {
                next |= UINT32_C(1) << destination;
                next_possible |= UINT32_C(1) << destination;
                if (i == start) next_origin |= UINT32_C(1) << destination;
            }
        }
        offer(i + 1, {next, next_possible, next_origin});
    }
    for (const auto& entry : incoming) {
        size_t i = entry.first;
        uint32_t mask = entry.second[0], possible = entry.second[1], origin = entry.second[2];
        bool uncertain = false;
        if (!possible || !insn[i].id || !insn[i].detail) continue;
        const auto& a = insn[i].detail->arm64;
        auto is_page = [&](uint32_t r) {
            int number = page_register(handle, r);
            if (number < 0 || !(origin & (UINT32_C(1) << number))) return false;
            if (!(mask & (UINT32_C(1) << number))) uncertain = true;
            return true;
        };
        bool consumer = false;
        uint32_t displacement = 0;
        if (insn[i].id == ARM64_INS_ADD && a.op_count == 3
            && a.operands[1].type == ARM64_OP_REG && a.operands[2].type == ARM64_OP_IMM
            && is_page(a.operands[1].reg) && a.operands[2].shift.value == 0
            && a.operands[2].imm >= 0 && a.operands[2].imm < 4096) {
            consumer = true;
            displacement = static_cast<uint32_t>(a.operands[2].imm);
        } else if (strstr(insn[i].mnemonic, "ldr") || strstr(insn[i].mnemonic, "str")) {
            for (int n = 0; n < a.op_count; ++n) {
                const auto& operand = a.operands[n];
                if (operand.type != ARM64_OP_MEM || !is_page(operand.mem.base)) continue;
                if (operand.mem.index != ARM64_REG_INVALID || a.writeback
                    || operand.mem.disp < 0 || operand.mem.disp >= 4096) {
                    printf("Unsupported ADRP memory consumer at 0x%" PRIx64 "\n", insn[i].address);
                    return false;
                }
                consumer = true;
                displacement = static_cast<uint32_t>(operand.mem.disp);
            }
        }
        if (consumer) {
            if (indexs.empty()) off = displacement;
            indexs.emplace(static_cast<int>(i), displacement);
            if (uncertain) ambiguous.insert(static_cast<int>(i));
        }
    }
    return !indexs.empty();
}

bool patcher_detail::test_adrp_consumers(const uint32_t* words, size_t count, size_t start,
    std::vector<size_t>& consumers, std::vector<size_t>* uncertain, const Macho* image)
{
    csh handle;
    if (cs_open(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, &handle))
        return false;
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
    cs_option(handle, CS_OPT_SKIPDATA, CS_OPT_ON);
    cs_insn* decoded = nullptr;
    size_t decoded_count = cs_disasm(handle, reinterpret_cast<const uint8_t*>(words), count * 4,
        0x1000, 0, &decoded);
    bool result = false;
    if (decoded_count == count && start < count) {
        std::map<int, uint32_t> offsets;
        uint32_t offset = 0;
        std::set<int> ambiguous;
        result = find_offs(handle, decoded, start, count, decoded[start].detail->arm64.operands[0].reg,
            offsets, offset, ambiguous, image);
        for (const auto& entry : offsets)
            if (!ambiguous.count(entry.first)) consumers.push_back(entry.first);
        if (uncertain) uncertain->assign(ambiguous.begin(), ambiguous.end());
    }
    cs_free(decoded, decoded_count);
    cs_close(&handle);
    return result;
}

size_t patch_kext_to_kernel(KernelMacho& y_kernel, KernelMacho& i_kernel, const LinkResolver* resolver)
{
    size_t unresolved_externals = 0;
    if (y_kernel.is_newer_ver) {
        uint32_t prelink_text_size = y_kernel.get_prelink_text_size();
        uint32_t prelink_text_exec_size = y_kernel.get_prelink_segment_size("__TEXT_EXEC", 1 << 12);
        uint32_t prelink_data_size = y_kernel.get_prelink_data_size(SEG_ALIGN);
        uint32_t prelink_data_const_size = y_kernel.get_prelink_segment_size("__DATA_CONST", SEG_ALIGN);

        printf("pre text size: %x\n", prelink_text_size);
        printf("pre text exec size: %x\n", prelink_text_exec_size);
        printf("pre data size: %x\n", prelink_data_size);
        printf("pre data const size: %x\n", prelink_data_const_size);

        prelink_text_size = ALIGN_UP(prelink_text_size, ARM64_VM_PAGE_SIZE);
        prelink_text_exec_size = ALIGN_UP(prelink_text_exec_size, ARM64_VM_PAGE_SIZE);
        prelink_data_size = ALIGN_UP(prelink_data_size, ARM64_VM_PAGE_SIZE);
        prelink_data_const_size = ALIGN_UP(prelink_data_const_size, ARM64_VM_PAGE_SIZE);

        void* prelink_text_buf = calloc(prelink_text_size, sizeof(char));
        void* prelink_text_exec_buf = calloc(prelink_text_exec_size, sizeof(char));
        void* prelink_data_buf = calloc(prelink_data_size, sizeof(char));
        void* prelink_data_const_buf = calloc(prelink_data_const_size, sizeof(char));

        // 将对应kext拷贝到目标段缓冲区中
        uint64_t kerenl_text_off = 0;
        uint64_t kerenl_text_exec_off = 0;
        uint64_t kerenl_data_off = 0;
        uint64_t kerenl_data_const_off = 0;
        uint32_t size = 0;

        for (auto kext : y_kernel.kexts) {
            if (kext->exec_macho) {
                kext->text_off = kerenl_text_off;
                segment_command_64_t* text_seg = (segment_command_64_t*)(kext->exec_macho->find_segment("__TEXT"));
                if (text_seg && text_seg->filesize > 0) {
                    memset((void*)((uint64_t)prelink_text_buf + kerenl_text_off), 0, text_seg->filesize);
                    memcpy((char*)((uint64_t)prelink_text_buf + kerenl_text_off),
                        (char*)(uint64_t)kext->exec_macho->file_buf + text_seg->fileoff,
                        text_seg->filesize);
                    // printf("Copy 0x%016llx -> 0x%016llx\n", text_seg->fileoff, kerenl_text_off);
                    kerenl_text_off += ALIGN_UP(text_seg->filesize, SEG_ALIGN);
                } else {
                    if (kext->exec_macho->header->sizeofcmds) {
                        size = ALIGN_UP(sizeof(mach_header_64_t) + kext->exec_macho->header->sizeofcmds, SEG_ALIGN);
                        memcpy((char*)((uint64_t)prelink_text_buf + kerenl_text_off),
                            (char*)(uint64_t)kext->exec_macho->file_buf,
                            size);

                        // Resource-only KPI headers in this firmware advertise
                        // an omitted UUID command, overlapping the next header.
                        // Normalize only the serialized copy; retain the source
                        // symbol tables for resolving kernel exports.
                        auto* resource = plist_get_item(kext->kextInfoElement, "OSKernelResource");
                        if (!resource || strcmp(resource->Name(), "true")) {
                            printf("Unsupported executable Kext without __TEXT: %s\n", kext->kext_id);
                            return SIZE_MAX;
                        }
                        auto* packed = reinterpret_cast<mach_header_64_t*>(
                            static_cast<char*>(prelink_text_buf) + kerenl_text_off);
                        uint32_t valid = 0, used = sizeof(*packed);
                        while (valid < packed->ncmds && used + sizeof(load_command) <= size) {
                            auto* command = reinterpret_cast<load_command*>(reinterpret_cast<char*>(packed) + used);
                            if (command->cmdsize < 8 || (command->cmdsize & 7)
                                || command->cmdsize > size - used)
                                break;
                            used += command->cmdsize;
                            ++valid;
                        }
                        if (!valid)
                            return SIZE_MAX;
                        packed->ncmds = valid;
                        packed->sizeofcmds = used - sizeof(*packed);
                        packed->reserved = 0;
                        memset(reinterpret_cast<char*>(packed) + used, 0, size - used);
                        kext->serialized_text_size = used;

                        // XNU KCGEN walks resource headers using aligned
                        // sizeofcmds (not the stale source allocation or
                        // sizeof(header)+sizeofcmds). No interior zero gap is
                        // permitted: zero magic means end-of-all-Kexts.
                        uint32_t stride = ALIGN_UP(packed->sizeofcmds, SEG_ALIGN);
                        if (stride < used || stride > size) {
                            printf("Resource header cannot satisfy XNU KCGEN walk: %s\n", kext->kext_id);
                            return SIZE_MAX;
                        }
                        kerenl_text_off += stride;
                    }
                }
                kext->text_exec_off = kerenl_text_exec_off;
                copy_segment_from_kext(kext, i_kernel, "__TEXT_EXEC", "__PLK_TEXT_EXEC", prelink_text_exec_buf, kerenl_text_exec_off, 1 << 12);
                if (auto* data = static_cast<segment_command_64_t*>(kext->exec_macho->find_segment("__DATA")))
                    kerenl_data_off = ALIGN_UP(kerenl_data_off, 1 << 12) + (data->vmaddr & 0xfff);
                kext->data_off = kerenl_data_off;
                copy_segment_from_kext(kext, i_kernel, "__DATA", "__PRELINK_DATA", prelink_data_buf, kerenl_data_off, SEG_ALIGN);
                if (auto* constants = static_cast<segment_command_64_t*>(kext->exec_macho->find_segment("__DATA_CONST")))
                    kerenl_data_const_off = ALIGN_UP(kerenl_data_const_off, 1 << 12) + (constants->vmaddr & 0xfff);
                kext->data_const_off = kerenl_data_const_off;
                copy_segment_from_kext(kext, i_kernel, "__DATA_CONST", "__PLK_DATA_CONST", prelink_data_const_buf, kerenl_data_const_off, SEG_ALIGN);
            }
        }

        // 重新设定头部地址
        // 先计算各段基址
        uint64_t new_prelink_text_base = 0;
        uint64_t new_prelink_text_exec_base = 0;
        uint64_t new_prelink_data_base = 0;
        uint64_t new_prelink_data_const_base = 0;
        segment_command_64_t* y_text_segment = (segment_command_64_t*)(y_kernel.find_segment("__TEXT"));
        segment_command_64_t* y_linkedit_seg = (segment_command_64_t*)y_kernel.find_segment("__LINKEDIT");

        segment_command_64_t* i_prelink_text_segment = (segment_command_64_t*)(i_kernel.find_segment("__PRELINK_TEXT"));
        segment_command_64_t* i_prelink_data_segment = (segment_command_64_t*)(i_kernel.find_segment("__PRELINK_DATA"));

        uint64_t kernel_text_vmbase = y_text_segment->vmaddr;
        new_prelink_data_const_base = kernel_text_vmbase - prelink_data_const_size;
        new_prelink_data_base = ALIGN_UP(y_linkedit_seg->vmaddr + y_linkedit_seg->vmsize, ARM64_VM_PAGE_SIZE);
        new_prelink_text_exec_base = new_prelink_data_const_base - prelink_text_exec_size;
        new_prelink_text_base = new_prelink_text_exec_base - prelink_text_size;

        y_kernel.prelink_text_base = new_prelink_text_base;
        printf("Prelink text will at 0x%016" PRIx64 "\n", new_prelink_text_base);
        printf("Prelink data will at 0x%016" PRIx64 "\n", new_prelink_data_base);

        // 获取内核末尾
        uint64_t end_fileoff = y_linkedit_seg->fileoff + y_linkedit_seg->filesize;
        end_fileoff = ALIGN_UP(end_fileoff, ARM64_VM_PAGE_SIZE);
        printf("End at 0x%016" PRIx64 "\n", end_fileoff);
        uint64_t new_prelink_text_fileoff = end_fileoff;
        uint64_t new_prelink_text_exec_fileoff = new_prelink_text_fileoff + prelink_text_size;
        uint64_t new_prelink_data_const_fileoff = new_prelink_text_exec_fileoff + prelink_text_exec_size;
        printf("data const off: 0x%" PRIx64 "\n", new_prelink_data_const_fileoff);
        uint64_t new_prelink_data_fileoff = new_prelink_data_const_fileoff + prelink_data_const_size;

        // 修改Kext符号表
        csh handle;
        if (cs_open(CS_ARCH_ARM64, (cs_mode)(CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN), &handle)) {
            printf("ERROR: Failed to initialize engine!\n");
            return SIZE_MAX;
        }
        cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
        cs_option(handle, CS_OPT_SKIPDATA, CS_OPT_ON);
        ks_engine* ks;
        if (ks_open(KS_ARCH_ARM64, KS_MODE_LITTLE_ENDIAN, &ks)) {
            printf("ERROR: failed on ks_open(), quit\n");
            return SIZE_MAX;
        }

        for (auto kext : y_kernel.kexts) {
            // KMOD处理
            if (kext->exec_macho) {
                segment_command_64_t* kext_text_seg = (segment_command_64_t*)kext->exec_macho->find_segment("__TEXT");
                segment_command_64_t* kext_data_const_seg = (segment_command_64_t*)kext->exec_macho->find_segment("__DATA_CONST");
                segment_command_64_t* kext_text_exec_seg = (segment_command_64_t*)kext->exec_macho->find_segment("__TEXT_EXEC");
                segment_command_64_t* kext_data_seg = (segment_command_64_t*)kext->exec_macho->find_segment("__DATA");

                if (kext->is_from_file) {
                    if (kext->exec_macho->find_segment("__TEXT")) {
                        Symbol* kmod_symbol = kext->exec_macho->find_symbol_by_name("_kmod_info");
                        if (kmod_symbol && kext_data_seg) {
                            kext->kmod_addr = kmod_symbol->symbol_addr - kext_data_seg->vmaddr;
                            kext->kmod_addr += new_prelink_data_base + kext->data_off;
                            printf("New kmod addr: 0x%" PRIx64 "\n", kext->kmod_addr);
                        }
                    } else {
                        kext->kmod_addr = 0;
                    }
                } else {
                    if (kext->kmod_addr && kext_data_seg)
                        kext->kmod_addr = kext->kmod_addr - kext_data_seg->vmaddr + new_prelink_data_base + kext->data_off;
                }

                if (kext->kmod_addr) {
                    kmod_info_64_v1_t* kext_kmod = (kmod_info_64_v1_t*)(kext->kmod_addr - new_prelink_data_base + (uint64_t)prelink_data_buf);
                    printf("K add: 0x%" PRIx64 "\n", kext_kmod->address);
                    printf("K n: %s\n", kext_kmod->name);
                    const char* bundle_version = plist_get_string(kext->kextInfoElement, "CFBundleVersion");
                    if (!bundle_version || strlen(kext->kext_id) >= KMOD_MAX_NAME
                        || strlen(bundle_version) >= KMOD_MAX_NAME) {
                        printf("Cannot represent canonical kmod name/version for %s\n", kext->kext_id);
                        return SIZE_MAX;
                    }
                    memset(kext_kmod->name, 0, sizeof(kext_kmod->name));
                    memset(kext_kmod->version, 0, sizeof(kext_kmod->version));
                    memcpy(kext_kmod->name, kext->kext_id, strlen(kext->kext_id));
                    memcpy(kext_kmod->version, bundle_version, strlen(bundle_version));
                    kext_kmod->address = new_prelink_text_base + kext->text_off;
                    segment_command_64_t* kext_text_exec_seg = (segment_command_64_t*)kext->exec_macho->find_segment("__TEXT_EXEC");
                    if (kext_kmod->start_addr) {
                        kext_kmod->start_addr -= kext_text_exec_seg->vmaddr;
                        kext_kmod->start_addr += new_prelink_text_exec_base + kext->text_exec_off;
                        printf("k start at: 0x%" PRIx64 "\n", kext_kmod->start_addr);
                    }
                    if (kext_kmod->stop_addr) {
                        kext_kmod->stop_addr -= kext_text_exec_seg->vmaddr;
                        kext_kmod->stop_addr += new_prelink_text_exec_base + kext->text_exec_off;
                    }
                }

                // Local Kexts carry explicit local/external relocation records.
                // Their scalar register offsets, counts and masks can equal
                // small image addresses; only stripped provider images need
                // the legacy pointer scan. Apply local records below.
                if (kext_data_seg && !kext->is_from_file) {
                    section_64_t* kext_cstring_sect = (section_64*)kext->exec_macho->find_section("__TEXT", "__cstring");
                    uint64_t kext_start_addr = (kext->is_from_file && kext_cstring_sect)
                        ? kext_cstring_sect->addr : kext_text_seg->vmaddr;
                    uint64_t* addrs = (uint64_t*)((uint64_t)prelink_data_buf + kext->data_off);
                    for (size_t i = 0; i < kext_data_seg->vmsize / sizeof(uint64_t); i++) {
                        const uint64_t site = new_prelink_data_base + kext->data_off + i * sizeof(uint64_t);
                        if (patcher_detail::kmod_identity_word(kext->kmod_addr, site))
                            continue;
                        if (addrs[i] >= kext_start_addr && addrs[i] < kext_text_seg->vmaddr + kext_text_seg->vmsize) {
                            // printf("Data in __TEXT, addr: %llx\n", addrs[i]);
                            addrs[i] -= kext_text_seg->vmaddr;
                            addrs[i] += new_prelink_text_base + kext->text_off;
                        } else if (addrs[i] >= kext_text_exec_seg->vmaddr && addrs[i] < kext_text_exec_seg->vmaddr + kext_text_exec_seg->vmsize) {
                            // printf("Data in __TEXT_EXEC, addr: %llx\n", addrs[i]);
                            addrs[i] -= kext_text_exec_seg->vmaddr;
                            addrs[i] += new_prelink_text_exec_base + kext->text_exec_off;
                        } else if (addrs[i] >= kext_data_seg->vmaddr && addrs[i] < kext_data_seg->vmaddr + kext_data_seg->vmsize) {
                            // printf("Data in __DATA, addr: %llx\n", addrs[i]);
                            addrs[i] -= kext_data_seg->vmaddr;
                            addrs[i] += new_prelink_data_base + kext->data_off;
                        } else if (IN_SEGMENT_RANGE(addrs[i], kext_data_const_seg)) {
                            // printf("Data in __DATA_CONST, addr: %llx\n", addrs[i]);
                            addrs[i] -= kext_data_const_seg->vmaddr;
                            addrs[i] += new_prelink_data_const_base + kext->data_const_off;
                        } else if (!kext->is_from_file) {
                            if (addrs[i]) {
                                uint64_t resolved_address = 0;
                                if (rebase_dependency_reference(kext, addrs[i], new_prelink_text_base,
                                        new_prelink_text_exec_base, new_prelink_data_base,
                                        new_prelink_data_const_base, resolved_address)
                                    || resolve_kernel_reference(addrs[i], i_kernel, y_kernel, resolver, resolved_address)) {
                                    addrs[i] = resolved_address;
                                } else if (!i_kernel.find_symbol_by_addr(addrs[i])) {
                                    bool found_symbol = false;
                                    // 找不到再从i_kernel的kext里面找
                                    for (size_t j = 0; j < kext->depends.size(); j++) {
                                        if (!kext->depends[j]->is_from_file && kext->depends[j]->exec_macho) {
                                            segment_command_64_t* dep_text_seg = (segment_command_64_t*)kext->depends[j]->exec_macho->find_segment("__TEXT");
                                            segment_command_64_t* dep_text_exec_seg = (segment_command_64_t*)kext->depends[j]->exec_macho->find_segment("__TEXT_EXEC");
                                            segment_command_64_t* dep_data_seg = (segment_command_64_t*)kext->depends[j]->exec_macho->find_segment("__DATA");
                                            segment_command_64_t* dep_data_const_seg = (segment_command_64_t*)kext->depends[j]->exec_macho->find_segment("__DATA_CONST");

                                            if (kext->depends[j]->exec_macho->addr_in_segment(dep_text_seg, addrs[i])) {
                                                addrs[i] -= dep_text_seg->vmaddr;
                                                addrs[i] += new_prelink_text_base + kext->depends[j]->text_off;
                                                found_symbol = true;
                                            } else if (kext->depends[j]->exec_macho->addr_in_segment(dep_text_exec_seg, addrs[i])) {
                                                addrs[i] -= dep_text_exec_seg->vmaddr;
                                                addrs[i] += new_prelink_text_exec_base + kext->depends[j]->text_exec_off;
                                                found_symbol = true;
                                            } else if (kext->depends[j]->exec_macho->addr_in_segment(dep_data_seg, addrs[i])) {
                                                addrs[i] -= dep_data_seg->vmaddr;
                                                addrs[i] += new_prelink_data_base + kext->depends[j]->data_off;
                                                found_symbol = true;
                                            // } else if (addrs[i] >= dep_data_const_seg->vmaddr && addrs[i] < dep_data_const_seg->vmaddr + dep_data_const_seg->vmsize) {
                                            } else if (IN_SEGMENT_RANGE(addrs[i], dep_data_const_seg)) {
                                                addrs[i] -= dep_data_const_seg->vmaddr;
                                                addrs[i] += new_prelink_data_const_base + kext->depends[j]->data_const_off;
                                                found_symbol = true;
                                            }
                                        }
                                    }
                                    if (!found_symbol)
                                        if (addrs[i] >= i_prelink_text_segment->vmaddr && addrs[i] < i_prelink_data_segment->vmaddr + i_prelink_data_segment->vmsize) {
                                            ++unresolved_externals;
                                            printf("%d: Not found kernel symbol %" PRIx64 " at 0x%" PRIx64 "\n", __LINE__, addrs[i], kext_data_seg->vmaddr + i * sizeof(uint64_t));
                                        }
                                } else {
                                    ++unresolved_externals;
                                    Symbol* source_symbol = i_kernel.find_symbol_by_addr(addrs[i]);
                                    printf("Not found kernel symbol(%s)\n", source_symbol->symbol_name);
                                }
                            }
                        }
                        // i_kernel!!!
                    }
                }

                // Rebase constructor and destructor tables independently.
                // Init-only kexts are common; requiring a term section leaves
                // raw addresses such as 0x43f0 in __mod_init_func.
                if (kext_data_const_seg) {
                    section_64_t* init_sect = (section_64_t*)kext->exec_macho->find_section("__DATA_CONST", "__mod_init_func");
                    section_64_t* term_sect = (section_64_t*)kext->exec_macho->find_section("__DATA_CONST", "__mod_term_func");
                    if (!rebase_structor_section(kext, init_sect, "__mod_init_func",
                            kext_data_const_seg, kext_text_exec_seg,
                            prelink_data_const_buf, new_prelink_text_exec_base)
                        || !rebase_structor_section(kext, term_sect, "__mod_term_func",
                            kext_data_const_seg, kext_text_exec_seg,
                            prelink_data_const_buf, new_prelink_text_exec_base)) {
                        ks_close(ks);
                        cs_close(&handle);
                        return SIZE_MAX;
                    }

                    if (kext_text_exec_seg)
                        printf("Kext __TEXT_EXEC at %" PRIx64 "\n", kext_text_exec_seg->vmaddr);
                    if (kext_data_seg)
                        printf("Kext __DATA at %" PRIx64 "\n", kext_data_seg->vmaddr);
                    printf("Kext __DATA_CONST at %" PRIx64 "\n", kext_data_const_seg->vmaddr);

                    section_64_t* kext_cstring_sect = (section_64*)kext->exec_macho->find_section("__TEXT", "__cstring");
                    section_64_t* kext_const_sect = (section_64*)kext->exec_macho->find_section("__DATA_CONST", "__const");

                    section_64_t* kext_got_sect = (section_64*)kext->exec_macho->find_section("__DATA_CONST", "__got");

                    if (!kext_const_sect) {
                        kext_const_sect = (section_64*)kext->exec_macho->find_section("__DATA_CONST", "__kalloc_type");
                    }
                    if (kext_data_const_seg) {
                        uint64_t* addrs = 0;
                        if (kext_const_sect && !kext->is_from_file) {
                            addrs = (uint64_t*)((uint64_t)prelink_data_const_buf + kext->data_const_off + kext_const_sect->addr - kext_data_const_seg->vmaddr);
                            for (size_t i = 0; i < (kext_data_const_seg->vmaddr + kext_data_const_seg->vmsize - kext_const_sect->addr) / sizeof(uint64_t); i++) {
                                if (kext_got_sect && addrs[i] >= kext_got_sect->addr && addrs[i] < kext_got_sect->addr + kext_got_sect->size) {
                                    printf("?In?\n");
                                    continue;
                                }
                                if (kext->exec_macho->addr_in_segment(kext_text_seg, addrs[i])) {
                                    // printf("Data in __TEXT, addr: %llx\n", addrs[i]);
                                    if (kext->is_from_file) {
                                        if (kext_cstring_sect && addrs[i] >= kext_cstring_sect->addr) {
                                            addrs[i] -= kext_text_seg->vmaddr;
                                            addrs[i] += new_prelink_text_base + kext->text_off;
                                        }
                                    } else {
                                        addrs[i] -= kext_text_seg->vmaddr;
                                        addrs[i] += new_prelink_text_base + kext->text_off;
                                    }

                                } else if (kext->exec_macho->addr_in_segment(kext_text_exec_seg, addrs[i])) {
                                    // printf("Data in __TEXT_EXEC, addr: %llx\n", addrs[i]);
                                    addrs[i] -= kext_text_exec_seg->vmaddr;
                                    addrs[i] += new_prelink_text_exec_base + kext->text_exec_off;
                                } else if (kext->exec_macho->addr_in_segment(kext_data_seg, addrs[i])) {
                                    // printf("Data in __DATA, addr: %llx\n", addrs[i]);
                                    addrs[i] -= kext_data_seg->vmaddr;
                                    addrs[i] += new_prelink_data_base + kext->data_off;
                                } else if (kext->exec_macho->addr_in_segment(kext_data_const_seg, addrs[i])) {
                                    // printf("Data in __DATA_CONST, addr: %llx\n", addrs[i]);
                                    addrs[i] -= kext_data_const_seg->vmaddr;
                                    addrs[i] += new_prelink_data_const_base + kext->data_const_off;
                                } else if (!kext->is_from_file) {
                                    if (addrs[i]) {
                                        uint64_t resolved_address = 0;
                                        if (rebase_dependency_reference(kext, addrs[i], new_prelink_text_base,
                                                new_prelink_text_exec_base, new_prelink_data_base,
                                                new_prelink_data_const_base, resolved_address)
                                            || resolve_kernel_reference(addrs[i], i_kernel, y_kernel, resolver, resolved_address)) {
                                            addrs[i] = resolved_address;
                                        } else if (!i_kernel.find_symbol_by_addr(addrs[i])) {
                                            bool found_symbol = false;
                                            // 找不到再从i_kernel的kext里面找
                                            for (size_t j = 0; j < kext->depends.size(); j++) {
                                                if (!kext->depends[j]->is_from_file && kext->depends[j]->exec_macho) {
                                                    segment_command_64_t* dep_text_seg = (segment_command_64_t*)kext->depends[j]->exec_macho->find_segment("__TEXT");
                                                    segment_command_64_t* dep_text_exec_seg = (segment_command_64_t*)kext->depends[j]->exec_macho->find_segment("__TEXT_EXEC");
                                                    segment_command_64_t* dep_data_seg = (segment_command_64_t*)kext->depends[j]->exec_macho->find_segment("__DATA");
                                                    segment_command_64_t* dep_data_const_seg = (segment_command_64_t*)kext->depends[j]->exec_macho->find_segment("__DATA_CONST");

                                                    if (kext->depends[j]->exec_macho->addr_in_segment(dep_text_seg, addrs[i])) {
                                                        addrs[i] -= dep_text_seg->vmaddr;
                                                        addrs[i] += new_prelink_text_base + kext->depends[j]->text_off;
                                                        found_symbol = true;
                                                    } else if (kext->depends[j]->exec_macho->addr_in_segment(dep_text_exec_seg, addrs[i])) {
                                                        addrs[i] -= dep_text_exec_seg->vmaddr;
                                                        addrs[i] += new_prelink_text_exec_base + kext->depends[j]->text_exec_off;
                                                        found_symbol = true;
                                                    } else if (kext->depends[j]->exec_macho->addr_in_segment(dep_data_seg, addrs[i])) {
                                                        addrs[i] -= dep_data_seg->vmaddr;
                                                        addrs[i] += new_prelink_data_base + kext->depends[j]->data_off;
                                                        found_symbol = true;
                                                    } else if (IN_SEGMENT_RANGE(addrs[i], dep_data_const_seg)) {
                                                        addrs[i] -= dep_data_const_seg->vmaddr;
                                                        addrs[i] += new_prelink_data_const_base + kext->depends[j]->data_const_off;
                                                        found_symbol = true;
                                                    }

                                                    if (found_symbol)
                                                        break;
                                                }
                                            }
                                            if (!found_symbol)
                                                if (addrs[i] >= i_prelink_text_segment->vmaddr && addrs[i] < i_prelink_data_segment->vmaddr + i_prelink_data_segment->vmsize) {
                                                    ++unresolved_externals;
                                                    printf("%d: Not found kernel symbol %" PRIx64 " at 0x%" PRIx64 "\n", __LINE__, addrs[i], kext_const_sect->addr + i * sizeof(uint64_t));
                                                }
                                            // printf("%d: Not found kernel symbol %llx at 0x%llx\n", __LINE__, addrs[i], &addrs[i] - ((uint64_t)prelink_data_const_buf + kext->data_const_off) + kext_data_const_seg->vmaddr);
                                        } else {
                                            ++unresolved_externals;
                                            Symbol* source_symbol = i_kernel.find_symbol_by_addr(addrs[i]);
                                            printf("Not found kernel symbol(%s)\n", source_symbol->symbol_name);
                                        }
                                    }
                                }
                            }
                        }

                        if (kext_got_sect) {
                            if (!kext->is_from_file) {
                                addrs = (uint64_t*)((uint64_t)prelink_data_const_buf + kext->data_const_off + kext_got_sect->addr - kext_data_const_seg->vmaddr);
                                for (size_t i = 0; i < kext_got_sect->size / sizeof(uint64_t); i++) {
                                    uint64_t resolved_address = 0;
                                    if (rebase_dependency_reference(kext, addrs[i], new_prelink_text_base,
                                            new_prelink_text_exec_base, new_prelink_data_base,
                                            new_prelink_data_const_base, resolved_address)
                                        || resolve_kernel_reference(addrs[i], i_kernel, y_kernel, resolver, resolved_address)) {
                                        addrs[i] = resolved_address;
                                    } else if (i_kernel.find_symbol_by_addr(addrs[i])) {
                                        ++unresolved_externals;
                                        printf("%d: Not found symbol %s\n", __LINE__, i_kernel.find_symbol_by_addr(addrs[i])->symbol_name);
                                    } else {
                                        bool found_symbol = false;
                                        // 找不到再从i_kernel的kext里面找
                                        for (size_t j = 0; j < kext->depends.size(); j++) {
                                            if (!kext->depends[j]->is_from_file && kext->depends[j]->exec_macho) {
                                                segment_command_64_t* dep_text_seg = (segment_command_64_t*)kext->depends[j]->exec_macho->find_segment("__TEXT");
                                                segment_command_64_t* dep_text_exec_seg = (segment_command_64_t*)kext->depends[j]->exec_macho->find_segment("__TEXT_EXEC");
                                                segment_command_64_t* dep_data_seg = (segment_command_64_t*)kext->depends[j]->exec_macho->find_segment("__DATA");
                                                segment_command_64_t* dep_data_const_seg = (segment_command_64_t*)kext->depends[j]->exec_macho->find_segment("__DATA_CONST");

                                                if (kext->depends[j]->exec_macho->addr_in_segment(dep_text_seg, addrs[i])) {
                                                    addrs[i] -= dep_text_seg->vmaddr;
                                                    addrs[i] += new_prelink_text_base + kext->depends[j]->text_off;
                                                    found_symbol = true;
                                                } else if (kext->depends[j]->exec_macho->addr_in_segment(dep_text_exec_seg, addrs[i])) {
                                                    addrs[i] -= dep_text_exec_seg->vmaddr;
                                                    addrs[i] += new_prelink_text_exec_base + kext->depends[j]->text_exec_off;
                                                    found_symbol = true;
                                                } else if (kext->depends[j]->exec_macho->addr_in_segment(dep_data_seg, addrs[i])) {
                                                    addrs[i] -= dep_data_seg->vmaddr;
                                                    addrs[i] += new_prelink_data_base + kext->depends[j]->data_off;
                                                    found_symbol = true;
                                                } else if (IN_SEGMENT_RANGE(addrs[i], dep_data_const_seg)) {
                                                    addrs[i] -= dep_data_const_seg->vmaddr;
                                                    addrs[i] += new_prelink_data_const_base + kext->depends[j]->data_const_off;
                                                    found_symbol = true;
                                                }

                                                if (found_symbol) {
                                                    break;
                                                }
                                            }
                                        }
                                        if (!found_symbol) {
                                            ++unresolved_externals;
                                                printf("%d:Not found kernel symbol %" PRIx64 " at 0x%" PRIx64 "\n", __LINE__, addrs[i], kext_got_sect->addr + i * sizeof(uint64_t));
                                        }
                                    }
                                }
                            }
                        }
                    } else {
                        printf("Kext not have __DATA_CONST.__const\n");
                    }
                }

                // 修补adrp
                if (kext_text_seg) {
                    printf("Kext TEXT at 0x%" PRIx64 "\n", kext_text_seg->vmaddr);
                }
                if (kext_text_exec_seg) {
                    size_t count;
                    cs_insn* insn;
                    uint64_t kext_text_exec_buf_addr = (uint64_t)prelink_text_exec_buf + kext->text_exec_off;
                    const auto data_ranges = find_verified_code_data(
                        reinterpret_cast<const uint8_t*>(kext_text_exec_buf_addr), kext_text_exec_seg->filesize, kext->kext_id);
                    for (const auto& range : data_ranges)
                        printf("Verified complete constant data at 0x%" PRIx64 " (%zu bytes)\n",
                            kext_text_exec_seg->vmaddr + range.first, range.second);

                    count = cs_disasm(handle, (const uint8_t*)kext_text_exec_buf_addr, kext_text_exec_seg->filesize, kext_text_exec_seg->vmaddr, 0, &insn);
                    printf("Disassembled %zu instructions\n", count);
                    // printf("Nedd: %x\n", kext_text_exec_seg->filesize / 4);

                    char new_insn[128];
                    if (count > 0) {
                        for (size_t i = 0; i < count; i++) {
                            bool embedded_data = false;
                            for (const auto& range : data_ranges)
                                if (insn[i].address >= kext_text_exec_seg->vmaddr + range.first
                                    && insn[i].address < kext_text_exec_seg->vmaddr + range.first + range.second)
                                    embedded_data = true;
                            if (embedded_data)
                                continue;
                            if (strstr(insn[i].mnemonic, "adrp")) {
                                int acount = cs_op_count(handle, &insn[i], ARM64_OP_IMM);
                                // printf("acount: %d\n", acount);
                                if (acount == 1) {
                                    uint64_t imm = getSingleIMM(handle, &insn[i]);

                                    unsigned char* encode;
                                    size_t encode_size;
                                    size_t stat_count;
                                    int reg_index = cs_op_index(handle, &insn[i], ARM64_OP_REG, 1);
                                    const char* write_reg = cs_reg_name(handle, insn[i].detail->arm64.operands[reg_index].reg);
                                    uint64_t patch_addr = 0;

                                    // 寻找匹配的off
                                    std::map<int, uint32_t> off_indexs;
                                    uint32_t off = 0;
                                    std::set<int> ambiguous;
                                    // Only a wholly file-backed page in this
                                    // image's read-only TEXT can use this path.
                                    // Partial pages and changed low bits still
                                    // require the original consumer analysis.
                                    bool uniform_page = kext_text_seg
                                        && !(kext_text_seg->initprot & 2) // VM_PROT_WRITE
                                        && patcher_detail::rebase_uniform_page(imm,
                                            kext_text_seg->vmaddr, kext_text_seg->filesize,
                                            new_prelink_text_base + kext->text_off, patch_addr);
                                    bool find_off = uniform_page || find_offs(handle, insn, i, count,
                                        insn[i].detail->arm64.operands[reg_index].reg,
                                        off_indexs, off, ambiguous, &i_kernel);
                                    if (!find_off) {
                                        printf("Unresolved ADRP consumers at 0x%" PRIx64 "\n", insn[i].address);
                                        ++unresolved_externals;
                                        continue;
                                    }

                                    auto relocate_address = [&](uint64_t source_addr, uint64_t& relocated_addr) {
                                        if (kext->exec_macho->addr_in_segment(kext_text_seg, source_addr)) {
                                            relocated_addr = source_addr - kext_text_seg->vmaddr
                                                + new_prelink_text_base + kext->text_off;
                                        } else if (kext->exec_macho->addr_in_segment(kext_text_exec_seg, source_addr)) {
                                            relocated_addr = source_addr - kext_text_exec_seg->vmaddr
                                                + new_prelink_text_exec_base + kext->text_exec_off;
                                        } else if (kext->exec_macho->addr_in_segment(kext_data_seg, source_addr)) {
                                            relocated_addr = source_addr - kext_data_seg->vmaddr
                                                + new_prelink_data_base + kext->data_off;
                                        } else if (IN_SEGMENT_RANGE(source_addr, kext_data_const_seg)) {
                                            relocated_addr = source_addr - kext_data_const_seg->vmaddr
                                                + new_prelink_data_const_base + kext->data_const_off;
                                        } else {
                                            return rebase_dependency_reference(kext, source_addr,
                                                new_prelink_text_base, new_prelink_text_exec_base,
                                                new_prelink_data_base, new_prelink_data_const_base, relocated_addr)
                                                || resolve_kernel_reference(source_addr, i_kernel, y_kernel, resolver, relocated_addr);
                                        }
                                        return true;
                                    };

                                    std::map<int, uint32_t> relocated_offs;
                                    bool page_conflict = false;
                                    for (const auto& consumer : off_indexs) {
                                        uint64_t relocated_addr = 0;
                                        if (!relocate_address(imm + consumer.second, relocated_addr))
                                            continue;
                                        if (ambiguous.count(consumer.first)
                                            && (relocated_addr & 0xfff) != consumer.second) {
                                            printf("Ambiguous ADRP/object displacement at 0x%" PRIx64 " in %s\n",
                                                insn[consumer.first].address, kext->kext_id);
                                            page_conflict = true;
                                            break;
                                        }
                                        uint64_t relocated_page = relocated_addr & ~UINT64_C(0xFFF);
                                        if (!patch_addr)
                                            patch_addr = relocated_page;
                                        else if (patch_addr != relocated_page) {
                                            printf("ADRP page conflict source=0x%" PRIx64 " target=0x%" PRIx64 " first-page=0x%" PRIx64 " consumer=0x%" PRIx64 "\n",
                                                imm + consumer.second, relocated_addr, patch_addr, insn[consumer.first].address);
                                            page_conflict = true;
                                            break;
                                        }
                                        relocated_offs.emplace(consumer.first,
                                            static_cast<uint32_t>(relocated_addr & 0xFFF));
                                    }
                                    if (!patch_addr || page_conflict) {
                                        ++unresolved_externals;
                                        if (!patch_addr)
                                            printf("Unresolved ADRP target at 0x%" PRIx64 " in %s\n",
                                                insn[i].address, kext->kext_id);
                                        if (page_conflict) {
                                            printf("ADRP consumers relocate to different pages at 0x%" PRIx64
                                                   " in %s\n", insn[i].address,
                                                kext->kext_id ? kext->kext_id : "<unknown>");
                                        }
                                        continue;
                                    }

                                    snprintf(new_insn, sizeof(new_insn), "adrp %s, 0x%" PRIx64, write_reg, patch_addr);
                                    uint64_t insn_addr = insn[i].address - kext_text_exec_seg->vmaddr + new_prelink_text_exec_base + kext->text_exec_off;
                                    // printf("%llx\n", insn_addr);
                                    if (ks_asm(ks, new_insn, insn_addr, &encode, &encode_size, &stat_count)) {
                                        printf("Can not ks_asm code %s(0x%" PRIx64 ")\n", new_insn, insn[i].address);
                                        exit(-1);
                                    } else {
                                        for (size_t j = 0; j < encode_size; j++) {
                                            ((unsigned char*)((uint64_t)prelink_text_exec_buf + insn_addr - new_prelink_text_exec_base))[j] = encode[j];
                                        }
                                        ks_free(encode);
                                    }

                                    // 处理匹配的off
                                    if (find_off) {
                                        for (const auto& consumer : relocated_offs) {
                                            int off_index = consumer.first;
                                            uint32_t relocated_off = consumer.second;
                                            bool assembled_consumer = false;
                                            if (!strcmp(insn[off_index].mnemonic, "ldraa")
                                                || !strcmp(insn[off_index].mnemonic, "ldrab")) {
                                                // Keystone used by this project predates the
                                                // authenticated-load mnemonics.  Their signed,
                                                // scaled imm9 is otherwise independent of the
                                                // destination and base register fields.
                                                if ((relocated_off & 7) || relocated_off > 4088) {
                                                    printf("Cannot encode %s displacement 0x%x at 0x%" PRIx64 "\n",
                                                        insn[off_index].mnemonic, relocated_off,
                                                        insn[off_index].address);
                                                    exit(-1);
                                                }
                                                uint32_t encoded = 0;
                                                memcpy(&encoded, insn[off_index].bytes, sizeof(encoded));
                                                encoded &= ~((UINT32_C(0x1ff) << 12) | (UINT32_C(1) << 22));
                                                encoded |= ((relocated_off >> 3) & UINT32_C(0x1ff)) << 12;
                                                uint64_t consumer_addr = insn[off_index].address
                                                    - kext_text_exec_seg->vmaddr
                                                    + new_prelink_text_exec_base + kext->text_exec_off;
                                                memcpy(reinterpret_cast<void*>(
                                                           reinterpret_cast<uint64_t>(prelink_text_exec_buf)
                                                           + consumer_addr - new_prelink_text_exec_base),
                                                    &encoded, sizeof(encoded));
                                                continue;
                                            } else if (!strcmp(insn[off_index].mnemonic, "add")) {
                                                int rcount = cs_op_count(handle, &insn[off_index], ARM64_OP_REG);
                                                const char* reg1_name;
                                                const char* reg2_name;
                                                if (rcount == 2) {
                                                    int reg1_index = cs_op_index(handle, &insn[off_index], ARM64_OP_REG, 1);
                                                    int reg2_index = cs_op_index(handle, &insn[off_index], ARM64_OP_REG, 2);
                                                    reg1_name = cs_reg_name(handle, insn[off_index].detail->arm64.operands[reg1_index].reg);
                                                    reg2_name = cs_reg_name(handle, insn[off_index].detail->arm64.operands[reg2_index].reg);

                                                    snprintf(new_insn, sizeof(new_insn), "add %s, %s, #0x%x", reg1_name, reg2_name, relocated_off);
                                                    assembled_consumer = true;
                                                }
                                            } else if (strstr(insn[off_index].mnemonic, "ldr")) {
                                                int reg1_index = cs_op_index(handle, &insn[off_index], ARM64_OP_REG, 1);
                                                const char* reg1_name = cs_reg_name(handle, insn[off_index].detail->arm64.operands[reg1_index].reg);
                                                int reg2_index = cs_op_index(handle, &insn[off_index], ARM64_OP_MEM, 1);
                                                const char* reg2_name = cs_reg_name(handle, insn[off_index].detail->arm64.operands[reg2_index].mem.base);

                                                snprintf(new_insn, sizeof(new_insn), "%s %s, [%s, #0x%x]", insn[off_index].mnemonic, reg1_name, reg2_name, relocated_off);
                                                assembled_consumer = true;
                                            } else if (strstr(insn[off_index].mnemonic, "str")) {
                                                int reg1_index = cs_op_index(handle, &insn[off_index], ARM64_OP_REG, 1);
                                                const char* reg1_name = cs_reg_name(handle, insn[off_index].detail->arm64.operands[reg1_index].reg);
                                                int reg2_index = cs_op_index(handle, &insn[off_index], ARM64_OP_MEM, 1);
                                                const char* reg2_name = cs_reg_name(handle, insn[off_index].detail->arm64.operands[reg2_index].mem.base);

                                                snprintf(new_insn, sizeof(new_insn), "%s %s, [%s, #0x%x]", insn[off_index].mnemonic, reg1_name, reg2_name, relocated_off);
                                                assembled_consumer = true;
                                            }

                                            if (!assembled_consumer)
                                                continue;
                                            insn_addr = insn[off_index].address - kext_text_exec_seg->vmaddr + new_prelink_text_exec_base + kext->text_exec_off;
                                            if (ks_asm(ks, new_insn, insn_addr, &encode, &encode_size, &stat_count)) {
                                                printf("Can not ks_asm code %s(0x%" PRIx64 ")\n", new_insn, insn[off_index].address);
                                                exit(-1);
                                            } else {
                                                for (size_t j = 0; j < encode_size; j++) {
                                                    ((unsigned char*)((uint64_t)prelink_text_exec_buf + insn_addr - new_prelink_text_exec_base))[j] = encode[j];
                                                }
                                                ks_free(encode);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (count > 0)
                        cs_free(insn, count);
                }

                // 处理外部符号
                struct dysymtab_command* kext_dysymtab = (struct dysymtab_command*)find_command((mach_header_64_t*)((uint64_t)prelink_text_buf + kext->text_off), LC_DYSYMTAB);
                if (kext->is_from_file && kext_dysymtab) {
                    segment_command_64_t* kext_linkedit_seg = (segment_command_64_t*)kext->exec_macho->find_segment("__LINKEDIT");
                    if (kext_linkedit_seg) {
                        struct relocation_info* ext_ri = (struct relocation_info*)((uint64_t)kext->exec_macho->file_buf + kext_dysymtab->extreloff);

                        for (uint32_t i = 0; i < kext_dysymtab->nextrel; i++) {
                            if (ext_ri[i].r_extern != 1 || ext_ri[i].r_type != 0
                                || ext_ri[i].r_pcrel || ext_ri[i].r_length != 3) {
                                ++unresolved_externals;
                                printf("Unsupported external relocation kind in %s\n", kext->kext_id);
                                continue;
                            }
                            if (ext_ri[i].r_extern == 1) {
                                const char* symbol_name = NULL;
                                if (ext_ri[i].r_symbolnum < kext->exec_macho->symbol_count)
                                    symbol_name = kext->exec_macho->symbol_list[ext_ri[i].r_symbolnum]->symbol_name;

                                if (!symbol_name) {
                                    ++unresolved_externals;
                                    printf("Invalid external relocation symbol index in %s\n", kext->kext_id);
                                    continue;
                                }

                                uint64_t found_addr = 0;
                                // Prefer an explicitly declared Kext dependency. The local
                                // kernel's old prelink segments are replaced below, so using
                                // one of their symbol addresses would leave a dangling link.
                                for (auto dep_kext : kext->depends) {
                                    Symbol* dependency_symbol = dep_kext->exec_macho
                                        ? dep_kext->exec_macho->find_symbol_by_name(symbol_name) : nullptr;
                                    if (!dependency_symbol || !dependency_symbol->symbol_addr)
                                        continue;
                                    segment_command_64_t* dep_text_seg = (segment_command_64_t*)dep_kext->exec_macho->find_segment("__TEXT");
                                    segment_command_64_t* dep_text_exec_seg = (segment_command_64_t*)dep_kext->exec_macho->find_segment("__TEXT_EXEC");
                                    segment_command_64_t* dep_data_seg = (segment_command_64_t*)dep_kext->exec_macho->find_segment("__DATA");
                                    segment_command_64_t* dep_data_const_seg = (segment_command_64_t*)dep_kext->exec_macho->find_segment("__DATA_CONST");

                                    found_addr = dependency_symbol->symbol_addr;
                                    if (dep_kext->exec_macho->addr_in_segment(dep_text_seg, found_addr)) {
                                        found_addr = found_addr - dep_text_seg->vmaddr + new_prelink_text_base + dep_kext->text_off;
                                    } else if (dep_kext->exec_macho->addr_in_segment(dep_text_exec_seg, found_addr)) {
                                        found_addr = found_addr - dep_text_exec_seg->vmaddr + new_prelink_text_exec_base + dep_kext->text_exec_off;
                                    } else if (dep_kext->exec_macho->addr_in_segment(dep_data_seg, found_addr)) {
                                        found_addr = found_addr - dep_data_seg->vmaddr + new_prelink_data_base + dep_kext->data_off;
                                    } else if (dep_kext->exec_macho->addr_in_segment(dep_data_const_seg, found_addr)) {
                                        found_addr = found_addr - dep_data_const_seg->vmaddr + new_prelink_data_const_base + dep_kext->data_const_off;
                                    } else if (!dep_kext->is_from_file
                                        && resolve_kernel_reference(dependency_symbol->symbol_addr,
                                            i_kernel, y_kernel, resolver, found_addr)) {
                                        // An audited native vtable alias can name an inherited
                                        // kernel method (e.g. IODMACommand::free). Resolve its
                                        // original address through the validated kernel map;
                                        // it has no storage in the dependency's segments.
                                    } else {
                                        found_addr = 0;
                                        continue;
                                    }
                                    break;
                                }

                                if (!found_addr) {
                                    Symbol* kernel_symbol = y_kernel.find_symbol_by_name(symbol_name);
                                    if (kernel_symbol && kernel_symbol->symbol_addr)
                                        found_addr = kernel_symbol->symbol_addr;
                                }
                                if (!found_addr) {
                                    Symbol* inherited = i_kernel.find_symbol_by_name(symbol_name);
                                    if (inherited && inherited->symbol_addr)
                                        resolve_kernel_reference(inherited->symbol_addr,
                                            i_kernel, y_kernel, resolver, found_addr);
                                }
                                if (!found_addr) {
                                    ++unresolved_externals;
                                    printf("Not found external symbol %s required by %s\n",
                                        symbol_name, kext->kext_id ? kext->kext_id : "<unknown>");
                                }

                                if (found_addr) {
                                    uint64_t* patch_symbol = NULL;
                                    if (kext->exec_macho->addr_in_segment(kext_data_const_seg, ext_ri[i].r_address)) {
                                        // patch_symbol -= kext_data_const_seg->vmaddr;
                                        // printf("PP: %llx\n", (ext_ri[i].r_address - kext_data_const_seg->vmaddr));
                                        patch_symbol = (uint64_t*)((uint64_t)prelink_data_const_buf + kext->data_const_off + ext_ri[i].r_address - kext_data_const_seg->vmaddr);
                                        *patch_symbol = found_addr;
                                        // printf("Patch addr: 0x%llx\n", (uint64_t)patch_symbol - (uint64_t)prelink_data_const_buf + new_prelink_data_const_base);
                                    } else if (kext->exec_macho->addr_in_segment(kext_data_seg, ext_ri[i].r_address)) {
                                        patch_symbol = (uint64_t*)((uint64_t)prelink_data_buf + kext->data_off + ext_ri[i].r_address - kext_data_seg->vmaddr);
                                        *patch_symbol = found_addr;
                                    } else {
                                        ++unresolved_externals;
                                        printf("Unsupported external relocation site for %s in %s\n", symbol_name, kext->kext_id);
                                    }
                                } else {

                                    // printf("Not found symbol %s\n", symbol_name);
                                }
                            }
                        }
                    }
                }

                if (kext->is_from_file && kext_dysymtab && kext_dysymtab->nlocrel) {
                    const auto& d = *kext_dysymtab;
                    if (d.locreloff > kext->exec_macho->file_size ||
                        d.nlocrel > (kext->exec_macho->file_size - d.locreloff) / sizeof(relocation_info)) {
                        printf("Local relocation table outside %s\n", kext->kext_id);
                        return SIZE_MAX;
                    }
                    std::vector<patcher_detail::LocalRebaseSegment> maps;
                    auto add_map = [&](segment_command_64_t* s, uint64_t destination, uint8_t* output) {
                        if (s) maps.push_back({s->vmaddr, s->vmsize, s->filesize, destination,
                            reinterpret_cast<const uint8_t*>(kext->exec_macho->file_buf) + s->fileoff, output});
                    };
                    add_map(kext_text_seg, new_prelink_text_base + kext->text_off,
                        reinterpret_cast<uint8_t*>(prelink_text_buf) + kext->text_off);
                    add_map(kext_text_exec_seg, new_prelink_text_exec_base + kext->text_exec_off,
                        reinterpret_cast<uint8_t*>(prelink_text_exec_buf) + kext->text_exec_off);
                    add_map(kext_data_seg, new_prelink_data_base + kext->data_off,
                        reinterpret_cast<uint8_t*>(prelink_data_buf) + kext->data_off);
                    add_map(kext_data_const_seg, new_prelink_data_const_base + kext->data_const_off,
                        reinterpret_cast<uint8_t*>(prelink_data_const_buf) + kext->data_const_off);
                    size_t invalid;
                    if (!patcher_detail::rebase_local_pointers(
                            reinterpret_cast<const relocation_info*>(kext->exec_macho->file_buf + d.locreloff),
                            d.nlocrel, maps.data(), maps.size(), kext_text_seg->vmaddr, &invalid)) {
                        printf("Invalid local pointer relocation %zu in %s\n", invalid, kext->kext_id);
                        return SIZE_MAX;
                    }
                }

                // 清空symtab
                struct symtab_command* kext_symtab = (struct symtab_command*)find_command((mach_header_64_t*)((uint64_t)prelink_text_buf + kext->text_off), LC_SYMTAB);
                // struct dysymtab_command* kext_dysymtab = (struct dysymtab_command*)find_command((mach_header_64_t*)((uint64_t)prelink_text_buf + kext->text_off), LC_DYSYMTAB);
                if (kext_symtab) {
                    printf("Found kext symtab\n");
                    kext_symtab->nsyms = 0;
                    kext_symtab->stroff = 0;
                    kext_symtab->strsize = 0;
                    kext_symtab->symoff = 0;
                }

                if (kext_dysymtab) {
                    printf("Found kext dysymtab\n");
                    memset((void*)((uint64_t)kext_dysymtab + 8), 0, sizeof(struct dysymtab_command) - 8);
                }
            }
        }

        ks_close(ks);
        cs_close(&handle);

        // 创建 PRELINK_INFO
        char* prelink_info_buf;
        uint64_t prelink_info_size;
        prelink_info_buf = make_prelink_info(y_kernel, prelink_info_size);
        uint64_t new_prelink_info_fileoff = new_prelink_data_fileoff + prelink_data_size;
        uint64_t new_kernel_size = new_prelink_info_fileoff + prelink_info_size;
        uint64_t new_prelink_info_base = new_prelink_data_base + prelink_data_size;

        // 修改内核基址
        patch_seg_base((segment_command_64_t*)y_kernel.find_segment("__PRELINK_TEXT"), new_prelink_text_fileoff, new_prelink_text_base, prelink_text_size);
        patch_seg_base((segment_command_64_t*)y_kernel.find_segment("__PLK_TEXT_EXEC"), new_prelink_text_exec_fileoff, new_prelink_text_exec_base, prelink_text_exec_size);
        patch_seg_base((segment_command_64_t*)y_kernel.find_segment("__PRELINK_DATA"), new_prelink_data_fileoff, new_prelink_data_base, prelink_data_size);
        patch_seg_base((segment_command_64_t*)y_kernel.find_segment("__PLK_DATA_CONST"), new_prelink_data_const_fileoff, new_prelink_data_const_base, prelink_data_const_size);
        patch_seg_base((segment_command_64_t*)y_kernel.find_segment("__PLK_LLVM_COV"), new_prelink_info_fileoff, new_prelink_info_base, 0);
        patch_seg_base((segment_command_64_t*)y_kernel.find_segment("__PLK_LINKEDIT"), new_prelink_info_fileoff, new_prelink_info_base, 0);
        patch_seg_base((segment_command_64_t*)y_kernel.find_segment("__PRELINK_INFO"), new_prelink_info_fileoff, new_prelink_info_base, prelink_info_size);

        // 修改Kext段基址
        // 20240526: fileoff也要修改
        for (auto kext : y_kernel.kexts) {
            if (kext->exec_macho) {
                {
                    mach_header_64_t* new_hdr = (mach_header_64_t*)((uint64_t)prelink_text_buf + kext->text_off);
                    struct load_command* lcd = (struct load_command*)(new_hdr + 1);
                    for (uint32_t i = 0; i < new_hdr->ncmds; i++) {
                        if (lcd->cmd == LC_SEGMENT_64) {
                            segment_command_64_t* seg = (segment_command_64_t*)lcd;
                            if (!strncmp(seg->segname, "__TEXT", 16)) {
                                patch_seg_vmbase((segment_command_64_t*)lcd, new_prelink_text_base + kext->text_off, seg->filesize);
                                printf("Kext %s new __TEXT at 0x%" PRIx64 "\n", kext->kext_id, new_prelink_text_base + kext->text_off);
                            } else if (!strncmp(seg->segname, "__TEXT_EXEC", 16)) {
                                patch_seg_vmbase((segment_command_64_t*)lcd, new_prelink_text_exec_base + kext->text_exec_off, seg->vmsize);
                                printf("Kext new __TEXT_EXEC at 0x%" PRIx64 "\n", new_prelink_text_exec_base + kext->text_exec_off);
                                patch_seg_fileoff((segment_command_64_t*)lcd, (new_prelink_text_exec_fileoff + kext->text_exec_off) - (new_prelink_text_fileoff + kext->text_off), seg->filesize);
                                // printf("%p -- %p\n", (kerenl_text_exec_off + kext->text_exec_off), (new_prelink_text_fileoff + kext->text_off));
                            } else if (!strncmp(seg->segname, "__DATA", 16)) {
                                patch_seg_vmbase((segment_command_64_t*)lcd, new_prelink_data_base + kext->data_off, seg->filesize);
                                printf("Kext new __DATA at 0x%" PRIx64 "\n", new_prelink_data_base + kext->data_off);
                                patch_seg_fileoff((segment_command_64_t*)lcd, (new_prelink_data_fileoff + kext->data_off) - (new_prelink_text_fileoff + kext->text_off), seg->filesize);
                            } else if (!strncmp(seg->segname, "__DATA_CONST", 16)) {
                                // A standalone -kext image keeps constants writable
                                // for external relocation. Those fixups are complete
                                // now; the embedded header must describe the same
                                // read-only mapping as the enclosing KC segment.
                                auto* container = static_cast<segment_command_64_t*>(
                                    y_kernel.find_segment("__PLK_DATA_CONST"));
                                if (!container || container->initprot != 1 ||
                                    (seg->initprot != 1 && seg->initprot != 3)) {
                                    printf("Incompatible constant segment protection\n");
                                    return SIZE_MAX;
                                }
                                seg->initprot = container->initprot;
                                patch_seg_vmbase((segment_command_64_t*)lcd, new_prelink_data_const_base + kext->data_const_off, seg->filesize);
                                printf("Kext new __DATA_CONST at 0x%" PRIx64 "\n", new_prelink_data_const_base + kext->data_const_off);
                                patch_seg_fileoff((segment_command_64_t*)lcd, (new_prelink_data_const_fileoff + kext->data_const_off) - (new_prelink_text_fileoff + kext->text_off), seg->filesize);
                            } else if (!strncmp(seg->segname, "__LINKEDIT", 16)) {
                                patch_seg_vmbase((segment_command_64_t*)lcd, new_prelink_info_base, 0);
                                patch_seg_fileoff((segment_command_64_t*)lcd, 0, 0);
                            }
                        } else if (lcd->cmd == 0x1e || lcd->cmd == 0x26 || lcd->cmd == 0x29
                            || lcd->cmd == 0x2b || lcd->cmd == 0x2e || lcd->cmd == 0x1d) {
                            // These payloads belonged to the removed Kext LINKEDIT.
                            // Retaining their old offsets makes the new image malformed.
                            if (lcd->cmdsize != 16)
                                return SIZE_MAX;
                            memset(reinterpret_cast<char*>(lcd) + 8, 0, 8);
                        } else if (lcd->cmd == LC_SEGMENT) {
                        }
                        lcd = (struct load_command*)((uint64_t)lcd + lcd->cmdsize);
                    }
                }
            }
        }
        // 最后将段复制到内核中
        printf("New kernel filesize: 0x%" PRIx64 "\n", new_kernel_size);
        y_kernel.file_buf = (char*)realloc(y_kernel.file_buf, new_kernel_size);
        y_kernel.file_size = new_kernel_size;
        memcpy((void*)((uint64_t)y_kernel.file_buf + new_prelink_text_fileoff), prelink_text_buf, prelink_text_size);
        memcpy((void*)((uint64_t)y_kernel.file_buf + new_prelink_text_exec_fileoff), prelink_text_exec_buf, prelink_text_exec_size);
        memcpy((void*)((uint64_t)y_kernel.file_buf + new_prelink_data_const_fileoff), prelink_data_const_buf, prelink_data_const_size);
        memcpy((void*)((uint64_t)y_kernel.file_buf + new_prelink_data_fileoff), prelink_data_buf, prelink_data_size);
        memcpy((void*)((uint64_t)y_kernel.file_buf + new_prelink_info_fileoff), prelink_info_buf, prelink_info_size);

        free(prelink_text_buf);
        free(prelink_text_exec_buf);
        free(prelink_data_const_buf);
        free(prelink_data_buf);
        free(prelink_info_buf);
    }
    return unresolved_externals;
}

uint32_t patch_ios_kernel(KernelMacho& kernel, const char* patch_path)
{
    std::ifstream patch_file_fs(patch_path);
    if (!patch_file_fs) {
        printf("Faild to load kernel patch list file %s!\n", patch_path);
        return 1;
    }

    char tmp_line[255];
    uint64_t dst_addr = 0;
    uint32_t file_off = 0;
    uint32_t length;
    uint32_t value = 0;

    while (patch_file_fs.getline(tmp_line, 255)) {
        if (tmp_line[0] == '#') {
            continue;
        } else if (tmp_line[0] == '*') {
            // 目标地址
            unsigned long long parsed_addr = 0;
            if (sscanf(tmp_line, "*0x%llx:", &parsed_addr) <= 0) {
                return 1;
            }
            dst_addr = static_cast<uint64_t>(parsed_addr);

            file_off = kernel.get_fileoff(dst_addr);
            if (!file_off) {
                printf("Faild to get dst addr 0x%" PRIx64 " fileoff\n", dst_addr);
                return 1;
            }
        } else if (tmp_line[0] == '+') {
            // 补丁内容
            length = strlen(tmp_line);
            if ((length - 1) % 2 == 0) {
                sscanf(tmp_line, "+%x", &value);
                // printf("Patching 0x%llx(0x%x)\n", dst_addr, file_off);
                // printf("%x\n", *((uint32_t*)((uint64_t)kernel.file_buf + file_off)));
                *((uint32_t*)((uint64_t)kernel.file_buf + file_off)) = value;
                file_off += sizeof(uint32_t);
                // printf("%x\n", n);
            }
        }
    }

    return 0;
}
