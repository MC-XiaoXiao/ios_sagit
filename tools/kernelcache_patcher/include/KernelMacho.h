#pragma once

#include <map>
#include <vector>
#include <string>

#include <Macho.h>
#include <Kext.h>

class KernelMacho : public Macho
{
private:
    /* data */
public:
    std::vector<Kext *> kexts;
    XMLDocument prlink_info_doc;
    uint64_t prelink_text_base;

    virtual void format_macho() override;

    Kext *find_kext(const char *id);

    using Macho::Macho;
    uint32_t get_prelink_text_size();
    uint32_t get_prelink_segment_size(const char* seg_name, uint64_t align);
    uint32_t get_prelink_data_size(uint64_t align);
};

