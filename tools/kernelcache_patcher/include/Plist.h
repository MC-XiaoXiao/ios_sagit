#pragma once

#include <tinyxml2.h>

#include <KernelMacho.h>

using namespace tinyxml2;

XMLElement* new_plist_elem(XMLDocument* doc, XMLElement* ins_ele, const char* type_name, const char* key_name);
void plist_add_int(XMLDocument* doc, XMLElement* elem, const char* name, uint64_t value, uint32_t size);
XMLElement* plist_get_item(XMLElement* elm, const char* keyName);
XMLElement* plist_get_arch_item(XMLElement* elm, const char* keyName, const char* architecture);
uint64_t plist_get_uint64(XMLElement* dictElem, const char* keyName);
const char* plist_get_string(XMLElement* dictElem, const char* keyName);
bool plist_has_item(XMLElement* dictElem, const char* keyName);
char* make_prelink_info(KernelMacho& kernel, uint64_t& info_size);
bool expand_plist_references(XMLElement* root, std::string& error);
