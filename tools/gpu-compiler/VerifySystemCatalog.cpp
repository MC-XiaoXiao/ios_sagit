// SPDX-License-Identifier: BSD-2-Clause
// Decode each generated catalog variant using the production loader, without LLVM.
#include <GPUSystemShaders.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>

int main(int argc, char **argv) {
    if (argc != 2) return 64;
    using namespace GPU;
    size_t count = 0;
    auto image = std::make_unique<QGPUProgramImage>();
    for (const auto &entry : GPUSystemShaders::Entries) {
        if (SystemShaders::find(GPUSystemShaders::Entries, GPUSystemShaders::Count,
                                entry.airHash, entry.name) != &entry) return 1;
        std::vector<Compiler::FunctionConstant> declarations;
        unsigned stage;
        SystemShaders::reflect(entry, declarations, stage);
        if (stage != entry.stage) return 2;
        for (size_t n = 0; n < entry.variantCount; ++n) {
            const auto &variant = entry.variants[n];
            std::vector<Compiler::FunctionConstantValue> input, values;
            for (size_t i = 0; i < variant.count; ++i) {
                Compiler::FunctionConstantValue value;
                value.index = variant.constants[i].index;
                value.type = variant.constants[i].type;
                std::memcpy(value.bytes.data(), variant.constants[i].bytes, sizeof(variant.constants[i].bytes));
                input.push_back(value);
            }
            std::reverse(input.begin(), input.end());
            std::string diagnostic;
            if (!Compiler::resolveFunctionConstants(declarations, input, values, diagnostic)) return 3;
            const auto *matched = SystemShaders::match(entry, values);
            if (matched != &variant ||
                !SystemShaders::restore(entry, *matched, GPUSystemShaders::Registry, *image)) return 4;
            std::ofstream output(std::string(argv[1]) + "/" + std::to_string(count) + ".bin", std::ios::binary);
            output.write(reinterpret_cast<const char *>(image.get()), sizeof(*image));
            output.close();
            if (!output) return 5;
            ++count;
        }
    }
    if (!count) return 6;
    std::cout << "{\"variants\":" << count << ",\"compiler_linked\":false,\"hardware_tested\":false}\n";
}
