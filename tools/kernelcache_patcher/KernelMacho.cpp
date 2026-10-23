#include <KernelMacho.h>

Kext *KernelMacho::find_kext(const char *id)
{
    for(auto kext : kexts) {
        if(!strcmp(kext->kext_id, id)) {
            return kext;
        }
    }
    return NULL;
}

void KernelMacho::format_macho()
{
    Macho::format_macho();


    if (find_segment("__PRELINK_DATA")) {
        is_newer_ver = true;
        printf("Kernel has __PRELINK_DATA!\n");
    }
}


uint32_t KernelMacho::get_prelink_text_size()
{
    uint32_t size = 0;
    for (auto kext : kexts) {
        if (kext->exec_macho) {
            segment_command_64_t* text_seg = (segment_command_64_t*)(kext->exec_macho->find_segment("__TEXT"));
            if (text_seg) {
                size += ALIGN_UP(text_seg->vmsize, SEG_ALIGN);
            } else if (kext->exec_macho->header) {
                if (kext->exec_macho->header->sizeofcmds) {
                    size += ALIGN_UP(sizeof(mach_header_64_t) + kext->exec_macho->header->sizeofcmds, SEG_ALIGN);
                }
            }
        }
    }

    return size;
}

uint32_t KernelMacho::get_prelink_segment_size(const char* seg_name, uint64_t align)
{
    uint32_t size = 0;

    for (auto kext : kexts) {
        if (kext->exec_macho) {
            segment_command_64_t* text_exec_seg = (segment_command_64_t*)(kext->exec_macho->find_segment(seg_name));
            if (text_exec_seg) {
                // Keep each constant segment's original page offset. A single
                // ADRP may address globals near both ends of one source page;
                // packing at a different offset can split those consumers
                // across two destination pages, which one ADRP cannot encode.
                if (!strcmp(seg_name, "__DATA_CONST"))
                    size = ALIGN_UP(size, 1 << 12) + (text_exec_seg->vmaddr & 0xfff);
                size += ALIGN_UP(text_exec_seg->vmsize, align);
            }
        }
    }

    return size;
}

uint32_t KernelMacho::get_prelink_data_size(uint64_t align)
{
    uint32_t size = 0;

    for (auto kext : kexts) {
        if (kext->exec_macho) {
            segment_command_64_t* text_exec_seg = (segment_command_64_t*)(kext->exec_macho->find_segment("__DATA"));
            if (text_exec_seg) {
                size = ALIGN_UP(size, 1 << 12) + (text_exec_seg->vmaddr & 0xfff);
                size += ALIGN_UP(text_exec_seg->vmsize, align);
            } else if (kext->data_size) {
                size += ALIGN_UP(kext->data_size, align);
            }
        }
    }

    return size;
}
