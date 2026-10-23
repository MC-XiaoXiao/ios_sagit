#pragma once

#include <fstream>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>

#define ALIGN_UP(x, y) (((x) + ((y) - 1)) & ~((y) - 1))
#define SEG_ALIGN 1 << 6
// Coalesced protection boundaries must also work with 16 KiB software VM pages.
#define ARM64_VM_PAGE_SIZE (1ULL << 14)

#define IN_SEGMENT_RANGE(x, seg) (seg && ((x) >= (seg)->vmaddr && (x) < (seg)->vmaddr + (seg)->vmsize))

typedef struct mach_header {
	uint32_t	magic;		/* mach magic number identifier */
	uint32_t	cputype;	/* cpu specifier */
	uint32_t	cpusubtype;	/* machine specifier */
	uint32_t	filetype;	/* type of file */
	uint32_t	ncmds;		/* number of load commands */
	uint32_t	sizeofcmds;	/* the size of all the load commands */
	uint32_t	flags;		/* flags */
} mach_header_t;

#define	MH_MAGIC	0xfeedface	/* the mach magic number */

typedef struct mach_header_64
{
    uint32_t magic;           /* mach magic number identifier */
    uint32_t cputype;         /* cpu specifier */
    uint32_t cpusubtype;      /* machine specifier */
    uint32_t filetype;        /* type of file */
    uint32_t ncmds;           /* number of load commands */
    uint32_t sizeofcmds;      /* the size of all the load commands */
    uint32_t flags;           /* flags */
    uint32_t reserved;        /* reserved */
} mach_header_64_t;

#define MH_MAGIC_64 0xfeedfacf /* the 64-bit mach magic number */

struct load_command {
	uint32_t cmd;		/* type of load command */
	uint32_t cmdsize;	/* total size of command in bytes */
};

#define	LC_SEGMENT	0x1	/* segment of this file to be mapped */
#define	LC_SYMTAB	0x2	/* link-edit stab symbol table info */
#define	LC_THREAD	0x4	/* thread */
#define	LC_UNIXTHREAD	0x5	/* unix thread (includes a stack) */
#define	LC_DYSYMTAB	0xb	/* dynamic link-edit symbol table info */
#define LC_SEGMENT_64   0x19    /* 64-bit segment of this file to be mapped */

typedef struct segment_command { /* for 32-bit architectures */
	uint32_t	cmd;		/* LC_SEGMENT */
	uint32_t	cmdsize;	/* includes sizeof section structs */
	char		segname[16];	/* segment name */
	uint32_t	vmaddr;		/* memory address of this segment */
	uint32_t	vmsize;		/* memory size of this segment */
	uint32_t	fileoff;	/* file offset of this segment */
	uint32_t	filesize;	/* amount to map from the file */
	int     	maxprot;	/* maximum VM protection */
	int     	initprot;	/* initial VM protection */
	uint32_t	nsects;		/* number of sections in segment */
	uint32_t	flags;		/* flags */
} segment_command_t;

typedef struct segment_command_64 { /* for 64-bit architectures */
	uint32_t	cmd;		/* LC_SEGMENT_64 */
	uint32_t	cmdsize;	/* includes sizeof section_64 structs */
	char		segname[16];	/* segment name */
	uint64_t	vmaddr;		/* memory address of this segment */
	uint64_t	vmsize;		/* memory size of this segment */
	uint64_t	fileoff;	/* file offset of this segment */
	uint64_t	filesize;	/* amount to map from the file */
	int     	maxprot;	/* maximum VM protection */
	int     	initprot;	/* initial VM protection */
	uint32_t	nsects;		/* number of sections in segment */
	uint32_t	flags;		/* flags */
} segment_command_64_t;

typedef struct section_64
{                       /* for 64-bit architectures */
    char sectname[16];  /* name of this section */
    char segname[16];   /* segment this section goes in */
    uint64_t addr;      /* memory address of this section */
    uint64_t size;      /* size in bytes of this section */
    uint32_t offset;    /* file offset of this section */
    uint32_t align;     /* section alignment (power of 2) */
    uint32_t reloff;    /* file offset of relocation entries */
    uint32_t nreloc;    /* number of relocation entries */
    uint32_t flags;     /* flags (section type and attributes)*/
    uint32_t reserved1; /* reserved (for offset or index) */
    uint32_t reserved2; /* reserved (for count or sizeof) */
    uint32_t reserved3; /* reserved */
} section_64_t;

struct symtab_command
{
    uint32_t cmd;     /* LC_SYMTAB */
    uint32_t cmdsize; /* sizeof(struct symtab_command) */
    uint32_t symoff;  /* symbol table offset */
    uint32_t nsyms;   /* number of symbol table entries */
    uint32_t stroff;  /* string table offset */
    uint32_t strsize; /* string table size in bytes */
};

struct dysymtab_command
{
    uint32_t cmd;     /* LC_DYSYMTAB */
    uint32_t cmdsize; /* sizeof(struct dysymtab_command) */

    /*
     * The symbols indicated by symoff and nsyms of the LC_SYMTAB load command
     * are grouped into the following three groups:
     *    local symbols (further grouped by the module they are from)
     *    defined external symbols (further grouped by the module they are from)
     *    undefined symbols
     *
     * The local symbols are used only for debugging.  The dynamic binding
     * process may have to use them to indicate to the debugger the local
     * symbols for a module that is being bound.
     *
     * The last two groups are used by the dynamic binding process to do the
     * binding (indirectly through the module table and the reference symbol
     * table when this is a dynamically linked shared library file).
     */
    uint32_t ilocalsym; /* index to local symbols */
    uint32_t nlocalsym; /* number of local symbols */

    uint32_t iextdefsym; /* index to externally defined symbols */
    uint32_t nextdefsym; /* number of externally defined symbols */

    uint32_t iundefsym; /* index to undefined symbols */
    uint32_t nundefsym; /* number of undefined symbols */

    /*
     * For the for the dynamic binding process to find which module a symbol
     * is defined in the table of contents is used (analogous to the ranlib
     * structure in an archive) which maps defined external symbols to modules
     * they are defined in.  This exists only in a dynamically linked shared
     * library file.  For executable and object modules the defined external
     * symbols are sorted by name and is use as the table of contents.
     */
    uint32_t tocoff; /* file offset to table of contents */
    uint32_t ntoc;   /* number of entries in table of contents */

    /*
     * To support dynamic binding of "modules" (whole object files) the symbol
     * table must reflect the modules that the file was created from.  This is
     * done by having a module table that has indexes and counts into the merged
     * tables for each module.  The module structure that these two entries
     * refer to is described below.  This exists only in a dynamically linked
     * shared library file.  For executable and object modules the file only
     * contains one module so everything in the file belongs to the module.
     */
    uint32_t modtaboff; /* file offset to module table */
    uint32_t nmodtab;   /* number of module table entries */

    /*
     * To support dynamic module binding the module structure for each module
     * indicates the external references (defined and undefined) each module
     * makes.  For each module there is an offset and a count into the
     * reference symbol table for the symbols that the module references.
     * This exists only in a dynamically linked shared library file.  For
     * executable and object modules the defined external symbols and the
     * undefined external symbols indicates the external references.
     */
    uint32_t extrefsymoff; /* offset to referenced symbol table */
    uint32_t nextrefsyms;  /* number of referenced symbol table entries */

    /*
     * The sections that contain "symbol pointers" and "routine stubs" have
     * indexes and (implied counts based on the size of the section and fixed
     * size of the entry) into the "indirect symbol" table for each pointer
     * and stub.  For every section of these two types the index into the
     * indirect symbol table is stored in the section header in the field
     * reserved1.  An indirect symbol table entry is simply a 32bit index into
     * the symbol table to the symbol that the pointer or stub is referring to.
     * The indirect symbol table is ordered to match the entries in the section.
     */
    uint32_t indirectsymoff; /* file offset to the indirect symbol table */
    uint32_t nindirectsyms;  /* number of indirect symbol table entries */

    /*
     * To support relocating an individual module in a library file quickly the
     * external relocation entries for each module in the library need to be
     * accessed efficiently.  Since the relocation entries can't be accessed
     * through the section headers for a library file they are separated into
     * groups of local and external entries further grouped by module.  In this
     * case the presents of this load command who's extreloff, nextrel,
     * locreloff and nlocrel fields are non-zero indicates that the relocation
     * entries of non-merged sections are not referenced through the section
     * structures (and the reloff and nreloc fields in the section headers are
     * set to zero).
     *
     * Since the relocation entries are not accessed through the section headers
     * this requires the r_address field to be something other than a section
     * offset to identify the item to be relocated.  In this case r_address is
     * set to the offset from the vmaddr of the first LC_SEGMENT command.
     * For MH_SPLIT_SEGS images r_address is set to the the offset from the
     * vmaddr of the first read-write LC_SEGMENT command.
     *
     * The relocation entries are grouped by module and the module table
     * entries have indexes and counts into them for the group of external
     * relocation entries for that the module.
     *
     * For sections that are merged across modules there must not be any
     * remaining external relocation entries for them (for merged sections
     * remaining relocation entries must be local).
     */
    uint32_t extreloff; /* offset to external relocation entries */
    uint32_t nextrel;   /* number of external relocation entries */

    /*
     * All the local relocation entries are grouped together (they are not
     * grouped by their module since they are only used if the object is moved
     * from it staticly link edited address).
     */
    uint32_t locreloff; /* offset to local relocation entries */
    uint32_t nlocrel;   /* number of local relocation entries */
};

/*
 * The n_type field really contains four fields:
 *	unsigned char N_STAB:3,
 *		      N_PEXT:1,
 *		      N_TYPE:3,
 *		      N_EXT:1;
 * which are used via the following masks.
 */
#define N_STAB 0xe0 /* if any of these bits set, a symbolic debugging entry */
#define N_PEXT 0x10 /* private external symbol bit */
#define N_TYPE 0x0e /* mask for the type bits */
#define N_EXT 0x01  /* external symbol bit, set for external symbols */

struct relocation_info
{
    int32_t r_address;         /* offset in the section to what is being
                          relocated */
    uint32_t r_symbolnum : 24, /* symbol index if r_extern == 1 or section
                  ordinal if r_extern == 0 */
        r_pcrel : 1,           /* was relocated pc relative already */
        r_length : 2,          /* 0=byte, 1=word, 2=long, 3=quad */
        r_extern : 1,          /* does not include value of sym referenced */
        r_type : 4;            /* if not 0, machine specific relocation type */
};

struct nlist_64
{
    union
    {
        uint32_t n_strx; /* index into the string table */
    } n_un;
    uint8_t n_type;   /* type flag, see below */
    uint8_t n_sect;   /* section number or NO_SECT */
    uint16_t n_desc;  /* see <mach-o/stab.h> */
    uint64_t n_value; /* value of this symbol (or stab offset) */
};


typedef struct zone_view *zone_view_t;
struct zone_view {
	uint64_t          zv_zone;
	uint64_t    	zv_stats;
	const char     *zv_name;
	zone_view_t     zv_next;
};

struct kalloc_type_view {
	struct zone_view        kt_zv;
	const char             *kt_signature;
	uint32_t     kt_flags;
	uint32_t                kt_size;
	void                   *unused1;
	void                   *unused2;
};

typedef struct kalloc_type_view *kalloc_type_view_t;

#define kalloc_log2down(mask)   (31 - __builtin_clz(mask))
#define KHEAP_START_SIZE        32
#define KHEAP_MAX_SIZE          (32 * 1024)
#define KHEAP_EXTRA_ZONES       2
#define KHEAP_STEP_WIDTH        2
#define KHEAP_STEP_START        16
#define KHEAP_START_IDX         kalloc_log2down(KHEAP_START_SIZE)
#define KHEAP_NUM_STEPS         (kalloc_log2down(KHEAP_MAX_SIZE) - \
	                                kalloc_log2down(KHEAP_START_SIZE))
#define KHEAP_NUM_ZONES         (KHEAP_NUM_STEPS * KHEAP_STEP_WIDTH + \
	                                KHEAP_EXTRA_ZONES)

struct kalloc_type_var_view {
	uint16_t   kt_version;
	uint16_t                kt_size_hdr;
	/*
	 * Temporary: Needs to be 32bits cause we have many structs that use
	 * IONew/Delete that are larger than 32K.
	 */
	uint32_t                kt_size_type;
	uint64_t            	kt_stats;
	const char             * kt_name;
	zone_view_t             kt_next;
	uint16_t               	kt_heap_start;
	uint8_t                 kt_zones[KHEAP_NUM_ZONES];
	const char             *  kt_sig_hdr;
	const char             *  kt_sig_type;
	uint32_t     			kt_flags;
};

typedef struct kalloc_type_var_view *kalloc_type_var_view_t;

load_command* find_command(mach_header_64_t* header, uint32_t cmd);

typedef struct Symbol {
	const char *symbol_name;
	uint64_t	symbol_addr;
} Symbol;

class Macho
{
private:
    std::ifstream filefs;
    char file_path[255];
    uint32_t get_file_size();

public:
    const char *path() const { return file_path; }
    char *file_buf = nullptr;
    uint32_t file_size = 0;
    mach_header_64_t *header = nullptr;
    bool is_64 = false;
    bool is_newer_ver = false;
	uint32_t symbol_count = 0;
	std::vector<Symbol *>symbol_list;
	std::map<uint64_t, Symbol *> symbol_addr_map;
	std::map<std::string, Symbol *> symbol_name_map;
	std::unordered_map<std::string, Symbol *> symbol_alias_map;
	std::deque<std::string> owned_symbol_names;

    virtual void format_macho();
    void init_symbols();
    Symbol *add_symbol(uint64_t address, const std::string& name, bool overwrite = false);
    Symbol *find_symbol_by_name(const std::string& name) const;
    Symbol *find_symbol_by_addr(uint64_t address) const;
    size_t load_symbols_json(const char *path, bool overwrite = false);
    void copy_from_file(uint64_t offset, char *targetBuff, size_t size);
    void *find_segment(const char *segment_name);
    void *find_section(const char *segname, const char *section_name);
    bool addr_in_segment(const char *segment_name, uint64_t addr);
    bool addr_in_segment(segment_command_64_t *seg, uint64_t addr);
	load_command *find_command(uint32_t cmd);
    uint32_t get_fileoff(uint64_t vmaddr);
    bool vmaddr_to_fileoff(uint64_t vmaddr, uint64_t& fileoff, size_t size = 1) const;
    bool read_u32_vm(uint64_t vmaddr, uint32_t& value) const;
    bool read_u64_vm(uint64_t vmaddr, uint64_t& value) const;
    bool read_cstring_vm(uint64_t vmaddr, std::string& value, size_t max_length = 256) const;
    bool is_mapped_address(uint64_t vmaddr) const;
    bool is_executable_address(uint64_t vmaddr) const;
    std::vector<const segment_command_64_t*> segments() const;

	Macho();
	Macho(char *buf, uint32_t file_size);
    Macho(const char *path);
    ~Macho();
};
