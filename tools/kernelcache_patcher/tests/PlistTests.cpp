#include <Plist.h>
#include <iostream>

int main()
{
    XMLDocument doc;
    doc.Parse("<array><dict IDREF='2'/><dict ID='2'><key>version</key>"
              "<string IDREF='1'/></dict><string ID='1'>24.6.0</string></array>");
    std::string error;
    if (!expand_plist_references(doc.RootElement(), error)) {
        std::cerr << error << '\n';
        return 1;
    }
    auto* first = doc.RootElement()->FirstChildElement();
    if (strcmp(first->Name(), "dict") || !plist_get_string(first, "version")
        || strcmp(plist_get_string(first, "version"), "24.6.0"))
        return 1;
    for (const char* invalid : { "<array><string IDREF='missing'/></array>",
             "<array><dict ID='1'><dict IDREF='1'/></dict></array>",
             "<array><string ID='1'>a</string><string ID='1'>b</string></array>" }) {
        XMLDocument bad;
        bad.Parse(invalid);
        if (expand_plist_references(bad.RootElement(), error))
            return 1;
    }
    // A codeless bundle carries real IOKit personalities and dependencies.
    // It must survive serialization without acquiring invented code metadata.
    XMLDocument codeless;
    codeless.Parse("<dict><key>CFBundleIdentifier</key><string>org.example.matching</string>"
                   "<key>CFBundleVersion</key><string>1.0</string>"
                   "<key>OSBundleLibraries</key><dict><key>org.example.code</key><string>1.0</string></dict>"
                   "<key>IOKitPersonalities</key><dict><key>Events</key><dict>"
                   "<key>IOClass</key><string>ExampleEventDriver</string>"
                   "<key>CFBundleIdentifier</key><string>org.example.code</string>"
                   "<key>IOProviderClass</key><string>IOService</string></dict></dict></dict>");
    Kext metadata;
    metadata.kextInfoElement = codeless.RootElement();
    KernelMacho kernel;
    kernel.kexts.push_back(&metadata);
    uint64_t size = 0;
    char* bytes = make_prelink_info(kernel, size);
    XMLDocument packed;
    if (!bytes || !size || size % 4096 || packed.Parse(bytes, size) != tinyxml2::XML_SUCCESS) return 2;
    free(bytes);
    auto* array = plist_get_item(packed.RootElement(), "_PrelinkInfoDictionary");
    auto* entry = array ? array->FirstChildElement("dict") : nullptr;
    if (!entry || entry->NextSiblingElement()
        || strcmp(plist_get_string(entry, "CFBundleIdentifier"), "org.example.matching")
        || plist_has_item(entry, "_PrelinkExecutableLoadAddr")
        || plist_has_item(entry, "_PrelinkKmodInfo")) return 3;
    auto* personalities = plist_get_item(entry, "IOKitPersonalities");
    auto* events = personalities ? plist_get_item(personalities, "Events") : nullptr;
    auto* libraries = plist_get_item(entry, "OSBundleLibraries");
    if (!events || !libraries || strcmp(plist_get_string(events, "IOClass"), "ExampleEventDriver")
        || strcmp(plist_get_string(libraries, "org.example.code"), "1.0")) return 4;
    XMLDocument architecture;
    architecture.Parse("<dict><key>OSBundleLibraries</key><dict>"
                       "<key>org.example.generic</key><string>1</string></dict>"
                       "<key>OSBundleLibraries_arm64</key><dict>"
                       "<key>org.example.arm</key><string>2</string></dict>"
                       "<key>OSBundleLibraries_x86_64</key><dict/></dict>");
    auto* fallback = plist_get_item(architecture.RootElement(), "OSBundleLibraries");
    auto* arm = plist_get_arch_item(architecture.RootElement(), "OSBundleLibraries", "arm64");
    auto* x86 = plist_get_arch_item(architecture.RootElement(), "OSBundleLibraries", "x86_64");
    if (plist_has_item(arm, "org.example.generic")
        || strcmp(plist_get_string(arm, "org.example.arm"), "2")
        || !x86 || x86->FirstChildElement()
        || plist_get_arch_item(architecture.RootElement(), "OSBundleLibraries", "armv7") != fallback)
        return 5;
    metadata.kextInfoElement = architecture.RootElement();
    bytes = make_prelink_info(kernel, size);
    XMLDocument preserved;
    if (!bytes || preserved.Parse(bytes, size) != tinyxml2::XML_SUCCESS) return 6;
    free(bytes);
    auto* dict = plist_get_item(preserved.RootElement(), "_PrelinkInfoDictionary")->FirstChildElement("dict");
    if (!plist_has_item(dict, "OSBundleLibraries") || !plist_has_item(dict, "OSBundleLibraries_arm64")
        || !plist_has_item(dict, "OSBundleLibraries_x86_64")) return 7;
    return 0;
}
