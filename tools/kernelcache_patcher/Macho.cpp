#include <Macho.h>

#include <cxxabi.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {

static std::string symbol_alias(const std::string& name)
{
    if (name.size() > 3 && name[0] == '_' && name[1] == '_' && name[2] == 'Z') {
        int status = 0;
        char* demangled = abi::__cxa_demangle(name.c_str() + 1, nullptr, nullptr, &status);
        if (status == 0 && demangled) {
            std::string result(demangled);
            free(demangled);
            return result;
        }
        free(demangled);
    }

    // Mach-O adds a leading underscore to C external symbols.
    if (name.size() > 1 && name[0] == '_' && name[1] != '_')
        return name.substr(1);
    return name;
}

static void skip_ws(const std::string& input, size_t& pos)
{
    while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos])))
        ++pos;
}

static bool parse_json_string(const std::string& input, size_t& pos, std::string& result)
{
    skip_ws(input, pos);
    if (pos >= input.size() || input[pos++] != '"')
        return false;

    result.clear();
    while (pos < input.size()) {
        char c = input[pos++];
        if (c == '"')
            return true;
        if (c != '\\') {
            result.push_back(c);
            continue;
        }
        if (pos >= input.size())
            return false;
        char escaped = input[pos++];
        switch (escaped) {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u': {
            // Symbol names are normally ASCII. Preserve BMP escapes as UTF-8.
            if (pos + 4 > input.size())
                return false;
            unsigned value = 0;
            for (int i = 0; i < 4; ++i) {
                char digit = input[pos++];
                value <<= 4;
                if (digit >= '0' && digit <= '9') value |= digit - '0';
                else if (digit >= 'a' && digit <= 'f') value |= digit - 'a' + 10;
                else if (digit >= 'A' && digit <= 'F') value |= digit - 'A' + 10;
                else return false;
            }
            if (value <= 0x7f) result.push_back(static_cast<char>(value));
            else if (value <= 0x7ff) {
                result.push_back(static_cast<char>(0xc0 | (value >> 6)));
                result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
            } else {
                result.push_back(static_cast<char>(0xe0 | (value >> 12)));
                result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
                result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
            }
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

} // namespace

void Macho::init_symbols()
{
    struct symtab_command* k_symtab = (struct symtab_command*)find_command(LC_SYMTAB);
    if (!k_symtab) {
        return;
    }

    uint64_t symbols_size = static_cast<uint64_t>(k_symtab->nsyms) * sizeof(nlist_64);
    if (k_symtab->symoff > file_size || symbols_size > file_size - k_symtab->symoff
        || k_symtab->stroff > file_size || k_symtab->strsize > file_size - k_symtab->stroff) {
        printf("Mach-o symbol table is outside the file\n");
        return;
    }

    struct nlist_64* sym_list = (struct nlist_64*)((uint64_t)this->file_buf + k_symtab->symoff);
    symbol_count = k_symtab->nsyms;
    void* symstr = (void*)((uint64_t)file_buf + k_symtab->stroff);
    printf("Mach-o has %d symbols\n", symbol_count);

    for (uint32_t i = 0; i < symbol_count; i++) {
        Symbol* sym = new Symbol();
        sym->symbol_addr = sym_list[i].n_value;
        sym->symbol_name = sym_list[i].n_un.n_strx < k_symtab->strsize
            ? (const char*)((uint64_t)symstr + sym_list[i].n_un.n_strx) : nullptr;

        if (sym->symbol_addr)
            symbol_addr_map[sym->symbol_addr] = sym;
        if (sym->symbol_name && sym->symbol_name[0]) {
            symbol_name_map[sym->symbol_name] = sym;
            std::string alias = symbol_alias(sym->symbol_name);
            if (alias != sym->symbol_name)
                symbol_alias_map.emplace(alias, sym);
        }
        symbol_list.push_back(sym);
    }
}

Symbol* Macho::add_symbol(uint64_t address, const std::string& name, bool overwrite)
{
    if (name.empty())
        return nullptr;

    auto by_name = symbol_name_map.find(name);
    if (!overwrite && by_name != symbol_name_map.end())
        return by_name->second;

    owned_symbol_names.push_back(name);
    Symbol* symbol = new Symbol { owned_symbol_names.back().c_str(), address };
    symbol_list.push_back(symbol);
    // Multiple C++ symbols legitimately alias the same function address
    // (C1/C2, D1/D2 and inherited vtable slots). Preserve the first stable
    // address label while still adding every name to the name index.
    if (address)
        symbol_addr_map.emplace(address, symbol);
    if (overwrite)
        symbol_name_map[name] = symbol;
    else
        symbol_name_map.emplace(name, symbol);

    std::string alias = symbol_alias(name);
    if (overwrite)
        symbol_alias_map[alias] = symbol;
    else
        symbol_alias_map.emplace(alias, symbol);
    return symbol;
}

Symbol* Macho::find_symbol_by_name(const std::string& name) const
{
    auto exact = symbol_name_map.find(name);
    if (exact != symbol_name_map.end())
        return exact->second;
    auto alias = symbol_alias_map.find(name);
    if (alias != symbol_alias_map.end())
        return alias->second;
    return nullptr;
}

Symbol* Macho::find_symbol_by_addr(uint64_t address) const
{
    auto found = symbol_addr_map.find(address);
    return found == symbol_addr_map.end() ? nullptr : found->second;
}

size_t Macho::load_symbols_json(const char* path, bool overwrite)
{
    if (!path)
        return 0;
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return 0;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    std::string input = buffer.str();

    size_t pos = 0;
    skip_ws(input, pos);
    if (pos >= input.size() || input[pos++] != '{')
        return 0;

    size_t loaded = 0;
    while (pos < input.size()) {
        skip_ws(input, pos);
        if (pos < input.size() && input[pos] == '}')
            return loaded;

        std::string address_string;
        std::string name;
        if (!parse_json_string(input, pos, address_string))
            return 0;
        skip_ws(input, pos);
        if (pos >= input.size() || input[pos++] != ':')
            return 0;
        if (!parse_json_string(input, pos, name))
            return 0;

        char* end = nullptr;
        errno = 0;
        unsigned long long address = std::strtoull(address_string.c_str(), &end, 0);
        if (!errno && end && *end == '\0' && address && is_mapped_address(address)) {
            add_symbol(static_cast<uint64_t>(address), name, overwrite);
            ++loaded;
        }

        skip_ws(input, pos);
        if (pos < input.size() && input[pos] == ',') {
            ++pos;
            continue;
        }
        if (pos < input.size() && input[pos] == '}')
            return loaded;
        return 0;
    }
    return 0;
}

void Macho::format_macho()
{
    // printf("Formating %s\n", file_path);

    if (!file_buf || file_size < sizeof(mach_header_t)) {
        printf("Macho: File is empty or truncated\n");
        return;
    }
    header = (mach_header_64_t*)file_buf;

    if (header->magic == MH_MAGIC_64) {
        is_64 = true;
    } else if (header->magic == MH_MAGIC) {
        is_64 = false;
    } else {
        printf("Macho: Unknown magic 0x%08x!\n", header->magic);
        header = nullptr;
        return;
    }
}

void Macho::copy_from_file(uint64_t offset, char* targetBuff, size_t size)
{
    if (targetBuff || size) {
        if (filefs.is_open()) {
            filefs.seekg(offset, std::ios::beg);
            filefs.read(targetBuff, size);
        }
    }
}

uint32_t Macho::get_file_size()
{
    uint32_t result = 0;
    if (filefs.is_open()) {
        filefs.seekg(0, std::ios::end);
        result = filefs.tellg();
        filefs.seekg(0, std::ios::beg);
    }

    return result;
}

load_command* find_command(mach_header_64_t* header, uint32_t cmd)
{
    load_command* lcd = NULL;
    if (header->magic == MH_MAGIC_64) {
        lcd = (load_command*)(header + 1);
    } else {
        lcd = (load_command*)((mach_header_t*)header + 1);
    }

    for (uint32_t i = 0; i < header->ncmds; i++) {
        if (lcd->cmd == cmd) {
            return lcd;
        }

        lcd = (load_command*)((uint64_t)lcd + lcd->cmdsize);
    }

    return NULL;
}

load_command* Macho::find_command(uint32_t cmd)
{
    if (!header)
        return nullptr;
    load_command* lcd = NULL;
    if (is_64) {
        lcd = (load_command*)(header + 1);
    } else {
        lcd = (load_command*)((mach_header_t*)header + 1);
    }

    for (uint32_t i = 0; i < header->ncmds; i++) {
        if (lcd->cmd == cmd) {
            return lcd;
        }

        lcd = (load_command*)((uint64_t)lcd + lcd->cmdsize);
    }

    return NULL;
}

void* Macho::find_section(const char* segname, const char* section_name)
{
    segment_command_64_t* seg = (segment_command_64_t*)find_segment(segname);
    if (seg) {
        section_64_t* sect = (section_64_t*)((uint64_t)seg + sizeof(segment_command_64_t));
        for (uint32_t i = 0; i < seg->nsects; i++) {
            if (!strncmp(sect->sectname, section_name, 16)) {
                return sect;
            }
            sect = (section_64_t*)(sect + 1);
        }
    }

    return NULL;
}

void* Macho::find_segment(const char* segment_name)
{
    if (!header)
        return nullptr;
    load_command* lcd = NULL;
    if (is_64) {
        lcd = (load_command*)(header + 1);
    } else {
        lcd = (load_command*)((mach_header_t*)header + 1);
    }
    for (uint32_t i = 0; i < header->ncmds; i++) {
        if (is_64) {
            if (lcd->cmd == LC_SEGMENT_64) {
                segment_command_64_t* seg_cmd = (segment_command_64_t*)lcd;
                if (!strncmp(seg_cmd->segname, segment_name, 16)) {
                    return seg_cmd;
                }
            }
        } else {
            if (lcd->cmd == LC_SEGMENT) {
                segment_command_t* seg_cmd = (segment_command_t*)lcd;
                if (!strncmp(seg_cmd->segname, segment_name, 16)) {
                    return seg_cmd;
                }
            }
        }

        lcd = (load_command*)((uint64_t)lcd + lcd->cmdsize);
    }

    return NULL;
}

bool Macho::addr_in_segment(const char *segment_name, uint64_t addr)
{
    segment_command_64_t *found_segment = (segment_command_64_t *)find_segment(segment_name);
    if(!found_segment)
        return false;
    
    return (addr >= found_segment->vmaddr && addr < found_segment->vmaddr + found_segment->vmsize);
}

bool Macho::addr_in_segment(segment_command_64_t *seg, uint64_t addr)
{
    if(!seg)
        return false;
    return (addr >= seg->vmaddr && addr < seg->vmaddr + seg->vmsize);
}

uint32_t Macho::get_fileoff(uint64_t vmaddr)
{
    if (!header)
        return 0;
    load_command* lcd = NULL;
    uint32_t fileoff = 0;
    if (is_64) {
        lcd = (load_command*)(header + 1);
    } else {
        lcd = (load_command*)((mach_header_t*)header + 1);
    }
    for (uint32_t i = 0; i < header->ncmds; i++) {
        if (is_64) {
            if (lcd->cmd == LC_SEGMENT_64) {
                segment_command_64_t* seg_cmd = (segment_command_64_t*)lcd;
                if(vmaddr >= seg_cmd->vmaddr && vmaddr < seg_cmd->vmaddr + seg_cmd->vmsize) {
                    fileoff = vmaddr - seg_cmd->vmaddr + seg_cmd->fileoff;
                    break;
                }
            }
        } else {
            if (lcd->cmd == LC_SEGMENT) {
                segment_command_t* seg_cmd = (segment_command_t*)lcd;
                if(vmaddr >= seg_cmd->vmaddr && vmaddr < seg_cmd->vmaddr + seg_cmd->vmsize) {
                    fileoff = vmaddr - seg_cmd->vmaddr + seg_cmd->fileoff;
                    break;
                }
            }
        }

        lcd = (load_command*)((uint64_t)lcd + lcd->cmdsize);
    }

    return fileoff;
}

bool Macho::vmaddr_to_fileoff(uint64_t vmaddr, uint64_t& fileoff, size_t size) const
{
    if (!header || !file_buf || size > file_size)
        return false;

    load_command* command = is_64
        ? reinterpret_cast<load_command*>(header + 1)
        : reinterpret_cast<load_command*>(reinterpret_cast<mach_header_t*>(header) + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (command->cmd == LC_SEGMENT_64) {
            const segment_command_64_t* segment = reinterpret_cast<const segment_command_64_t*>(command);
            if (vmaddr >= segment->vmaddr && vmaddr - segment->vmaddr < segment->filesize) {
                uint64_t delta = vmaddr - segment->vmaddr;
                if (delta <= segment->filesize && size <= segment->filesize - delta
                    && segment->fileoff <= file_size && delta <= file_size - segment->fileoff
                    && size <= file_size - segment->fileoff - delta) {
                    fileoff = segment->fileoff + delta;
                    return true;
                }
            }
        }
        command = reinterpret_cast<load_command*>(reinterpret_cast<uint8_t*>(command) + command->cmdsize);
    }
    return false;
}

bool Macho::read_u64_vm(uint64_t vmaddr, uint64_t& value) const
{
    uint64_t fileoff = 0;
    if (!vmaddr_to_fileoff(vmaddr, fileoff, sizeof(value)))
        return false;
    memcpy(&value, file_buf + fileoff, sizeof(value));
    return true;
}

bool Macho::read_u32_vm(uint64_t vmaddr, uint32_t& value) const
{
    uint64_t fileoff = 0;
    if (!vmaddr_to_fileoff(vmaddr, fileoff, sizeof(value)))
        return false;
    memcpy(&value, file_buf + fileoff, sizeof(value));
    return true;
}

bool Macho::read_cstring_vm(uint64_t vmaddr, std::string& value, size_t max_length) const
{
    value.clear();
    for (size_t i = 0; i < max_length; ++i) {
        uint64_t fileoff = 0;
        if (!vmaddr_to_fileoff(vmaddr + i, fileoff, 1))
            return false;
        char byte = file_buf[fileoff];
        if (!byte)
            return !value.empty();
        value.push_back(byte);
    }
    return false;
}

std::vector<const segment_command_64_t*> Macho::segments() const
{
    std::vector<const segment_command_64_t*> result;
    if (!header || !is_64)
        return result;
    const load_command* command = reinterpret_cast<const load_command*>(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (command->cmd == LC_SEGMENT_64)
            result.push_back(reinterpret_cast<const segment_command_64_t*>(command));
        command = reinterpret_cast<const load_command*>(reinterpret_cast<const uint8_t*>(command) + command->cmdsize);
    }
    return result;
}

bool Macho::is_mapped_address(uint64_t vmaddr) const
{
    if (!header)
        return false;
    load_command* command = is_64
        ? reinterpret_cast<load_command*>(header + 1)
        : reinterpret_cast<load_command*>(reinterpret_cast<mach_header_t*>(header) + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (command->cmd == LC_SEGMENT_64) {
            const segment_command_64_t* segment = reinterpret_cast<const segment_command_64_t*>(command);
            if (vmaddr >= segment->vmaddr && vmaddr - segment->vmaddr < segment->vmsize)
                return true;
        }
        command = reinterpret_cast<load_command*>(reinterpret_cast<uint8_t*>(command) + command->cmdsize);
    }
    return false;
}

bool Macho::is_executable_address(uint64_t vmaddr) const
{
    if (!header)
        return false;
    load_command* command = is_64
        ? reinterpret_cast<load_command*>(header + 1)
        : reinterpret_cast<load_command*>(reinterpret_cast<mach_header_t*>(header) + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (command->cmd == LC_SEGMENT_64) {
            const segment_command_64_t* segment = reinterpret_cast<const segment_command_64_t*>(command);
            if ((segment->initprot & 4) && vmaddr >= segment->vmaddr
                && vmaddr - segment->vmaddr < segment->vmsize)
                return true;
        }
        command = reinterpret_cast<load_command*>(reinterpret_cast<uint8_t*>(command) + command->cmdsize);
    }
    return false;
}

Macho::Macho()
{
}

Macho::Macho(char* buf, uint32_t file_size)
    : file_buf(buf)
    , file_size(file_size)
{
}

Macho::Macho(const char* path)
{
    if (path) {
        filefs.open(path, std::ios::binary | std::ios::in);
        if (!filefs.is_open()) {
            printf("Macho file %s was not open!\n", path);
            return;
        }

        strcpy(file_path, path);

        file_size = get_file_size();
        file_buf = (char*)malloc(file_size);
        filefs.read(file_buf, file_size);
        filefs.close();
    }
}

Macho::~Macho()
{
    filefs.close();
}
