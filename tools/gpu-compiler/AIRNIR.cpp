// SPDX-License-Identifier: BSD-2-Clause
// Keep Mesa and LLVM handles private to the compiler dylib. The runtime AIR
// reader shares only LLVM's stable C ABI, not Apple's C++ layouts.
// The shared lowering intentionally uses the reader's internal LLVM helpers.
// clang-format off
#include "AIRRuntime.cpp"
#include "AIRNIR.h"
#include "AIRNIRLowering.h"
// clang-format on
extern "C" int qgpu_compile_air_nir(const void *bytes, size_t size, const char *entry, const QGPUAIRConstant *constants,
                                    unsigned count, uint64_t registry, QGPUProgramImage *out, QGPUAIRNIRTiming *timing,
                                    char *error, unsigned errorBytes) {
    if (timing)
        *timing = {};
    if (out)
        *out = {};
    if (!error || !errorBytes)
        return 0;
    error[0] = 0;
    if (!bytes || !size || size > 16u * 1024 * 1024 || !entry || strlen(entry) >= sizeof(out->name) || !out ||
        count > GPU::Compiler::MaxConstantValues || (count && !constants)) {
        snprintf(error, errorBytes, "Invalid bounded AIR input");
        return 0;
    }
    // A failed direct attempt must never leave a registerable partial image.
    *out = {};
    try {
        std::vector<GPU::Compiler::FunctionConstantValue> values;
        values.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            GPU::Compiler::FunctionConstantValue v;
            v.index = constants[i].index;
            v.type = constants[i].type;
            if (v.index > 65535 || !GPU::Compiler::constantBytes(v.type))
                throw std::runtime_error("Invalid resolved AIR constant");
            memcpy(v.bytes.data(), constants[i].bytes, sizeof(constants[i].bytes));
            values.push_back(v);
        }
        std::string diagnostic;
        GPU::Compiler::AIRNIRMetrics stats;
        bool ok = GPU::Compiler::compileAIRNIRModule(bytes, size, entry, values, registry, *out, diagnostic, stats,
                                                     qgpu_compile_nir);
        if (timing) {
            timing->lowerNS = uint64_t((stats.parseMS + stats.emitMS) * 1e6);
            timing->backendNS = uint64_t(stats.backendMS * 1e6);
        }
        if (ok)
            return 1;
        *out = {};
        snprintf(error, errorBytes, "%s", diagnostic.c_str());
        return 0;
    } catch (const std::exception &e) {
        *out = {};
        snprintf(error, errorBytes, "%s", e.what());
        return 0;
    }
}
