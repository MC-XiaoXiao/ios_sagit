#include <assert.h>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>
#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>
#include <unordered_set>
#include <vector>

#include <CppAnalyzer.h>
#include <KernelMacho.h>
#include <KextMacho.h>
#include <Macho.h>
#include <Patcher.h>
#include <Plist.h>
#include <Resolver.h>

#define PATH_LENGTH 255

void format_prelink_info(KernelMacho& kernel);
Kext* load_kext_from_file(const char* path);
uint32_t get_filesize(std::ifstream& fd);
void init_kext_depends(KernelMacho& kernel, Kext* kext);
void useage();

static std::filesystem::path find_offline_verifier()
{
    std::vector<std::filesystem::path> roots { std::filesystem::current_path() };
    std::error_code error;
    auto executable = std::filesystem::canonical("/proc/self/exe", error);
    if (!error)
        roots.push_back(executable.parent_path());
    for (auto current : roots) {
        for (unsigned depth = 0; depth < 8; ++depth) {
            auto candidate = current / "scripts" / "verify-kernelcache.py";
            if (std::filesystem::is_regular_file(candidate))
                return candidate;
            if (current == current.parent_path())
                break;
            current = current.parent_path();
        }
    }
    return {};
}

static bool verify_and_publish(const char* output, const char* source, const char* local, const KernelMacho& kernel)
{
    auto verifier = find_offline_verifier();
    if (verifier.empty()) {
        fprintf(stderr, "Required scripts/verify-kernelcache.py is missing; no output published.\n");
        return false;
    }
    std::string pattern = std::string(output) + ".pending.XXXXXX";
    std::vector<char> temporary(pattern.begin(), pattern.end());
    temporary.push_back(0);
    int fd = mkstemp(temporary.data());
    if (fd < 0) {
        perror("Cannot create temporary kernelcache");
        return false;
    }
    close(fd);
    std::ofstream stream(temporary.data(), std::ios::binary | std::ios::trunc);
    stream.write(kernel.file_buf, kernel.file_size);
    stream.close();
    if (!stream) {
        unlink(temporary.data());
        fprintf(stderr, "Cannot write complete temporary kernelcache.\n");
        return false;
    }
    // Lower Apple Kext ISA extensions into mapped ARMv8.0 kernel helpers
    // before the mandatory independent structural/ISA gate and atomic publish.
    auto lowerer = verifier.parent_path() / "armv8_compat.py";
    fflush(nullptr);
    pid_t lowering_child = fork();
    if (lowering_child == 0) {
        execlp("python3", "python3", lowerer.c_str(), "lower", temporary.data(), static_cast<char*>(nullptr));
        perror("Cannot execute required ARMv8 ISA lowerer");
        _exit(127);
    }
    int lowering_status = 0;
    pid_t lowering_waited = -1;
    if (lowering_child > 0) {
        do { lowering_waited = waitpid(lowering_child, &lowering_status, 0); }
        while (lowering_waited < 0 && errno == EINTR);
    }
    if (lowering_waited < 0 || !WIFEXITED(lowering_status) || WEXITSTATUS(lowering_status) != 0) {
        unlink(temporary.data());
        fprintf(stderr, "ARMv8 ISA lowering failed; no output published.\n");
        return false;
    }
    // Finalize the module digest after every relocation and ISA replacement;
    // the original corecrypto POST code and algorithm tests remain enabled.
    auto integrity = verifier.parent_path() / "kernelcache_integrity.py";
    pid_t integrity_child = fork();
    if (integrity_child == 0) {
        execlp("python3", "python3", integrity.c_str(), temporary.data(),
            "--source-kernelcache", source, static_cast<char*>(nullptr));
        perror("Cannot execute required module HMAC finalizer");
        _exit(127);
    }
    int integrity_status = 0;
    pid_t integrity_waited = -1;
    if (integrity_child > 0) {
        do { integrity_waited = waitpid(integrity_child, &integrity_status, 0); }
        while (integrity_waited < 0 && errno == EINTR);
    }
    if (integrity_waited < 0 || !WIFEXITED(integrity_status) || WEXITSTATUS(integrity_status) != 0) {
        unlink(temporary.data());
        fprintf(stderr, "Module HMAC finalization failed; no output published.\n");
        return false;
    }
    std::vector<std::string> arguments { "python3", verifier.string(), temporary.data(),
        "--report", std::string(output) + ".verification.json", "--source-kernelcache", source,
        "--local-kernel", local };
    for (const auto* kext : kernel.kexts) {
        arguments.push_back("--expected-id");
        arguments.push_back(kext->kext_id);
        if (kext->is_from_file && kext->exec_macho) {
            arguments.push_back("--local-kext");
            arguments.push_back(kext->kext_id);
            arguments.push_back(kext->exec_macho->path());
        }
    }
    std::vector<char*> argv;
    for (auto& argument : arguments)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    fflush(nullptr);
    pid_t child = fork();
    if (child == 0) {
        execvp(argv[0], argv.data());
        perror("Cannot execute required offline verifier");
        _exit(127);
    }
    int status = 0;
    pid_t waited = -1;
    if (child > 0) {
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    }
    if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        unlink(temporary.data());
        fprintf(stderr, "Offline validation failed; no output published.\n");
        return false;
    }
    if (rename(temporary.data(), output)) {
        perror("Cannot atomically publish verified kernelcache");
        unlink(temporary.data());
        return false;
    }
    return true;
}

static bool add_symbol_to_containing_kext(const RecoveredSymbol& recovered, const std::vector<Kext*>& kexts)
{
    for (Kext* kext : kexts) {
        if (!kext || kext->is_from_file || !kext->exec_macho)
            continue;
        for (const char* segment_name : { "__TEXT", "__TEXT_EXEC", "__DATA", "__DATA_CONST" }) {
            auto* segment = static_cast<segment_command_64_t*>(kext->exec_macho->find_segment(segment_name));
            if (kext->exec_macho->addr_in_segment(segment, recovered.address)) {
                kext->exec_macho->add_symbol(recovered.address, recovered.name, true);
                return true;
            }
        }
    }
    return false;
}

static std::filesystem::path find_reference_root()
{
    if (const char* configured = getenv("KERNELCACHE_PATCHER_REFERENCE_DIR")) {
        std::filesystem::path path(configured);
        if (std::filesystem::is_directory(path))
            return path;
    }
    std::filesystem::path current = std::filesystem::current_path();
    for (unsigned depth = 0; depth < 8; ++depth) {
        std::filesystem::path candidate = current / "kexts" / "reference";
        if (std::filesystem::is_directory(candidate))
            return candidate;
        if (!current.has_parent_path() || current == current.parent_path())
            break;
        current = current.parent_path();
    }
    return {};
}

static int analyze_resolver(int argc, char** argv)
{
    if (argc < 4) {
        printf("kernelcache_patcher analyze <local-unstripped-kernel> <release-kernel> [release-symbols.json] [--native-only] [--details]\n");
        return 1;
    }

    KernelMacho target(argv[2]);
    KernelMacho source(argv[3]);
    target.format_macho();
    source.format_macho();
    target.init_symbols();
    source.init_symbols();

    bool details = false;
    bool native_only = false;
    std::string symbols_path = std::string(argv[3]) + ".symbols.json";
    for (int i = 4; i < argc; ++i) {
        if (!strcmp(argv[i], "--details"))
            details = true;
        else if (!strcmp(argv[i], "--native-only"))
            native_only = true;
        else
            symbols_path = argv[i];
    }
    size_t loaded = native_only ? 0 : source.load_symbols_json(symbols_path.c_str());
    printf("Loaded %zu release symbols from %s\n", loaded, symbols_path.c_str());

    CppClassAnalyzer cpp_analyzer(source, target);
    CppAnalyzerStats cpp_stats = native_only ? CppAnalyzerStats{} : cpp_analyzer.recover();
    printf("Stripped C++ analyzer: %zu registrations, %zu instance vtables, %zu named slots, %zu symbols\n",
        cpp_stats.registrations, cpp_stats.classes_with_vtables, cpp_stats.named_virtual_slots,
        cpp_stats.injected_symbols);

    LinkResolver resolver(source, target);
    ResolverStats stats = resolver.build();
    printf("Resolver: %zu named symbols, %zu vtables (%zu rejected), %zu virtual slots, %zu ambiguous addresses\n",
        stats.named_symbols, stats.matched_vtables, stats.rejected_vtables, stats.matched_vtable_slots,
        stats.ambiguous_addresses);
    for (const VtableRejection& rejected : resolver.rejections()) {
        printf("REJECT %s: %s, slot=%zu, source-vtable=0x%llx, target-vtable=0x%llx, source-slots=%zu, target-slots=%zu\n",
            rejected.name.c_str(), rejected.reason.c_str(), rejected.slot,
            static_cast<unsigned long long>(rejected.source_address),
            static_cast<unsigned long long>(rejected.target_address), rejected.source_slots, rejected.target_slots);
        printf("  source=0x%llx %s; target=0x%llx %s; expected=0x%llx\n",
            static_cast<unsigned long long>(rejected.source_pointer), rejected.source_symbol.c_str(),
            static_cast<unsigned long long>(rejected.target_pointer), rejected.target_symbol.c_str(),
            static_cast<unsigned long long>(rejected.expected_pointer));
    }
    if (details) {
        for (const RecoveredClass& recovered : cpp_analyzer.classes()) {
            printf("class %s : %s, meta=0x%llx, registration=0x%llx, vtable=0x%llx, slots=%zu, instance-size=%u\n",
                recovered.name.c_str(), recovered.super_name.empty() ? "<unknown>" : recovered.super_name.c_str(),
                static_cast<unsigned long long>(recovered.meta_object),
                static_cast<unsigned long long>(recovered.registration_call),
                static_cast<unsigned long long>(recovered.vtable), recovered.slots.size(), recovered.instance_size);
            if (recovered.registration_calls.size() > 1) {
                printf("  registration calls:");
                for (uint64_t call : recovered.registration_calls)
                    printf(" 0x%llx", static_cast<unsigned long long>(call));
                printf("\n");
            }
        }
        for (const VtableMatch& vtable : resolver.vtables()) {
            printf("%s: 0x%llx -> 0x%llx (%zu slots)\n", vtable.name.c_str(),
                static_cast<unsigned long long>(vtable.source_address),
                static_cast<unsigned long long>(vtable.target_address), vtable.target_slots.size());
            for (size_t slot = 0; slot < vtable.target_slots.size(); ++slot) {
                Symbol* symbol = target.find_symbol_by_addr(vtable.target_slots[slot]);
                printf("  [%3zu] 0x%llx %s\n", slot,
                    static_cast<unsigned long long>(vtable.target_slots[slot]),
                    symbol && symbol->symbol_name ? symbol->symbol_name : "<unnamed>");
            }
        }
    }
    return stats.matched_vtables ? 0 : 2;
}

static uint64_t normalize_macho_pointer(const Macho& image, uint64_t pointer)
{
    if (image.is_mapped_address(pointer))
        return pointer;

    // arm64e vtable entries can contain high PAC metadata.  Only accept the
    // canonicalized value when it maps back into this Mach-O image.
    uint64_t canonical = pointer & 0x0000ffffffffffffULL;
    if (canonical & (1ULL << 47))
        canonical |= 0xffff000000000000ULL;
    return image.is_mapped_address(canonical) ? canonical : pointer;
}

static Symbol* relocation_symbol_at(const Macho& image, uint64_t address, bool& has_relocation)
{
    has_relocation = false;
    if (!image.header || !image.is_64 || !image.file_buf)
        return nullptr;

    if (const auto* dysymtab = reinterpret_cast<const dysymtab_command*>(
            const_cast<Macho&>(image).find_command(LC_DYSYMTAB))) {
        auto scan_table = [&](uint32_t offset, uint32_t count) -> Symbol* {
            const uint64_t bytes = static_cast<uint64_t>(count) * sizeof(relocation_info);
            if (offset > image.file_size || bytes > image.file_size - offset)
                return nullptr;
            const auto* relocations = reinterpret_cast<const relocation_info*>(image.file_buf + offset);
            for (uint32_t index = 0; index < count; ++index) {
                const relocation_info& relocation = relocations[index];
                if (relocation.r_address < 0
                    || static_cast<uint64_t>(relocation.r_address) != address)
                    continue;
                has_relocation = true;
                if (relocation.r_extern && relocation.r_symbolnum < image.symbol_list.size())
                    return image.symbol_list[relocation.r_symbolnum];
                return nullptr;
            }
            return nullptr;
        };
        if (Symbol* symbol = scan_table(dysymtab->extreloff, dysymtab->nextrel))
            return symbol;
        if (has_relocation)
            return nullptr;
        if (Symbol* symbol = scan_table(dysymtab->locreloff, dysymtab->nlocrel))
            return symbol;
        if (has_relocation)
            return nullptr;
    }

    const load_command* command = reinterpret_cast<const load_command*>(image.header + 1);
    for (uint32_t i = 0; i < image.header->ncmds; ++i) {
        if (command->cmd == LC_SEGMENT_64) {
            const auto* segment = reinterpret_cast<const segment_command_64_t*>(command);
            const auto* section = reinterpret_cast<const section_64_t*>(segment + 1);
            for (uint32_t section_index = 0; section_index < segment->nsects; ++section_index, ++section) {
                if (address < section->addr || address >= section->addr + section->size)
                    continue;
                const uint64_t reloc_size = static_cast<uint64_t>(section->nreloc) * sizeof(relocation_info);
                if (section->reloff > image.file_size || reloc_size > image.file_size - section->reloff)
                    return nullptr;
                const auto* relocations = reinterpret_cast<const relocation_info*>(image.file_buf + section->reloff);
                const uint64_t section_offset = address - section->addr;
                for (uint32_t reloc_index = 0; reloc_index < section->nreloc; ++reloc_index) {
                    const relocation_info& relocation = relocations[reloc_index];
                    // MH_OBJECT encodes a section-relative address, while the
                    // MH_KEXT_BUNDLE files used here encode the image VM
                    // address.  Accept either representation.
                    if (relocation.r_address < 0
                        || (static_cast<uint64_t>(relocation.r_address) != section_offset
                            && static_cast<uint64_t>(relocation.r_address) != address))
                        continue;
                    has_relocation = true;
                    if (relocation.r_extern && relocation.r_symbolnum < image.symbol_list.size())
                        return image.symbol_list[relocation.r_symbolnum];
                    return nullptr;
                }
                return nullptr;
            }
        }
        command = reinterpret_cast<const load_command*>(
            reinterpret_cast<const uint8_t*>(command) + command->cmdsize);
    }
    return nullptr;
}

static int dump_vtable(int argc, char** argv)
{
    if (argc < 4) {
        printf("kernelcache_patcher dump-vtable <unstripped-macho> <vtable-symbol>\n");
        return 1;
    }

    KextMacho image(argv[2]);
    image.format_macho();
    image.init_symbols();
    Symbol* vtable = image.find_symbol_by_name(argv[3]);
    if (!vtable || !vtable->symbol_addr) {
        printf("Vtable symbol %s was not found\n", argv[3]);
        return 2;
    }

    printf("%s at 0x%llx\n", argv[3],
        static_cast<unsigned long long>(vtable->symbol_addr));
    constexpr size_t kHeaderSlots = 2;
    constexpr size_t kMaxSlots = 512;
    size_t slots = 0;
    for (; slots < kMaxSlots; ++slots) {
        uint64_t pointer = 0;
        uint64_t address = vtable->symbol_addr
            + (kHeaderSlots + slots) * sizeof(uint64_t);
        if (!image.read_u64_vm(address, pointer))
            break;
        bool has_relocation = false;
        Symbol* relocation_symbol = relocation_symbol_at(image, address, has_relocation);
        pointer = normalize_macho_pointer(image, pointer);
        if (!has_relocation || (!relocation_symbol && !image.is_executable_address(pointer)))
            break;
        Symbol* symbol = relocation_symbol ? relocation_symbol : image.find_symbol_by_addr(pointer);
        printf("  [%3zu] 0x%llx %s\n", slots,
            static_cast<unsigned long long>(pointer),
            symbol && symbol->symbol_name ? symbol->symbol_name : "<unnamed>");
    }
    printf("%zu virtual slots\n", slots);
    return slots ? 0 : 3;
}

static void write_json_string(std::ostream& output, const std::string& value)
{
    static const char digits[] = "0123456789abcdef";
    output << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') output << '\\' << static_cast<char>(c);
        else if (c < 0x20) output << "\\u00" << digits[c >> 4] << digits[c & 15];
        else output << static_cast<char>(c);
    }
    output << '"';
}

// Read-only structural ABI inventory. Selection and expectations belong to
// caller-provided contracts, never to a hardcoded Kext or platform in this tool.
static int inspect_abi(int argc, char** argv)
{
    if (argc != 5) {
        fprintf(stderr, "inspect-abi <symbol-template> <provider-image> <output.json>\n");
        return 1;
    }
    Macho local(argv[2]);
    Macho native(argv[3]);
    local.format_macho();
    native.format_macho();
    local.init_symbols();
    native.init_symbols();
    CppClassAnalyzer analyzer(native, local);
    analyzer.recover();
    std::ofstream output(argv[4]);
    if (!output) return 1;
    output << "{\"classes\":[";
    bool first = true;
    for (const auto& cls : analyzer.classes()) {
        if (!first) output << ',';
        first = false;
        output << "{\"name\":\"" << cls.name << "\",\"super\":\"" << cls.super_name
            << "\",\"instance_size\":" << cls.instance_size
            << ",\"vtable_slots\":" << cls.slots.size() << ",\"slots\":[";
        for (size_t index = 0; index < cls.slots.size(); ++index) {
            if (index) output << ',';
            output << cls.slots[index];
        }
        output << "]}";
    }
    output << "],\"symbols\":{";
    first = true;
    for (const auto& entry : native.symbol_name_map) {
        const auto* symbol = entry.second;
        if (!symbol || !symbol->symbol_addr) continue;
        if (!first) output << ',';
        first = false;
        write_json_string(output, entry.first);
        output << ':' << symbol->symbol_addr;
    }
    output << "}}\n";
    return output.good() ? 0 : 1;
}

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "inspect-abi"))
        return inspect_abi(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "analyze"))
        return analyze_resolver(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "dump-vtable"))
        return dump_vtable(argc, argv);

    if (argc < 6) {
        useage();
        exit(0);
    }

    const char* your_kernel_path = argv[1];
    const char* ios_kernel_path = argv[2];
    const char* symbol_list_path = argv[3];
    // @file is an explicit, audited symbol list. Unlike legacy symbol-list
    // mode it must not pick up an unrelated <image>.symbols.json sidecar.
    const bool exact_symbol_list = symbol_list_path[0] == '@';
    if (exact_symbol_list) ++symbol_list_path;
    const char* kexts_path = argv[4];
    const char* output_path = argv[5];
    const char* ikernel_patch_list_path = argc > 6 ? argv[6] : nullptr;

    std::string kexts_list_path = std::string(kexts_path) + "/kexts.txt";
    std::ifstream kexts_list_fs(kexts_list_path);

    if (!kexts_list_fs.is_open()) {
        printf("Cannot open kexts list file %s!\n", kexts_list_path.c_str());
        exit(1);
    }

    KernelMacho y_kernel(your_kernel_path);
    KernelMacho i_kernel(ios_kernel_path);

    y_kernel.format_macho();
    y_kernel.init_symbols();

    i_kernel.format_macho();
    i_kernel.init_symbols();
    std::string automatic_symbols_path = std::string(ios_kernel_path) + ".symbols.json";
    size_t automatic_symbols = !exact_symbol_list && strcmp(symbol_list_path, "-")
        ? i_kernel.load_symbols_json(automatic_symbols_path.c_str()) : 0;
    if (automatic_symbols)
        printf("Automatically loaded %zu release symbols from %s\n", automatic_symbols, automatic_symbols_path.c_str());
    format_prelink_info(i_kernel);
    if (ikernel_patch_list_path && ikernel_patch_list_path[0])
        assert(patch_ios_kernel(i_kernel, ikernel_patch_list_path) == 0);

    std::ifstream symbol_list_fs;
    if (strcmp(symbol_list_path, "auto") && strcmp(symbol_list_path, "-")) {
        symbol_list_fs.open(symbol_list_path);
        if (!symbol_list_fs.is_open()) {
            printf("Cannot open optional iOS kernel symbol list file %s!\n", symbol_list_path);
            exit(1);
        }
    }

    std::vector<std::string> kext_paths;
    std::string kext_path;
    while (std::getline(kexts_list_fs, kext_path)) {
        if (kext_path.empty() || kext_path[0] == '#' || kext_path[0] == ' ')
            continue;
        if (!kext_path.empty() && kext_path.back() == '\r')
            kext_path.pop_back();
        kext_paths.push_back(kext_path);
    }

    printf("Will load %ld kexts from list\n", kext_paths.size());
    for (size_t i = 0; i < kext_paths.size(); i++) {
        std::string kext_dir = std::string(kexts_path) + "/" + kext_paths[i];

        Kext* kext = load_kext_from_file(kext_dir.c_str());
        if (kext) {
            // Kext 在文件夹中
            // printf("ID: %s\n", kext->kext_id);
            // y_kernel.kexts[kext_ID] = kext;
            y_kernel.kexts.push_back(kext);
        } else if ((kext = i_kernel.find_kext(kext_paths[i].c_str())) != NULL) {
            // Kext 在Kernelcache中

            // printf("ID: %s\n", kext->kext_id);
            y_kernel.kexts.push_back(kext);
        } else {
            printf("Not found kext %s\n", kext_paths[i].c_str());
            exit(1);
        }
    }

    for (auto kext : y_kernel.kexts) {
        printf("Loading : %s\n", kext->kext_id);
        init_kext_depends(y_kernel, kext);
        if (kext->exec_macho) {
            kext->exec_macho->init_symbols();
        }
    }

    char tmp_symbol[255];
    while (symbol_list_fs.getline(tmp_symbol, 255)) {
        if (tmp_symbol[0] == '#' || tmp_symbol[0] == ' ' || tmp_symbol[0] == '\0')
            continue;
        unsigned long long parsed_addr = 0;
        char symbol_name[192] = {};
        if (sscanf(tmp_symbol, "%llx,%191s", &parsed_addr, symbol_name) != 2) {
            printf("Ignoring malformed symbol line: %s\n", tmp_symbol);
            continue;
        }
        uint64_t addr = static_cast<uint64_t>(parsed_addr);
        i_kernel.add_symbol(addr, symbol_name, true);

        // A collection may contain personality-only Kexts. Reuse the same
        // bounded routing as recovered symbols; those bundles have no Mach-O.
        add_symbol_to_containing_kext({symbol_name, addr}, y_kernel.kexts);
    }

    CppClassAnalyzer cpp_analyzer(i_kernel, y_kernel);
    for (Kext* kext : y_kernel.kexts) {
        if (kext && kext->is_from_file && kext->exec_macho)
            cpp_analyzer.add_kext_template(*kext->exec_macho);
    }
    std::vector<std::unique_ptr<Macho>> reference_machos;
    std::filesystem::path reference_root = find_reference_root();
    if (!reference_root.empty()) {
        std::unordered_set<std::string> loaded_reference_paths;
        for (Kext* kext : y_kernel.kexts) {
            if (!kext || kext->is_from_file || !kext->kext_id)
                continue;
            std::string identifier(kext->kext_id);
            size_t dot = identifier.find_last_of('.');
            std::string binary = dot == std::string::npos ? identifier : identifier.substr(dot + 1);
            std::filesystem::path path = reference_root / (binary + ".kext") / binary;
            if (!std::filesystem::is_regular_file(path) || !loaded_reference_paths.insert(path.string()).second)
                continue;
            auto reference = std::make_unique<Macho>(path.c_str());
            reference->format_macho();
            if (!reference->header)
                continue;
            reference->init_symbols();
            cpp_analyzer.add_symbol_reference(*reference);
            reference_machos.push_back(std::move(reference));
        }
        if (!reference_machos.empty())
            printf("Loaded %zu unstripped Kext structure references from %s\n",
                reference_machos.size(), reference_root.c_str());
    }
    CppAnalyzerStats cpp_stats = cpp_analyzer.recover();
    size_t routed_symbols = 0;
    for (const RecoveredSymbol& recovered : cpp_analyzer.symbols()) {
        if (add_symbol_to_containing_kext(recovered, y_kernel.kexts))
            ++routed_symbols;
    }
    printf("Stripped C++ analyzer: %zu registrations, %zu instance vtables, %zu named slots, "
           "%zu Kext-template slots, %zu structural symbols, %zu reference symbols, "
           "%zu symbols (%zu routed to selected Kexts)\n",
        cpp_stats.registrations, cpp_stats.classes_with_vtables, cpp_stats.named_virtual_slots,
        cpp_stats.kext_template_slots, cpp_stats.structural_symbols, cpp_stats.reference_symbols,
        cpp_stats.injected_symbols, routed_symbols);

    LinkResolver resolver(i_kernel, y_kernel);
    ResolverStats resolver_stats = resolver.build();
    printf("Automatic resolver: %zu named symbols, %zu vtables (%zu rejected), %zu virtual slots (%zu ambiguous)\n",
        resolver_stats.named_symbols, resolver_stats.matched_vtables, resolver_stats.rejected_vtables,
        resolver_stats.matched_vtable_slots, resolver_stats.ambiguous_addresses);

    size_t unresolved_externals = patch_kext_to_kernel(y_kernel, i_kernel, &resolver);
    if (unresolved_externals == SIZE_MAX) {
        printf("Kernelcache patching failed before completion; output was not written.\n");
        return 2;
    }
    if (unresolved_externals) {
        printf("Kernelcache patching failed with %zu unresolved external relocation(s); output was not written.\n",
            unresolved_externals);
        return 3;
    }

    if (!verify_and_publish(output_path, ios_kernel_path, your_kernel_path, y_kernel))
        return 4;

    printf("Finish: offline structure verified, output published.\n");

    return 0;
}


void format_info_ids(XMLElement* kextsNode)
{
    std::string error;
    if (!expand_plist_references(kextsNode, error)) {
        fprintf(stderr, "Invalid prelink plist: %s\n", error.c_str());
        exit(2);
    }
}

void format_prelink_info(KernelMacho& kernel)
{
    segment_command_64_t* pre_info_seg = (segment_command_64_t*)kernel.find_segment("__PRELINK_INFO");
    segment_command_64_t* prelink_text = (segment_command_64_t*)kernel.find_segment("__PRELINK_TEXT");
    segment_command_64_t* kenrel_exec_seg = (segment_command_64_t*)kernel.find_segment("__TEXT_EXEC");
    section_64_t* pre_info_sect = (section_64_t*)kernel.find_section("__PRELINK_INFO", "__info");
    section_64_t* pre_info_kmod_infos = (section_64_t*)kernel.find_section("__PRELINK_INFO", "__kmod_info");
    section_64_t* pre_info_kmod_starts = (section_64_t*)kernel.find_section("__PRELINK_INFO", "__kmod_start");

    if (pre_info_seg && pre_info_sect) {
        if (pre_info_seg->filesize > 0 && pre_info_sect->size > 0) {
            printf("Found prelink info\n");
            uint32_t prelink_info_size = pre_info_sect->size;
            char* prelink_info_buf = (char*)malloc(prelink_info_size);
            memcpy(prelink_info_buf, (void*)((uint64_t)kernel.file_buf + pre_info_sect->offset), prelink_info_size);

            printf("Prelink info size: %x\n", prelink_info_size);
            kernel.prlink_info_doc.Parse(prelink_info_buf, prelink_info_size);
            if (kernel.prlink_info_doc.ErrorID()) {
                printf("Error to parse prelink(%d)\n", kernel.prlink_info_doc.ErrorID());
                free(prelink_info_buf);
                exit(2);
            }
            free(prelink_info_buf);

            XMLElement* rootDict = kernel.prlink_info_doc.FirstChildElement("dict");
            if (!rootDict || !rootDict->FirstChildElement("key")
                || !rootDict->FirstChildElement("key")->GetText()
                || strcmp(rootDict->FirstChildElement("key")->GetText(), "_PrelinkInfoDictionary")) {
                printf("Prelink info not have _PrelinkInfoDictionary\n");
                return;
            }

            XMLElement* rootArray = rootDict->FirstChildElement("array");
            if (!rootArray) {
                fprintf(stderr, "Prelink dictionary array is missing\n");
                exit(2);
            }
            XMLElement* kextNode = rootArray->FirstChildElement("dict");
            printf("iOS kernelcache has %d kexts\n", rootArray->ChildElementCount());

            format_info_ids(rootArray);

            while (kextNode) {
                Kext* kext = new Kext();
                // printf("0x%016llx\n", plist_get_uint64(kextNode, "_PrelinkExecutableLoadAddr"));
                uint64_t kext_vmaddr = plist_get_uint64(kextNode, "_PrelinkExecutableLoadAddr");
                // 找到文件偏移量
                if (kext_vmaddr) {

                    uint64_t prelink_text_base = prelink_text->vmaddr;
                    uint64_t kext_fileoff = (uint64_t)kernel.file_buf + kext_vmaddr - prelink_text_base + prelink_text->fileoff;
                    // printf("off: 0x%016x\n", kext_vmaddr - prelink_text_base + prelink_text->fileoff);

                    uint64_t kext_size = plist_get_uint64(kextNode, "_PrelinkExecutableSize");
                    if (!kext_size && kext_vmaddr >= prelink_text_base
                        && kext_vmaddr - prelink_text_base < prelink_text->filesize)
                        kext_size = prelink_text->filesize - (kext_vmaddr - prelink_text_base);
                    KextMacho* kext_macho = new KextMacho((char*)kext_fileoff, static_cast<uint32_t>(kext_size));
                    kext_macho->format_macho();
                    kext->exec_macho = kext_macho;
                    kext->kmod_addr = plist_get_uint64(kextNode, "_PrelinkKmodInfo");
                } else if (plist_has_item(kextNode, "ModuleIndex")) {
                    kext_vmaddr = plist_get_uint64(kextNode, "ModuleIndex");
                    uint64_t kext_start = ((uintptr_t*)((uint64_t)kernel.file_buf + pre_info_kmod_starts->addr - pre_info_seg->vmaddr + pre_info_seg->fileoff))[kext_vmaddr];
                    uint64_t kext_end = ((uintptr_t*)((uint64_t)kernel.file_buf + pre_info_kmod_starts->addr - pre_info_seg->vmaddr + pre_info_seg->fileoff))[kext_vmaddr + 1];

                    if (!(kext_start & 0xFF00000000000000)) {
                        kext_start |= 0xFFFF000000000000;
                        kext_end |= 0xFFFF000000000000;
                    }

                    uint64_t kext_fileoff = (uint64_t)kernel.file_buf + kext_start - kenrel_exec_seg->vmaddr + kenrel_exec_seg->fileoff;
                    uint64_t kext_size = kext_end >= kext_start ? kext_end - kext_start : 0;
                    KextMacho* kext_macho = new KextMacho((char*)kext_fileoff, static_cast<uint32_t>(kext_size));
                    kext_macho->format_macho();
                    kext->exec_macho = kext_macho;
                    kext->kmod_addr = ((uintptr_t*)((uint64_t)kernel.file_buf + pre_info_kmod_infos->addr - pre_info_seg->vmaddr + pre_info_seg->fileoff))[kext_vmaddr];
                    uint64_t kmod_end = ((uintptr_t*)((uint64_t)kernel.file_buf + pre_info_kmod_infos->addr - pre_info_seg->vmaddr + pre_info_seg->fileoff))[kext_vmaddr + 1];
                    if (!(kext->kmod_addr & 0xFF00000000000000)) {
                        kext->kmod_addr |= 0xFFFF000000000000;
                        kmod_end |= 0xFFFF000000000000;
                    }
                    kext->data_size = kmod_end - kext->kmod_addr;
                }
                kext->kextInfoElement = kextNode;
                kext->kext_id = plist_get_string(kextNode, "CFBundleIdentifier");

                kernel.kexts.push_back(kext);

                kextNode = kextNode->NextSiblingElement();
            }
        }
    }
}

void init_kext_depends(KernelMacho& kernel, Kext* kext)
{
    XMLElement* depes_el = plist_get_arch_item(kext->kextInfoElement, "OSBundleLibraries", "arm64");
    if (!depes_el)
        return;
    if (strcmp(depes_el->Name(), "dict")) {
        fprintf(stderr, "Invalid arm64 dependency dictionary for %s\n", kext->kext_id);
        exit(1);
    }
    XMLElement* depend_cld = depes_el->FirstChildElement("key");

    Kext* depend_kext;
    while (depend_cld) {
        depend_kext = kernel.find_kext(depend_cld->GetText());
        printf("....dep: %s, %p\n", depend_cld->GetText(), static_cast<void*>(depend_kext));
        if (depend_kext == NULL) {
            printf("Not found kext depend %s\n", depend_cld->GetText());
            exit(1);
        }
        kext->depends.push_back(depend_kext);

        depend_cld = depend_cld->NextSiblingElement("key");
    }
}

Kext* load_kext_from_file(const char* path)
{
    if (!path)
        return nullptr;
    std::filesystem::path directory(path);
    if (directory.extension() != ".kext") directory += ".kext";
    bool contents = std::filesystem::is_regular_file(directory / "Contents/Info.plist");
    auto info_path = directory / (contents ? "Contents/Info.plist" : "Info.plist");
    if (!std::filesystem::is_regular_file(info_path)) return nullptr;
    Kext* kext = new Kext();
    kext->exec_macho = NULL;
    if (kext->kextInfoDoc.LoadFile(info_path.c_str()) != tinyxml2::XML_SUCCESS) {
        fprintf(stderr, "Invalid XML Kext plist: %s\n", info_path.c_str());
        exit(1);
    }
    auto* root = kext->kextInfoDoc.FirstChildElement();
    kext->kextInfoElement = root && !strcmp(root->Name(), "plist") ? root->FirstChildElement("dict") : root;
    if (!kext->kextInfoElement || strcmp(kext->kextInfoElement->Name(), "dict")) {
        fprintf(stderr, "Kext plist has no dictionary: %s\n", info_path.c_str());
        exit(1);
    }
    kext->kext_id = plist_get_string(kext->kextInfoElement, "CFBundleIdentifier");
    const char* executable = plist_get_string(kext->kextInfoElement, "CFBundleExecutable");
    if (!kext->kext_id || !kext->kext_id[0]) {
        fprintf(stderr, "Kext plist has no bundle identifier: %s\n", info_path.c_str());
        exit(1);
    }
    if (executable) {
        std::filesystem::path leaf(executable);
        if (leaf.empty() || leaf != leaf.filename() || leaf == "." || leaf == "..") {
            fprintf(stderr, "Invalid Kext executable name: %s\n", executable);
            exit(1);
        }
        auto executable_path = directory / (contents ? "Contents/MacOS" : "") / leaf;
        if (!std::filesystem::is_regular_file(executable_path)) {
            fprintf(stderr, "Missing declared Kext executable: %s\n", executable_path.c_str());
            exit(1);
        }
        kext->exec_macho = new KextMacho(executable_path.c_str());
        kext->exec_macho->format_macho();
    }
    kext->is_from_file = true;
    return kext;
}

uint32_t get_filesize(std::ifstream& fd)
{
    uint32_t filesize = 0;
    fd.seekg(0, std::ios::end);
    filesize = fd.tellg();

    fd.seekg(0, std::ios::beg);
    return filesize;
}

void useage()
{
    printf("kernelcache_patcher <your kernel> <iOS kernel> <symbol list|auto|-> <your kexts path> <output filename> [iOS kernel patch list]\n");
    printf("kernelcache_patcher analyze <your unstripped kernel> <iOS kernel> [iOS symbols.json] [--native-only] [--details]\n");
    printf("kernelcache_patcher dump-vtable <unstripped Mach-O> <vtable symbol>\n");
    printf("kernelcache_patcher inspect-abi <symbol-template> <provider-image> <output.json>\n");
}
