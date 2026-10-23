#pragma once

#include <vector>

#include <tinyxml2.h>

#include <KextMacho.h>

#define KMOD_MAX_NAME    64

#pragma pack(push, 4)

/* A compatibility definition of kmod_info_t for 32-bit kexts.
 */
typedef struct kmod_info_32_v1 {
	uint32_t            next_addr;
	int32_t             info_version;
	uint32_t            id;
	uint8_t             name[KMOD_MAX_NAME];
	uint8_t             version[KMOD_MAX_NAME];
	int32_t             reference_count;
	uint32_t            reference_list_addr;
	uint32_t            address;
	uint32_t            size;
	uint32_t            hdr_size;
	uint32_t            start_addr;
	uint32_t            stop_addr;
} kmod_info_32_v1_t;

/* A compatibility definition of kmod_info_t for 64-bit kexts.
 */
typedef struct kmod_info_64_v1 {
	uint64_t            next_addr;
	int32_t             info_version;
	uint32_t            id;
	uint8_t             name[KMOD_MAX_NAME];
	uint8_t             version[KMOD_MAX_NAME];
	int32_t             reference_count;
	uint64_t            reference_list_addr;
	uint64_t            address;
	uint64_t            size;
	uint64_t            hdr_size;
	uint64_t            start_addr;
	uint64_t            stop_addr;
} kmod_info_64_v1_t;

#pragma pack(pop)

using namespace tinyxml2;

class Kext
{
private:
    /* data */
public:
    XMLDocument kextInfoDoc;
    XMLElement *kextInfoElement = nullptr;
    KextMacho *exec_macho = nullptr;
    std::vector<Kext *> depends;
    const char *kext_id = nullptr;
    bool is_from_file = false;

    uint64_t text_off = 0;
    uint64_t serialized_text_size = 0;
    uint64_t text_exec_off = 0;
    uint64_t data_off = 0;
	uint32_t data_size = 0;
    uint64_t data_const_off = 0;

    uint64_t kmod_addr = 0;

    Kext(/* args */);
    ~Kext();
};
