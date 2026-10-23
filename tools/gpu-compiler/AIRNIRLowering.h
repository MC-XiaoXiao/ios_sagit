// SPDX-License-Identifier: BSD-2-Clause
// Shared AIR lowering, included after AIRRuntime.cpp in the same translation unit.
#pragma once
#include "compiler/glsl_types.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_builtin_builder.h"
#include <chrono>
#include <functional>
namespace GPU {
namespace Compiler {
namespace {
struct AIRNIRMetrics {
    double parseMS = 0, emitMS = 0, backendMS = 0;
    unsigned blocks = 0, instructions = 0;
};
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
struct NV {
    enum Kind { Data, Pointer, Private, Texture, Sampler, ReadSampler, Aggregate, Unavailable } kind = Data;
    nir_def *def = nullptr;
    nir_variable *cell = nullptr;
    LLVMValueRef root = nullptr;
    unsigned index = 0;
    std::vector<NV> members;
    std::set<LLVMValueRef> roots;
};
struct NB {
    unsigned slot = 0, stride = 0;
    bool readonly = false;
    nir_variable *storage = nullptr;
    unsigned bytes = 0, dynamicSlot = UINT32_MAX;
    LLVMTypeRef constantElement = nullptr;
};
class Direct {
    LLVMValueRef f;
    LLVMTargetDataRef layout;
    unsigned stage;
    nir_builder b{};
    std::map<LLVMValueRef, NV> values;
    std::map<LLVMValueRef, NB> buffers;
    std::map<LLVMValueRef, unsigned> rootIDs;
    std::map<LLVMValueRef, AIRTextureBinding> textures;
    std::map<LLVMValueRef, unsigned> samplers;
    std::vector<QGPUConstantSampler> literals;
    std::vector<nir_variable *> outputs;
    std::map<std::string, nir_variable *> uniforms;
    std::map<LLVMValueRef, std::vector<NV>> tables;
    nir_variable *dynamicShared = nullptr;
    unsigned sharedBytes = 0, privateBytes = 0;
    std::set<LLVMValueRef> resolving;
    std::vector<LLVMBasicBlockRef> blocks;
    std::map<LLVMBasicBlockRef, unsigned> ids;
    std::vector<std::set<unsigned>> postdom;
    std::vector<unsigned> join;
    unsigned serial = 0, visits = 0;
    bool initializeSSA = false;
    static const nir_shader_compiler_options options;
    nir_def *u(uint32_t x) { return nir_imm_int(&b, x); }
    nir_def *c(nir_def *v, unsigned i) { return nir_channel(&b, v, i); }
    nir_def *vec(const std::vector<nir_def *> &v) {
        return nir_vec(&b, const_cast<nir_def **>(v.data()), v.size());
    }
    unsigned bits(LLVMTypeRef t) {
        if (half(t))
            return 16;
        if (floating(t))
            return 32;
        if (LLVMGetTypeKind(scalar(t)) != LLVMIntegerTypeKind)
            fail("NIR unsupported numeric type");
        unsigned n = LLVMGetIntTypeWidth(scalar(t));
        if (n != 1 && n != 8 && n != 16 && n != 32 && n != 64)
            fail("NIR unsupported integer width");
        return n;
    }
    const glsl_type *ntype(LLVMTypeRef t) {
        auto base = floating(t)             ? (half(t) ? GLSL_TYPE_FLOAT16 : GLSL_TYPE_FLOAT)
                    : integer(scalar(t), 1) ? GLSL_TYPE_BOOL
                    : bits(t) == 8          ? GLSL_TYPE_UINT8
                    : bits(t) == 64         ? GLSL_TYPE_UINT64
                    : bits(t) == 16         ? GLSL_TYPE_UINT16
                                            : GLSL_TYPE_UINT;
        return glsl_vector_type(base, lanes(t));
    }
    NV data(nir_def *d) {
        NV v;
        v.def = d;
        return v;
    }
    nir_def *read(NV v) {
        if (v.kind != NV::Data)
            fail("NIR frontend: nonnumeric operand");
        return v.cell ? nir_load_var(&b, v.cell) : v.def;
    }
    NV reserve(LLVMTypeRef t) {
        NV v;
        auto k = LLVMGetTypeKind(t);
        if (k == LLVMStructTypeKind) {
            v.kind = NV::Aggregate;
            for (unsigned n = 0; n < LLVMCountStructElementTypes(t); ++n)
                v.members.push_back(reserve(LLVMStructGetTypeAtIndex(t, n)));
        } else if ((floating(t) || k == LLVMIntegerTypeKind || k == LLVMVectorTypeKind) && bits(t) <= 64 &&
                   lanes(t) <= 4) {
            v.cell = nir_local_variable_create(b.impl, ntype(t), ("ssa" + std::to_string(serial++)).c_str());
            if (initializeSSA)
                nir_store_var(&b, v.cell, nir_imm_zero(&b, lanes(t), bits(t)), (1u << lanes(t)) - 1);
        } else
            v.kind = NV::Unavailable;
        return v;
    }
    nir_def *pointerPayload(NV p) {
        if (p.kind == NV::Private)
            return nir_vec3(&b, u(0), u(0), u(rootIDs.at(p.root)));
        if (p.kind != NV::Pointer)
            fail("NIR pointer merge requires a buffer/storage pointer");
        if (p.cell)
            return nir_load_var(&b, p.cell);
        if (!p.root)
            return p.def;
        return nir_vec3(&b, c(p.def, 0), c(p.def, 1), u(rootIDs.at(p.root)));
    }
    NV pointerOffset(NV p) {
        if (p.kind == NV::Private) {
            p.kind = NV::Pointer;
            p.cell = nullptr;
            p.def = addr(0);
        }
        if (p.kind == NV::Pointer && p.cell) {
            p.def = nir_load_var(&b, p.cell);
            p.cell = nullptr;
        }
        return p;
    }
    std::vector<std::pair<LLVMValueRef, unsigned>> pointerRoots(const NV &p) {
        std::vector<std::pair<LLVMValueRef, unsigned>> roots;
        if (p.root)
            roots.push_back({p.root, rootIDs.at(p.root)});
        else
            for (auto root : p.roots)
                if (root)
                    roots.push_back({root, rootIDs.at(root)});
        std::sort(roots.begin(), roots.end(),
                  [](const auto &a, const auto &d) { return a.second < d.second; });
        return roots;
    }
    void copy(NV target, NV source) {
        if (target.kind == NV::Unavailable)
            return;
        if (target.kind == NV::Aggregate) {
            if (source.kind != NV::Aggregate || source.members.size() != target.members.size())
                fail("NIR aggregate mismatch");
            for (unsigned n = 0; n < target.members.size(); ++n)
                copy(target.members[n], source.members[n]);
            return;
        }
        if (!target.cell)
            fail("NIR missing SSA storage");
        auto value = target.kind == NV::Pointer ? pointerPayload(source) : read(source);
        nir_store_var(&b, target.cell, value, (1u << value->num_components) - 1);
    }
    void set(LLVMValueRef i, NV v) {
        auto it = values.find(i);
        if (it != values.end() && (it->second.cell || it->second.kind == NV::Aggregate)) {
            if (v.index == 2)
                it->second.index = 2;
            copy(it->second, v);
            return;
        }
        values[i] = std::move(v);
    }
    void set(LLVMValueRef i, nir_def *v) { set(i, data(v)); }
    NV zero(LLVMTypeRef t) {
        if (LLVMGetTypeKind(t) == LLVMPointerTypeKind) {
            NV v;
            v.kind = NV::Pointer;
            v.def = nir_imm_zero(&b, 3, 32);
            v.roots.insert(nullptr);
            return v;
        }
        if (LLVMGetTypeKind(t) == LLVMStructTypeKind) {
            NV v;
            v.kind = NV::Aggregate;
            for (unsigned n = 0; n < LLVMCountStructElementTypes(t); ++n)
                v.members.push_back(zero(LLVMStructGetTypeAtIndex(t, n)));
            return v;
        }
        return data(nir_imm_zero(&b, lanes(t), bits(t)));
    }
    nir_def *addr(uint64_t a) { return nir_vec2(&b, u(uint32_t(a)), u(uint32_t(a >> 32))); }
    nir_def *addAddress(nir_def *a, nir_def *d) {
        auto lo = nir_iadd(&b, c(a, 0), c(d, 0));
        return nir_vec2(
            &b, lo, nir_iadd(&b, nir_iadd(&b, c(a, 1), c(d, 1)), nir_b2i32(&b, nir_ult(&b, lo, c(a, 0)))));
    }
    nir_def *extend(nir_def *v, bool sign) {
        if (v->bit_size == 1)
            v = sign ? nir_ineg(&b, nir_b2i32(&b, v)) : nir_b2i32(&b, v);
        auto low = sign ? nir_i2i32(&b, v) : nir_u2u32(&b, v);
        return nir_vec2(&b, low, sign ? nir_ishr_imm(&b, low, 31) : u(0));
    }
    nir_def *mulAddress(nir_def *a, uint32_t stride) {
        return nir_vec2(
            &b, nir_imul_imm(&b, c(a, 0), stride),
            nir_iadd(&b, nir_umul_high(&b, c(a, 0), u(stride)), nir_imul_imm(&b, c(a, 1), stride)));
    }
    NV get(LLVMValueRef v) {
        if (values.count(v) && values.at(v).kind != NV::Unavailable)
            return values.at(v);
        auto t = LLVMTypeOf(v);
        auto k = LLVMGetTypeKind(t);
        if (LLVMIsAAllocaInst(v)) {
            auto allocated = LLVMGetAllocatedType(v);
            if (LLVMGetInstructionParent(v) != LLVMGetFirstBasicBlock(f) ||
                !LLVMIsAConstantInt(LLVMGetOperand(v, 0)) || number(LLVMGetOperand(v, 0)) != 1 ||
                LLVMGetPointerAddressSpace(t) != 0)
                fail("NIR private allocation requires one entry-block object");
            if (LLVMGetTypeKind(allocated) == LLVMArrayTypeKind) {
                auto leaf = allocated;
                uint64_t elements = 1;
                unsigned depth = 0;
                while (LLVMGetTypeKind(leaf) == LLVMArrayTypeKind) {
                    auto count = LLVMGetArrayLength(leaf);
                    auto spelling = LLVMPrintTypeToString(leaf);
                    auto prefix = "[" + std::to_string(count) + " x ";
                    bool exact = spelling && !strncmp(spelling, prefix.c_str(), prefix.size());
                    if (spelling)
                        LLVMDisposeMessage(spelling);
                    if (++depth > 8 || !exact || !count || count > 256 / elements)
                        fail("NIR private array shape limit");
                    elements *= count;
                    leaf = LLVMGetElementType(leaf);
                }
                if ((!integer(scalar(leaf), 32) && !integer(scalar(leaf), 64) &&
                     LLVMGetTypeKind(scalar(leaf)) != LLVMFloatTypeKind) ||
                    lanes(leaf) > 4)
                    fail("NIR private array element type");
                auto bytes = LLVMABISizeOfType(layout, allocated);
                if (!bytes || bytes > 1024 || (bytes & 3) || privateBytes > 1024 - bytes)
                    fail("NIR private array byte limit");
                privateBytes += bytes;
                NB binding;
                binding.bytes = bytes;
                binding.storage = nir_local_variable_create(
                    b.impl, glsl_array_type(glsl_uint_type(), bytes / 4, 0), "private_array");
                if (!rootIDs.count(v))
                    rootIDs[v] = rootIDs.size() + 1;
                buffers[v] = binding;
                NV pointer;
                pointer.kind = NV::Pointer;
                pointer.root = v;
                pointer.def = addr(0);
                return values[v] = pointer;
            }
            if (LLVMABISizeOfType(layout, allocated) > 16)
                fail("NIR private object size");
            auto kind = LLVMGetTypeKind(allocated);
            if ((!floating(allocated) && kind != LLVMIntegerTypeKind && kind != LLVMVectorTypeKind) ||
                bits(allocated) > 64 || lanes(allocated) > 4)
                fail("NIR frontend unsupported private element");
            NV p;
            p.kind = NV::Private;
            p.root = v;
            rootIDs[v] = rootIDs.size() + 1;
            p.cell = nir_local_variable_create(b.impl, ntype(allocated), "private");
            return values[v] = p;
        }
        if (LLVMIsAConstantExpr(v)) {
            if (LLVMGetConstOpcode(v) != LLVMBitCast)
                fail("NIR unsupported constant expression");
            return get(LLVMGetOperand(v, 0));
        }
        if (LLVMIsAGlobalVariable(v)) {
            auto initial = LLVMGetInitializer(v);
            if (LLVMGetPointerAddressSpace(t) == 3) {
                if (stage || !initial || (!LLVMIsUndef(initial) && !LLVMIsPoison(initial)))
                    fail("NIR shared global requires uninitialized compute storage");
                auto bytes = LLVMABISizeOfType(layout, LLVMGlobalGetValueType(v));
                if (!bytes || (bytes & 3) || bytes > QGPU_MAX_THREADGROUP_BYTES ||
                    sharedBytes > QGPU_MAX_THREADGROUP_BYTES - bytes)
                    fail("NIR shared global size");
                sharedBytes += bytes;
                NB binding;
                binding.bytes = bytes;
                binding.storage = nir_variable_create(b.shader, nir_var_mem_shared,
                                                      glsl_array_type(glsl_uint_type(), bytes / 4, 0),
                                                      ("shared" + std::to_string(serial++)).c_str());
                if (!rootIDs.count(v))
                    rootIDs[v] = rootIDs.size() + 1;
                buffers[v] = binding;
                NV pointer;
                pointer.kind = NV::Pointer;
                pointer.root = v;
                pointer.def = addr(0);
                return values[v] = pointer;
            }
            if (initial && LLVMIsGlobalConstant(v) && LLVMGetPointerAddressSpace(t) == 2 &&
                LLVMGetTypeKind(LLVMTypeOf(initial)) == LLVMArrayTypeKind &&
                floating(LLVMGetElementType(LLVMTypeOf(initial)))) {
                auto element = LLVMGetElementType(LLVMTypeOf(initial));
                unsigned count = LLVMGetArrayLength(LLVMTypeOf(initial));
                if (!count || count > 256 || lanes(element) > 4)
                    fail("NIR constant table limit");
                auto &items = tables[v];
                for (unsigned n = 0; n < count; ++n)
                    items.push_back(LLVMIsAConstantAggregateZero(initial)
                                        ? zero(element)
                                        : get(LLVMIsAConstantDataArray(initial)
                                                  ? LLVMGetElementAsConstant(initial, n)
                                                  : LLVMGetOperand(initial, n)));
                NB binding;
                binding.readonly = true;
                binding.constantElement = element;
                if (!rootIDs.count(v))
                    rootIDs[v] = rootIDs.size() + 1;
                buffers[v] = binding;
                NV pointer;
                pointer.kind = NV::Pointer;
                pointer.root = v;
                pointer.def = addr(0);
                return values[v] = pointer;
            }
            uint64_t words[2] = {};
            if (!initial || LLVMGetPointerAddressSpace(t) != 2)
                fail("NIR unsupported global");
            if (integer(LLVMTypeOf(initial), 64))
                words[0] = number(initial);
            else if (LLVMGetTypeKind(LLVMTypeOf(initial)) == LLVMArrayTypeKind &&
                     LLVMGetArrayLength(LLVMTypeOf(initial)) == 2) {
                for (unsigned n = 0; n < 2; ++n)
                    words[n] =
                        number((LLVMIsAConstantDataArray(initial) ? LLVMGetElementAsConstant(initial, n)
                                                                  : LLVMGetOperand(initial, n)));
            } else
                fail("NIR unsupported global initializer");
            QGPUConstantSampler sampler{};
            if (!airConstantSampler(words[0], words[1], sampler))
                fail("NIR unsupported literal sampler");
            auto index = internConstantSampler(literals, sampler);
            if (index == UINT32_MAX)
                fail("NIR literal sampler limit");
            NV value;
            value.kind = NV::Sampler;
            value.root = v;
            samplers[v] = 8 + index;
            return values[v] = value;
        }
        if (LLVMIsUndef(v) || LLVMIsPoison(v) || LLVMIsAConstantAggregateZero(v) ||
            LLVMIsAConstantPointerNull(v))
            return zero(t);
        // LLVM 23 scalar-backed splats must retain their vector width.
        if (LLVMIsAConstant(v) && k == LLVMVectorTypeKind) {
            std::vector<nir_def *> parts;
            for (unsigned n = 0; n < lanes(t); ++n)
                parts.push_back(read(get(LLVMConstExtractElement(
                    v, LLVMConstInt(LLVMInt32TypeInContext(LLVMGetTypeContext(t)), n, false)))));
            return data(vec(parts));
        }
        if (LLVMIsAConstantInt(v)) {
            return data(nir_imm_intN_t(&b, number(v), bits(t)));
        }
        if (LLVMIsAConstantFP(v)) {
            LLVMBool loses;
            return data(nir_imm_floatN_t(&b, LLVMConstRealGetDouble(v, &loses), bits(t)));
        }
        if (LLVMIsAConstantStruct(v)) {
            NV out;
            out.kind = NV::Aggregate;
            for (unsigned n = 0; n < LLVMCountStructElementTypes(t); ++n)
                out.members.push_back(get(LLVMGetOperand(v, n)));
            return out;
        }
        if (LLVMIsAInstruction(v) &&
            (LLVMGetInstructionOpcode(v) == LLVMGetElementPtr ||
             ((LLVMGetInstructionOpcode(v) == LLVMZExt || LLVMGetInstructionOpcode(v) == LLVMSExt) &&
              integer(t, 64)))) {
            instruction(v);
            return values.at(v);
        }
        char *printed = LLVMPrintValueToString(v);
        std::string error = printed;
        LLVMDisposeMessage(printed);
        fail("NIR unsupported value: " + error);
    }
    nir_def *arg(LLVMValueRef i, unsigned n) { return read(get(LLVMGetOperand(i, n))); }
    void gep(LLVMValueRef i) {
        NV p = pointerOffset(get(LLVMGetOperand(i, 0)));
        if (p.kind != NV::Pointer)
            fail("NIR GEP base");
        auto t = LLVMGetGEPSourceElementType(i);
        uint64_t fixed = 0;
        nir_def *offset = p.def;
        for (unsigned n = 1; n < unsigned(LLVMGetNumOperands(i)); ++n) {
            auto index = LLVMGetOperand(i, n);
            if (n > 1 && LLVMGetTypeKind(t) == LLVMStructTypeKind) {
                unsigned at = number(index);
                fixed += LLVMOffsetOfElement(layout, t, at);
                t = LLVMStructGetTypeAtIndex(t, at);
                continue;
            }
            if (n > 1)
                t = LLVMGetElementType(t);
            uint64_t stride = LLVMABISizeOfType(layout, t);
            if (stride > UINT32_MAX)
                fail("NIR GEP stride");
            if (LLVMIsAConstantInt(index))
                fixed += uint64_t(LLVMConstIntGetSExtValue(index)) * stride;
            else {
                auto v = get(index);
                if (v.index == 2)
                    p.index = 2;
                offset = addAddress(offset,
                                    mulAddress(bits(LLVMTypeOf(index)) == 64 ? nir_unpack_64_2x32(&b, read(v))
                                                                             : extend(read(v), true),
                                               stride));
            }
        }
        auto address = addAddress(offset, addr(fixed));
        p.def = p.root ? address : nir_vec3(&b, c(address, 0), c(address, 1), c(p.def, 2));
        set(i, p);
    }
    nir_def *uniform(const std::string &name, unsigned components, unsigned index) {
        auto &var = uniforms[name];
        if (!var)
            var = nir_variable_create(b.shader, nir_var_uniform,
                                      glsl_array_type(glsl_vector_type(GLSL_TYPE_UINT, components), 8, 0),
                                      name.c_str());
        return nir_load_deref(&b, nir_build_deref_array_imm(&b, nir_build_deref_var(&b, var), index));
    }
    unsigned physicalBuffer(NV p, bool store) {
        auto &binding = buffers.at(p.root);
        if (store && (binding.readonly || stage == 1))
            fail("NIR readonly store");
        reflection.bufferMask |= 1u << binding.slot;
        if (store)
            reflection.writeMask |= 1u << binding.slot;
        if (p.index == 2)
            reflection.instanceMask |= 1u << binding.slot;
        unsigned physical = binding.slot + (stage == 2 ? 8 : 0);
        b.shader->info.num_ssbos = std::max<unsigned>(b.shader->info.num_ssbos, physical + 1);
        return physical;
    }
    nir_def *storageAddress(NV p, unsigned component) {
        auto &binding = buffers.at(p.root);
        auto address = addAddress(p.def, addr(component * 4));
        auto offset = c(address, 0);
        auto size = u(binding.bytes);
        auto base = u(0);
        if (binding.dynamicSlot != UINT32_MAX) {
            auto info = uniform("_qthreadgroup_info", 2, binding.dynamicSlot);
            base = c(info, 0);
            size = c(info, 1);
        }
        auto valid = nir_iand(&b, nir_ieq_imm(&b, c(address, 1), 0),
                              nir_iand(&b, nir_ieq_imm(&b, nir_iand_imm(&b, offset, 3), 0),
                                       nir_iand(&b, nir_uge_imm(&b, size, 4),
                                                nir_uge(&b, nir_iadd_imm(&b, size, -4), offset))));
        valid = nir_iand(&b, valid, nir_ieq_imm(&b, c(p.def, 1), 0));
        auto total = nir_iadd(&b, base, offset);
        valid = nir_iand(&b, valid, nir_uge(&b, total, base));
        valid = nir_iand(&b, valid, nir_ult(&b, total, u(binding.bytes)));
        return nir_bcsel(&b, valid, nir_ushr_imm(&b, total, 2), u(binding.bytes / 4));
    }
    nir_def *ssboLoad(unsigned slot, nir_def *offset, unsigned components, unsigned width,
                      gl_access_qualifier access = ACCESS_NON_WRITEABLE) {
        _nir_load_ssbo_indices attributes{};
        attributes.access = access;
        attributes.align_mul = width / 8;
        return _nir_build_load_ssbo(&b, components, width, u(slot), offset, attributes);
    }
    nir_def *privateAccess(NV p, unsigned width, unsigned components, nir_def *value = nullptr) {
        p = pointerOffset(p);
        if (p.kind != NV::Pointer)
            fail("NIR intrinsic output requires private storage");
        auto result = nir_local_variable_create(b.impl,
                                                glsl_vector_type(width == 64   ? GLSL_TYPE_UINT64
                                                                 : width == 16 ? GLSL_TYPE_UINT16
                                                                               : GLSL_TYPE_UINT,
                                                                 components),
                                                "private_result");
        nir_store_var(&b, result, nir_imm_zero(&b, components, width), (1u << components) - 1);
        for (const auto &root : pointerRoots(p)) {
            auto storage = values.at(root.first);
            if (storage.kind != NV::Private || glsl_get_bit_size(storage.cell->type) != width ||
                glsl_get_vector_elements(storage.cell->type) != components)
                fail("NIR intrinsic private output type");
            auto valid = nir_iand(&b, nir_ieq_imm(&b, c(p.def, 0), 0), nir_ieq_imm(&b, c(p.def, 1), 0));
            if (!p.root)
                valid = nir_iand(&b, valid, nir_ieq_imm(&b, c(p.def, 2), root.second));
            nir_push_if(&b, valid);
            if (value)
                nir_store_var(&b, storage.cell, value, (1u << components) - 1);
            else
                nir_store_var(&b, result, nir_load_var(&b, storage.cell), (1u << components) - 1);
            nir_pop_if(&b, nullptr);
        }
        return nir_load_var(&b, result);
    }
    void memory(LLVMValueRef i, bool store) {
        NV p = pointerOffset(get(LLVMGetOperand(i, store ? 1 : 0)));
        if (p.kind == NV::Pointer && !p.root) {
            if (!store)
                set(i, zero(LLVMTypeOf(i)));
            for (const auto &root : pointerRoots(p)) {
                nir_push_if(&b, nir_ieq_imm(&b, c(p.def, 2), root.second));
                NV selected = p;
                selected.root = root.first;
                memoryAt(i, store, selected);
                nir_pop_if(&b, nullptr);
            }
        } else
            memoryAt(i, store, p);
    }
    void memoryAt(LLVMValueRef i, bool store, NV p) {
        auto t = store ? LLVMTypeOf(LLVMGetOperand(i, 0)) : LLVMTypeOf(i);
        if (LLVMGetOrdering(i) != LLVMAtomicOrderingNotAtomic) {
            if (!integer(t, 32) || (LLVMGetOrdering(i) != LLVMAtomicOrderingMonotonic &&
                                    LLVMGetOrdering(i) != LLVMAtomicOrderingUnordered))
                fail("NIR atomic memory requires relaxed int32");
            auto value = atomic(p, store ? "xchg" : "or", store ? arg(i, 0) : u(0));
            if (!store)
                set(i, value);
            return;
        }
        if (p.kind == NV::Pointer && p.root && values.count(p.root) &&
            values.at(p.root).kind == NV::Private) {
            auto storage = values.at(p.root);
            if (bits(t) != glsl_get_bit_size(storage.cell->type) ||
                lanes(t) != glsl_get_vector_elements(storage.cell->type))
                fail("NIR private access type");
            auto valid = nir_iand(&b, nir_ieq_imm(&b, c(p.def, 0), 0), nir_ieq_imm(&b, c(p.def, 1), 0));
            if (store) {
                nir_push_if(&b, valid);
                nir_store_var(&b, storage.cell, arg(i, 0), (1u << lanes(t)) - 1);
                nir_pop_if(&b, nullptr);
            } else
                set(i, nir_bcsel(&b, valid, nir_load_var(&b, storage.cell),
                                 nir_imm_zero(&b, lanes(t), bits(t))));
            return;
        }
        if (p.kind == NV::Private) {
            if (bits(t) != glsl_get_bit_size(p.cell->type) ||
                lanes(t) != glsl_get_vector_elements(p.cell->type))
                fail("NIR private access type");
            if (store)
                nir_store_var(&b, p.cell, arg(i, 0), (1u << lanes(t)) - 1);
            else
                set(i, nir_load_var(&b, p.cell));
            return;
        }
        if (p.kind != NV::Pointer || !buffers.count(p.root))
            fail("NIR undeclared buffer");
        auto binding = buffers.at(p.root);
        if (store && binding.readonly)
            fail("NIR readonly store");
        if (binding.constantElement) {
            if (store || (t != binding.constantElement && t != scalar(binding.constantElement)))
                fail("NIR constant table access");
            unsigned stride = LLVMABISizeOfType(layout, binding.constantElement);
            auto index = nir_udiv_imm(&b, c(p.def, 0), stride);
            auto &items = tables.at(p.root);
            auto value = read(items.back());
            for (unsigned n = 0; n + 1 < items.size(); ++n)
                value = nir_bcsel(&b, nir_ieq_imm(&b, index, n), read(items[n]), value);
            if (t != binding.constantElement)
                value = nir_vector_extract(
                    &b, value,
                    nir_udiv_imm(&b, nir_umod(&b, c(p.def, 0), u(stride)), LLVMABISizeOfType(layout, t)));
            set(i, value);
            return;
        }
        if (bits(t) == 64) {
            std::vector<nir_def *> result;
            auto physical = binding.storage ? 0 : physicalBuffer(p, store);
            auto access = gl_access_qualifier((binding.readonly ? ACCESS_NON_WRITEABLE : 0) |
                                              (LLVMGetVolatile(i) ? ACCESS_VOLATILE : 0));
            for (unsigned n = 0; n < lanes(t); ++n) {
                nir_def *words[2];
                auto source = store ? nir_unpack_64_2x32(&b, c(arg(i, 0), n)) : nullptr;
                for (unsigned k = 0; k < 2; ++k) {
                    if (binding.storage) {
                        auto word = storageAddress(p, 2 * n + k);
                        nir_push_if(&b, nir_ult_imm(&b, word, binding.bytes / 4));
                        auto deref =
                            nir_build_deref_array(&b, nir_build_deref_var(&b, binding.storage), word);
                        if (store) {
                            nir_store_deref(&b, deref, c(source, k), 1);
                            nir_pop_if(&b, nullptr);
                        } else {
                            auto value = nir_load_deref(&b, deref);
                            nir_push_else(&b, nullptr);
                            auto zero = u(0);
                            nir_pop_if(&b, nullptr);
                            words[k] = nir_if_phi(&b, value, zero);
                        }
                    } else {
                        auto address = addAddress(p.def, addr(8 * n + 4 * k));
                        auto valid = nir_iand(&b, nir_ieq_imm(&b, c(address, 1), 0),
                                              nir_ieq_imm(&b, nir_iand_imm(&b, c(address, 0), 3), 0));
                        valid = nir_iand(&b, valid, nir_ieq_imm(&b, c(p.def, 1), 0));
                        auto offset = nir_bcsel(&b, valid, c(address, 0), u(0xfffffffcu));
                        if (store) {
                            _nir_store_ssbo_indices attrs{};
                            attrs.write_mask = 1;
                            attrs.align_mul = 4;
                            attrs.access = access;
                            _nir_build_store_ssbo(&b, c(source, k), u(physical), offset, attrs);
                        } else
                            words[k] = ssboLoad(physical, offset, 1, 32, access);
                    }
                }
                if (!store)
                    result.push_back(nir_pack_64_2x32(&b, nir_vec2(&b, words[0], words[1])));
            }
            if (!store)
                set(i, vec(result));
            return;
        }
        if (binding.storage) {
            if (bits(t) != 32 || lanes(t) > 4)
                fail("NIR shared/private memory requires 32-bit elements");
            std::vector<nir_def *> result;
            for (unsigned n = 0; n < lanes(t); ++n) {
                auto word = storageAddress(p, n);
                nir_push_if(&b, nir_ult_imm(&b, word, binding.bytes / 4));
                auto deref = nir_build_deref_array(&b, nir_build_deref_var(&b, binding.storage), word);
                if (store) {
                    nir_store_deref(&b, deref, c(arg(i, 0), n), 1);
                    nir_pop_if(&b, nullptr);
                } else {
                    auto value = nir_load_deref(&b, deref);
                    nir_push_else(&b, nullptr);
                    auto zero = u(0);
                    nir_pop_if(&b, nullptr);
                    result.push_back(nir_if_phi(&b, value, zero));
                }
            }
            if (!store)
                set(i, vec(result));
            return;
        }
        unsigned width = bits(t), bytes = width / 8;
        if (!bytes || bytes > 4 || lanes(t) > 4)
            fail("NIR buffer element width");
        unsigned physical = physicalBuffer(p, store);
        gl_access_qualifier access = gl_access_qualifier((binding.readonly ? ACCESS_NON_WRITEABLE : 0) |
                                                         (LLVMGetVolatile(i) ? ACCESS_VOLATILE : 0));
        if (integer(scalar(t), 8) || integer(scalar(t), 16)) {
            if (store)
                fail("NIR packed integer stores outside buffer profile");
            std::vector<nir_def *> result;
            for (unsigned n = 0; n < lanes(t); ++n) {
                auto value = u(0);
                for (unsigned byte = 0; byte < bytes; ++byte) {
                    auto a = addAddress(p.def, addr(n * bytes + byte));
                    auto offset = nir_bcsel(&b, nir_ieq_imm(&b, c(a, 1), 0), nir_iand_imm(&b, c(a, 0), ~3u),
                                            u(0xfffffffcu));
                    auto word = ssboLoad(physical, offset, 1, 32, access);
                    auto shift = nir_ishl_imm(&b, nir_iand_imm(&b, c(a, 0), 3), 3);
                    value =
                        nir_ior(&b, value,
                                nir_ishl_imm(&b, nir_iand_imm(&b, nir_ushr(&b, word, shift), 255), byte * 8));
                }
                result.push_back(nir_u2uN(&b, value, width));
            }
            set(i, vec(result));
            return;
        }
        auto valid = nir_iand(&b, nir_ieq_imm(&b, c(p.def, 1), 0),
                              nir_ieq_imm(&b, nir_iand_imm(&b, c(p.def, 0), bytes - 1), 0));
        auto offset = nir_bcsel(&b, valid, c(p.def, 0), u(bytes == 2 ? 0xfffffffeu : 0xfffffffcu));
        if (store) {
            _nir_store_ssbo_indices attributes{};
            attributes.write_mask = (1u << lanes(t)) - 1;
            attributes.align_mul = bytes;
            attributes.access = access;
            _nir_build_store_ssbo(&b, arg(i, 0), u(physical), offset, attributes);
        } else
            set(i, ssboLoad(physical, offset, lanes(t), width, access));
    }
    nir_def *atomic(NV p, const std::string &op, nir_def *value, nir_def *compare = nullptr,
                    bool sign = false) {
        p = pointerOffset(p);
        if (p.kind == NV::Pointer && !p.root) {
            auto result = nir_local_variable_create(b.impl, glsl_uint_type(), "selected_atomic");
            nir_store_var(&b, result, u(0), 1);
            for (const auto &root : pointerRoots(p)) {
                nir_push_if(&b, nir_ieq_imm(&b, c(p.def, 2), root.second));
                NV selected = p;
                selected.root = root.first;
                nir_store_var(&b, result, atomic(selected, op, value, compare, sign), 1);
                nir_pop_if(&b, nullptr);
            }
            return nir_load_var(&b, result);
        }
        if (p.kind != NV::Pointer || !buffers.count(p.root))
            fail("NIR atomic resource");
        auto binding = buffers.at(p.root);
        if (binding.readonly || stage == 1 ||
            (binding.storage && binding.storage->data.mode != nir_var_mem_shared))
            fail("NIR atomic memory access");
        nir_atomic_op operation = op == "cas"    ? nir_atomic_op_cmpxchg
                                  : op == "xchg" ? nir_atomic_op_xchg
                                  : op == "and"  ? nir_atomic_op_iand
                                  : op == "or"   ? nir_atomic_op_ior
                                  : op == "xor"  ? nir_atomic_op_ixor
                                  : op == "min"  ? (sign ? nir_atomic_op_imin : nir_atomic_op_umin)
                                  : op == "max"  ? (sign ? nir_atomic_op_imax : nir_atomic_op_umax)
                                                 : nir_atomic_op_iadd;
        if (op == "sub")
            value = nir_ineg(&b, value);
        auto result = nir_local_variable_create(b.impl, glsl_uint_type(), "atomic_result");
        nir_store_var(&b, result, u(0), 1);
        if (binding.storage) {
            auto word = storageAddress(p, 0);
            nir_push_if(&b, nir_ult_imm(&b, word, binding.bytes / 4));
            auto ptr = &nir_build_deref_array(&b, nir_build_deref_var(&b, binding.storage), word)->def;
            nir_def *previous;
            if (compare) {
                _nir_deref_atomic_swap_indices a{};
                a.atomic_op = operation;
                previous = _nir_build_deref_atomic_swap(&b, 32, ptr, compare, value, a);
            } else {
                _nir_deref_atomic_indices a{};
                a.atomic_op = operation;
                previous = _nir_build_deref_atomic(&b, 32, ptr, value, a);
            }
            nir_store_var(&b, result, previous, 1);
            nir_pop_if(&b, nullptr);
        } else {
            auto physical = physicalBuffer(p, true);
            auto valid = nir_iand(&b, nir_ieq_imm(&b, c(p.def, 1), 0),
                                  nir_ieq_imm(&b, nir_iand_imm(&b, c(p.def, 0), 3), 0));
            auto offset = nir_bcsel(&b, valid, c(p.def, 0), u(0xfffffffcu));
            nir_def *previous;
            if (compare) {
                _nir_ssbo_atomic_swap_indices a{};
                a.atomic_op = operation;
                previous = _nir_build_ssbo_atomic_swap(&b, 32, u(physical), offset, compare, value, a);
            } else {
                _nir_ssbo_atomic_indices a{};
                a.atomic_op = operation;
                previous = _nir_build_ssbo_atomic(&b, 32, u(physical), offset, value, a);
            }
            nir_store_var(&b, result, previous, 1);
        }
        return nir_load_var(&b, result);
    }
    nir_def *nativeConstants(unsigned base, unsigned count = 4) {
        _nir_load_const_ir3_indices attributes{};
        return _nir_build_load_const_ir3(&b, count, 32, u(base), attributes);
    }
    nir_def *derivative(nir_def *value, bool y) {
        // This IR3 revision implements derivatives one component at a time.
        // The GLSL frontend already emits this scalar form.
        std::vector<nir_def *> parts;
        for (unsigned n = 0; n < value->num_components; ++n)
            parts.push_back(y ? nir_ddy(&b, c(value, n)) : nir_ddx(&b, c(value, n)));
        return vec(parts);
    }
    nir_def *floatConvert(nir_def *v, LLVMTypeRef from, LLVMTypeRef to, bool signedFrom = false,
                          bool signedTo = false) {
        if (floating(to)) {
            if (floating(from))
                return nir_f2fN(&b, v, bits(to));
            if (integer(scalar(from), 1)) {
                // Integer zero converts to +0. Negating b2f would produce -0.
                return signedFrom ? nir_i2fN(&b, nir_ineg(&b, nir_b2i32(&b, v)), bits(to))
                                  : nir_b2fN(&b, v, bits(to));
            }
            return signedFrom ? nir_i2fN(&b, v, bits(to)) : nir_u2fN(&b, v, bits(to));
        }
        if (integer(scalar(to), 1)) {
            if (floating(from))
                v = signedTo ? nir_f2i32(&b, v) : nir_f2u32(&b, v);
            return v->bit_size == 1 ? v : nir_ine_imm(&b, nir_iand_imm(&b, v, 1), 0);
        }
        if (floating(from))
            return signedTo ? nir_f2iN(&b, v, bits(to)) : nir_f2uN(&b, v, bits(to));
        if (integer(scalar(from), 1)) {
            auto converted = nir_b2iN(&b, v, bits(to));
            return signedFrom ? nir_ineg(&b, converted) : converted;
        }
        return signedFrom ? nir_i2iN(&b, v, bits(to)) : nir_u2uN(&b, v, bits(to));
    }
    nir_def *fcmp(unsigned predicate, nir_def *a, nir_def *d) {
        auto ordered = nir_iand(&b, nir_feq(&b, a, a), nir_feq(&b, d, d));
        nir_def *cmp = nullptr;
        switch (predicate & 7) {
        case 0:
            cmp = nir_imm_bool(&b, false);
            break;
        case 1:
            cmp = nir_feq(&b, a, d);
            break;
        case 2:
            cmp = nir_flt(&b, d, a);
            break;
        case 3:
            cmp = nir_fge(&b, a, d);
            break;
        case 4:
            cmp = nir_flt(&b, a, d);
            break;
        case 5:
            cmp = nir_fge(&b, d, a);
            break;
        case 6:
            cmp = nir_fneu(&b, a, d);
            break;
        case 7:
            cmp = nir_imm_bool(&b, true);
            break;
        }
        if (predicate >= 8)
            return nir_ior(&b, nir_inot(&b, ordered), cmp);
        return nir_iand(&b, ordered, cmp);
    }
    nir_def *tex(nir_texop op, unsigned unit, nir_def *coordinate, nir_def *lod = nullptr,
                 nir_def *dx = nullptr, nir_def *dy = nullptr, unsigned shape = 0,
                 nir_def *offset = nullptr) {
        unsigned count = 1 + (lod ? 1 : 0) + (dx ? 2 : 0) + (offset ? 1 : 0);
        auto t = nir_tex_instr_create(b.shader, count);
        t->op = op;
        t->sampler_dim = op == nir_texop_txf_ms ? GLSL_SAMPLER_DIM_MS
                         : shape & 2            ? GLSL_SAMPLER_DIM_CUBE
                                                : GLSL_SAMPLER_DIM_2D;
        t->is_array = shape & 1;
        t->coord_components = coordinate->num_components;
        t->dest_type = nir_type_float32;
        t->texture_index = unit;
        t->sampler_index = unit;
        t->src[0] = {nir_src_for_ssa(coordinate), nir_tex_src_coord};
        unsigned n = 1;
        if (lod)
            t->src[n++] = {nir_src_for_ssa(lod), op == nir_texop_txb      ? nir_tex_src_bias
                                                 : op == nir_texop_txf_ms ? nir_tex_src_ms_index
                                                                          : nir_tex_src_lod};
        if (dx) {
            t->src[n++] = {nir_src_for_ssa(dx), nir_tex_src_ddx};
            t->src[n++] = {nir_src_for_ssa(dy), nir_tex_src_ddy};
        }
        if (offset)
            t->src[n++] = {nir_src_for_ssa(offset), nir_tex_src_offset};
        nir_def_init(&t->instr, &t->def, 4, 32);
        nir_builder_instr_insert(&b, &t->instr);
        return &t->def;
    }
    nir_def *sample(unsigned texture, unsigned sampler, nir_def *coord, nir_def *bias, bool level,
                    const QGPUConstantSampler *literal) {
        unsigned unit = literal ? 256 + texture * 16 + sampler - 8 + (level ? 128 : 0)
                                : texture * 8 + sampler + (level ? 64 : 0);
        nir_def *flags = literal ? u(Shader::samplerMirrorFlags(*literal)) : nativeConstants(sampler, 1);
        nir_def *p[2], *direction[2];
        for (unsigned n = 0; n < 2; ++n) {
            auto mirrored = nir_ine_imm(&b, nir_iand_imm(&b, flags, 1u << n), 0);
            auto raw = c(coord, n);
            p[n] = nir_bcsel(&b, mirrored, nir_fabs(&b, raw), raw);
            direction[n] = nir_bcsel(&b, nir_iand(&b, mirrored, nir_flt_imm(&b, raw, 0)),
                                     nir_imm_float(&b, -1), nir_imm_float(&b, 1));
        }
        auto point = nir_vec2(&b, p[0], p[1]);
        if (level)
            return tex(nir_texop_txl, unit, point, bias);
        if (literal && !Shader::samplerMirrorFlags(*literal))
            return tex(nir_texop_txb, unit, coord, bias);
        auto result = nir_local_variable_create(b.impl, glsl_vec4_type(), "sample");
        // Match production's implicit path when an external sampler needs no
        // mirror fix; compute derivatives before control flow.
        nir_def *scale = nir_fexp2(&b, bias), *dx = nir_fmul(&b, derivative(coord, false), scale),
                *dy = nir_fmul(&b, derivative(coord, true), scale);
        if (!literal) {
            nir_push_if(&b, nir_ieq_imm(&b, nir_iand_imm(&b, flags, 3), 0));
            nir_store_var(&b, result, tex(nir_texop_txb, unit, coord, bias), 15);
            nir_push_else(&b, nullptr);
        }
        auto dir = nir_vec2(&b, direction[0], direction[1]);
        nir_store_var(&b, result,
                      tex(nir_texop_txd, unit, point, nullptr, nir_fmul(&b, dx, dir), nir_fmul(&b, dy, dir)),
                      15);
        if (!literal)
            nir_pop_if(&b, nullptr);
        return nir_load_var(&b, result);
    }
    nir_def *sampleGeneral(const AIRTextureBinding &binding, unsigned sampler, nir_def *coord, nir_def *lod,
                           bool level, nir_def *layer, nir_def *dx, nir_def *dy, nir_def *offset,
                           const QGPUConstantSampler *literal) {
        if (stage == 2 && !binding.shape && !dx && !offset)
            return sample(binding.slot, sampler, coord, lod, level, literal);
        unsigned unit = literal ? 256 + binding.slot * 16 + sampler - 8 + (level ? 128 : 0)
                                : binding.slot * 8 + sampler + (level ? 64 : 0);
        bool cube = binding.shape & 2, array = binding.shape & 1;
        auto point = [&](nir_def *xy) {
            if (!array)
                return xy;
            std::vector<nir_def *> parts;
            for (unsigned n = 0; n < xy->num_components; ++n)
                parts.push_back(c(xy, n));
            parts.push_back(nir_u2f32(&b, layer));
            return vec(parts);
        };
        if (cube)
            return tex(dx                      ? nir_texop_txd
                       : (level || stage != 2) ? nir_texop_txl
                                               : nir_texop_txb,
                       unit, point(coord), dx ? nullptr : lod, dx, dy, binding.shape, offset);
        auto flags = literal ? u(Shader::samplerMirrorFlags(*literal))
                             : uniform(stage == 1 ? "_qvertex_sampler_info" : "_qsampler_info", 1, sampler);
        nir_def *p[2], *direction[2];
        for (unsigned n = 0; n < 2; ++n) {
            auto mirror = nir_ine_imm(&b, nir_iand_imm(&b, flags, 1u << n), 0), raw = c(coord, n);
            p[n] = nir_bcsel(&b, mirror, nir_fabs(&b, raw), raw);
            direction[n] = nir_bcsel(&b, nir_iand(&b, mirror, nir_flt_imm(&b, raw, 0)), nir_imm_float(&b, -1),
                                     nir_imm_float(&b, 1));
        }
        auto transformed = point(nir_vec2(&b, p[0], p[1]));
        if (level || (stage != 2 && !dx))
            return tex(nir_texop_txl, unit, transformed, lod, nullptr, nullptr, binding.shape, offset);
        if (!dx && literal && !Shader::samplerMirrorFlags(*literal))
            return tex(nir_texop_txb, unit, point(coord), lod, nullptr, nullptr, binding.shape, offset);
        bool implicit = !dx;
        if (implicit) {
            auto scale = nir_fexp2(&b, lod);
            dx = nir_fmul(&b, derivative(coord, false), scale);
            dy = nir_fmul(&b, derivative(coord, true), scale);
        }
        auto dir = nir_vec2(&b, direction[0], direction[1]);
        if (!implicit || literal)
            return tex(nir_texop_txd, unit, transformed, nullptr, nir_fmul(&b, dx, dir),
                       nir_fmul(&b, dy, dir), binding.shape, offset);
        auto result = nir_local_variable_create(b.impl, glsl_vec4_type(), "sample");
        nir_push_if(&b, nir_ieq_imm(&b, nir_iand_imm(&b, flags, 3), 0));
        nir_store_var(&b, result,
                      tex(nir_texop_txb, unit, point(coord), lod, nullptr, nullptr, binding.shape, offset),
                      15);
        nir_push_else(&b, nullptr);
        nir_store_var(&b, result,
                      tex(nir_texop_txd, unit, transformed, nullptr, nir_fmul(&b, dx, dir),
                          nir_fmul(&b, dy, dir), binding.shape, offset),
                      15);
        nir_pop_if(&b, nullptr);
        return nir_load_var(&b, result);
    }
    nir_def *imageInfo(const AIRTextureBinding &binding, bool array = false) {
        if (stage)
            return uniform(graphicsTexturePrefix(binding.shape, stage) + "_info", 4, binding.slot);
        auto name = binding.shape & 2 ? cubeImagePrefix(binding.shape & 1) + (array ? "_face_info" : "_info")
                                      : std::string(array ? "_qimage_array_info" : "_qimage_info");
        return uniform(name, array ? 2 : 4, binding.slot);
    }
    nir_def *imageSize(nir_def *info, nir_def *lod) {
        auto levels = nir_iand_imm(&b, nir_ushr_imm(&b, c(info, 3), 8), 255);
        return nir_bcsel(&b, nir_ult(&b, lod, levels),
                         nir_umax(&b, nir_ushr(&b, nir_channels(&b, info, 3), lod), nir_imm_ivec2(&b, 1, 1)),
                         nir_imm_ivec2(&b, 0, 0));
    }
    nir_def *swizzleImage(nir_def *value, nir_def *encoding, bool integerValue) {
        auto swizzle = nir_iadd_imm(&b, encoding, -1);
        auto zero = integerValue ? u(0) : nir_imm_float(&b, 0),
             one = integerValue ? u(1) : nir_imm_float(&b, 1);
        std::vector<nir_def *> channels;
        for (unsigned n = 0; n < 4; ++n) {
            auto code = nir_iand_imm(&b, nir_ushr_imm(&b, swizzle, n * 3), 7);
            auto channel = nir_bcsel(
                &b, nir_ieq_imm(&b, code, 0), zero,
                nir_bcsel(&b, nir_ieq_imm(&b, code, 1), one,
                          nir_vector_extract(&b, value, nir_umin(&b, nir_iadd_imm(&b, code, -2), u(3)))));
            channels.push_back(nir_bcsel(&b, nir_ieq_imm(&b, encoding, 0), c(value, n), channel));
        }
        return vec(channels);
    }
    nir_def *imageZero(nir_def *format, bool integerValue) {
        auto zero = integerValue ? u(0) : nir_imm_float(&b, 0);
        auto alpha = integerValue ? u(1)
                                  : nir_b2f32(&b, nir_ior(&b, nir_ieq_imm(&b, format, Shader::R8),
                                                          nir_ieq_imm(&b, format, Shader::RG8)));
        return nir_vec4(&b, zero, zero, zero, alpha);
    }
    nir_def *imageCPP(nir_def *format) {
        return nir_bcsel(&b, nir_ieq_imm(&b, format, Shader::R8), u(1),
                         nir_bcsel(&b, nir_ieq_imm(&b, format, Shader::RG8), u(2),
                                   nir_bcsel(&b, nir_ieq_imm(&b, format, Shader::RGBA16Float), u(8), u(4))));
    }
    nir_def *srgb(nir_def *value, bool encode) {
        auto rgb = nir_channels(&b, value, 7);
        auto linear = nir_fmul_imm(&b, rgb, encode ? 12.92 : 1.0 / 12.92);
        auto nonlinear =
            encode ? nir_fadd_imm(
                         &b, nir_fmul_imm(&b, nir_fpow(&b, rgb, nir_imm_float(&b, 1.0 / 2.4)), 1.055), -0.055)
                   : nir_fpow(&b, nir_fdiv(&b, nir_fadd_imm(&b, rgb, 0.055), nir_imm_float(&b, 1.055)),
                              nir_imm_float(&b, 2.4));
        auto out = nir_bcsel(&b, nir_flt(&b, nir_imm_float(&b, encode ? 0.0031308 : 0.04045), rgb), nonlinear,
                             linear);
        return nir_vec4(&b, c(out, 0), c(out, 1), c(out, 2), c(value, 3));
    }
    nir_def *imageReadWrite(const AIRTextureBinding &binding, nir_def *coord, nir_def *lod, nir_def *layer,
                            nir_def *face, nir_def *writeValue) {
        auto info = imageInfo(binding), size = imageSize(info, lod),
             format = nir_iand_imm(&b, c(info, 3), 255);
        auto valid = nir_iand(&b, nir_ult(&b, c(coord, 0), c(size, 0)), nir_ult(&b, c(coord, 1), c(size, 1)));
        auto slice = layer ? layer : u(0), count = u(1), stride = u(0);
        if (binding.shape) {
            auto array = stage ? info : imageInfo(binding, true);
            count = c(array, stage ? 2 : 0);
            if (!stage)
                stride = c(array, 1);
            if (binding.shape & 2) {
                valid = nir_iand(&b, valid, nir_ult_imm(&b, face, 6));
                if (!stage)
                    count = nir_udiv_imm(&b, count, 6);
                slice = nir_iadd(&b, nir_imul_imm(&b, slice, 6), face);
            }
            if (binding.shape & 1)
                valid = nir_iand(&b, valid, nir_ult(&b, layer, count));
        }
        if (stage) {
            auto result = nir_local_variable_create(b.impl, glsl_vec4_type(), "fetch");
            nir_push_if(&b, valid);
            auto point = binding.shape ? nir_vec3(&b, c(coord, 0), c(coord, 1), slice) : coord;
            // Cube fetches use the native 2D-array descriptor view.
            nir_store_var(&b, result,
                          tex(nir_texop_txf, 128 + binding.shape * 8 + binding.slot, point, lod, nullptr,
                              nullptr, binding.shape ? 1 : 0),
                          15);
            nir_push_else(&b, nullptr);
            nir_store_var(&b, result,
                          swizzleImage(imageZero(format, false),
                                       nir_iand_imm(&b, nir_ushr_imm(&b, c(info, 3), 16), 4095), false),
                          15);
            nir_pop_if(&b, nullptr);
            return nir_load_var(&b, result);
        }
        unsigned slot = QGPU_COMPUTE_IMAGE_BASE + binding.slot;
        b.shader->info.num_ssbos = std::max<unsigned>(b.shader->info.num_ssbos, slot + 1);
        reflection.imageMask |= 1u << binding.slot;
        if (writeValue)
            reflection.imageWriteMask |= 1u << binding.slot;
        else
            reflection.imageReadMask |= 1u << binding.slot;
        auto result = nir_local_variable_create(
            b.impl, binding.unsignedInteger ? glsl_uvec4_type() : glsl_vec4_type(), "image_result");
        nir_store_var(&b, result, imageZero(format, binding.unsignedInteger), 15);
        nir_push_if(&b, valid);
        auto cpp = imageCPP(format), alignment = nir_imul_imm(&b, cpp, 64),
             alignMask = nir_ineg(&b, alignment);
        auto pitchFor = [&](nir_def *level) {
            return nir_iand(&b,
                            nir_iadd(&b, nir_umax(&b, nir_ushr(&b, c(info, 2), level), u(1)),
                                     nir_iadd_imm(&b, alignment, -1)),
                            alignMask);
        };
        auto offset = nir_local_variable_create(b.impl, glsl_uint_type(), "mip_offset"),
             level = nir_local_variable_create(b.impl, glsl_uint_type(), "mip_level");
        nir_store_var(&b, offset, u(0), 1);
        nir_store_var(&b, level, u(0), 1);
        nir_push_loop(&b);
        auto current = nir_load_var(&b, level);
        nir_push_if(&b, nir_uge(&b, current, lod));
        nir_jump(&b, nir_jump_break);
        nir_pop_if(&b, nullptr);
        auto bytes = nir_imul(&b, pitchFor(current), nir_umax(&b, nir_ushr(&b, c(info, 1), current), u(1)));
        nir_store_var(&b, offset, nir_iadd(&b, nir_load_var(&b, offset), bytes), 1);
        nir_store_var(&b, level, nir_iadd_imm(&b, current, 1), 1);
        nir_pop_loop(&b, nullptr);
        auto levels = nir_iand_imm(&b, nir_ushr_imm(&b, c(info, 3), 8), 255);
        auto pitch = nir_bcsel(&b, nir_ult(&b, u(1), levels), pitchFor(lod), c(info, 2));
        auto address =
            nir_iadd(&b, nir_load_var(&b, offset),
                     nir_iadd(&b, nir_imul(&b, c(coord, 1), pitch), nir_imul(&b, c(coord, 0), cpp)));
        if (binding.shape)
            address = nir_iadd(&b, address, nir_imul(&b, slice, stride));
        auto wordAddress = nir_iand_imm(&b, address, ~3u);
        auto shift = nir_ishl_imm(&b, nir_iand_imm(&b, nir_imul(&b, c(coord, 0), cpp), 3), 3);
        auto load = [&](unsigned relative) {
            return ssboLoad(slot, nir_iadd_imm(&b, wordAddress, relative), 1, 32, ACCESS_COHERENT);
        };
        auto store = [&](nir_def *v, unsigned relative) {
            _nir_store_ssbo_indices a{};
            a.write_mask = 1;
            a.align_mul = 4;
            a.access = ACCESS_COHERENT;
            _nir_build_store_ssbo(&b, v, u(slot), nir_iadd_imm(&b, wordAddress, relative), a);
        };
        if (binding.unsignedInteger) {
            if (writeValue)
                store(c(writeValue, 0), 0);
            else
                nir_store_var(&b, result, nir_vec4(&b, load(0), u(0), u(0), u(1)), 15);
        } else {
            nir_push_if(&b, nir_ieq_imm(&b, format, Shader::RGBA16Float));
            if (writeValue) {
                store(nir_pack_half_2x16(&b, nir_channels(&b, writeValue, 3)), 0);
                store(nir_pack_half_2x16(&b, nir_channels(&b, writeValue, 12)), 4);
            } else {
                auto lo = nir_unpack_half_2x16(&b, load(0)), hi = nir_unpack_half_2x16(&b, load(4));
                nir_store_var(&b, result, nir_vec4(&b, c(lo, 0), c(lo, 1), c(hi, 0), c(hi, 1)), 15);
            }
            nir_push_else(&b, nullptr);
            auto isSRGB = nir_ior(&b, nir_ieq_imm(&b, format, 5), nir_ieq_imm(&b, format, 6));
            auto isBGRA = nir_ior(&b, nir_ieq_imm(&b, format, 2), nir_ieq_imm(&b, format, 6));
            if (writeValue) {
                auto value = nir_fsat(&b, nir_bcsel(&b, nir_fneu(&b, writeValue, writeValue),
                                                    nir_imm_float(&b, 0), writeValue));
                value = nir_bcsel(&b, isSRGB, srgb(value, true), value);
                unsigned bgra[] = {2, 1, 0, 3};
                value = nir_bcsel(&b, isBGRA, nir_swizzle(&b, value, bgra, 4), value);
                auto encoded = nir_pack_unorm_4x8(&b, value);
                nir_push_if(&b, nir_ieq_imm(&b, cpp, 4));
                store(encoded, 0);
                nir_push_else(&b, nullptr);
                auto mask = nir_ishl(&b, nir_bcsel(&b, nir_ieq_imm(&b, cpp, 1), u(255), u(65535)), shift);
                auto expected = nir_local_variable_create(b.impl, glsl_uint_type(), "packed_expected");
                nir_store_var(&b, expected, u(0), 1);
                nir_push_loop(&b);
                auto old = nir_load_var(&b, expected);
                auto desired = nir_ior(&b, nir_iand(&b, old, nir_inot(&b, mask)),
                                       nir_iand(&b, nir_ishl(&b, encoded, shift), mask));
                _nir_ssbo_atomic_swap_indices a{};
                a.atomic_op = nir_atomic_op_cmpxchg;
                a.access = ACCESS_COHERENT;
                auto observed = _nir_build_ssbo_atomic_swap(&b, 32, u(slot), wordAddress, old, desired, a);
                nir_push_if(&b, nir_ieq(&b, observed, old));
                nir_jump(&b, nir_jump_break);
                nir_pop_if(&b, nullptr);
                nir_store_var(&b, expected, observed, 1);
                nir_pop_loop(&b, nullptr);
                nir_pop_if(&b, nullptr);
            } else {
                auto value = nir_unpack_unorm_4x8(&b, nir_ushr(&b, load(0), shift));
                value = nir_bcsel(&b, nir_ieq_imm(&b, cpp, 1),
                                  nir_vec4(&b, c(value, 0), nir_imm_float(&b, 0), nir_imm_float(&b, 0),
                                           nir_imm_float(&b, 1)),
                                  value);
                value = nir_bcsel(
                    &b, nir_ieq_imm(&b, cpp, 2),
                    nir_vec4(&b, c(value, 0), c(value, 1), nir_imm_float(&b, 0), nir_imm_float(&b, 1)),
                    value);
                value = nir_bcsel(&b, isSRGB, srgb(value, false), value);
                unsigned bgra[] = {2, 1, 0, 3};
                value = nir_bcsel(&b, isBGRA, nir_swizzle(&b, value, bgra, 4), value);
                nir_store_var(&b, result, value, 15);
            }
            nir_pop_if(&b, nullptr);
        }
        nir_pop_if(&b, nullptr);
        return writeValue ? nullptr
                          : swizzleImage(nir_load_var(&b, result), nir_ushr_imm(&b, c(info, 3), 16),
                                         binding.unsignedInteger);
    }
    nir_def *framebuffer(unsigned slot) {
        reflection.framebufferMask |= 1u << slot;
        b.shader->info.fs.uses_sample_shading = true;
        nir_def *info = nativeConstants(QGPU_FRAMEBUFFER_CONSTANT_BASE + slot * 4),
                *pixel = nir_f2i32(&b, nir_channels(&b, nir_load_frag_coord(&b), 3));
        auto valid = nir_iand(&b, nir_ige_imm(&b, c(pixel, 0), 0), nir_ige_imm(&b, c(pixel, 1), 0));
        valid = nir_iand(
            &b, valid,
            nir_iand(&b, nir_ult(&b, c(pixel, 0), c(info, 0)), nir_ult(&b, c(pixel, 1), c(info, 1))));
        auto result = nir_local_variable_create(b.impl, glsl_vec4_type(), "framebuffer");
        nir_push_if(&b, valid);
        nir_push_if(&b, nir_ult(&b, u(1), c(info, 2)));
        nir_store_var(&b, result, tex(nir_texop_txf_ms, 184 + slot, pixel, nir_load_sample_id(&b)), 15);
        nir_push_else(&b, nullptr);
        nir_store_var(&b, result, tex(nir_texop_txf, 176 + slot, pixel, u(0)), 15);
        nir_pop_if(&b, nullptr);
        nir_push_else(&b, nullptr);
        auto alpha = nir_b2f32(&b, nir_ior(&b, nir_ieq_imm(&b, c(info, 3), Shader::R8),
                                           nir_ieq_imm(&b, c(info, 3), Shader::RG8)));
        nir_store_var(&b, result,
                      nir_vec4(&b, nir_imm_float(&b, 0), nir_imm_float(&b, 0), nir_imm_float(&b, 0), alpha),
                      15);
        nir_pop_if(&b, nullptr);
        return nir_load_var(&b, result);
    }
    nir_variable *varying(LLVMValueRef md, LLVMTypeRef t, bool output) {
        if (!floating(t) || lanes(t) > 4 ||
            text(field(md, "air.arg_type_name")) != airFloatName(half(t), lanes(t)))
            fail("NIR varying type");
        unsigned index = reflection.varyingCount;
        if (index >= QGPU_MAX_VARYINGS)
            fail("NIR varying count");
        auto &v = reflection.varyings[index];
        std::string identity;
        if (!airSemantic(text(field(md, output ? "air.vertex_output" : "air.fragment_input")), identity))
            fail("NIR varying semantic");
        if (half(t))
            identity += ":half";
        semantic(identity, v.semantic);
        v.components = lanes(t);
        if (!airInterpolation(has(md, "air.flat"), has(md, "air.no_perspective"), has(md, "air.perspective"),
                              has(md, "air.center"), has(md, "air.centroid"), has(md, "air.sample"), v.mode))
            fail("NIR interpolation");
        for (unsigned n = 0; n < index; ++n)
            if (Shader::sameSemantic(v, reflection.varyings[n]))
                fail("NIR duplicate varying");
        auto var = nir_variable_create(b.shader, output ? nir_var_shader_out : nir_var_shader_in,
                                       glsl_vector_type(GLSL_TYPE_FLOAT, lanes(t)),
                                       ("_qvarying" + std::to_string(index)).c_str());
        var->data.location = VARYING_SLOT_VAR0 + index;
        var->data.interpolation = v.mode == 2                                   ? INTERP_MODE_FLAT
                                  : (v.mode == 1 || v.mode == 4 || v.mode == 6) ? INTERP_MODE_NOPERSPECTIVE
                                                                                : INTERP_MODE_SMOOTH;
        var->data.centroid = v.mode == 3 || v.mode == 4;
        var->data.sample = v.mode == 5 || v.mode == 6;
        ++reflection.varyingCount;
        return var;
    }
    void call(LLVMValueRef i);
    void instruction(LLVMValueRef i);
    void interface(LLVMValueRef metadata);
    void flow();

  public:
    QGPUProgramDescription reflection{};
    Direct(LLVMValueRef function, LLVMTargetDataRef dl, unsigned kind)
        : f(function), layout(dl), stage(kind) {
        b = nir_builder_init_simple_shader(kind == 0   ? MESA_SHADER_COMPUTE
                                           : kind == 1 ? MESA_SHADER_VERTEX
                                                       : MESA_SHADER_FRAGMENT,
                                           &options, "direct AIR");
        reflection.stage = kind;
        if (!kind)
            b.shader->info.workgroup_size_variable = true;
    }
    ~Direct() {
        if (b.shader)
            ralloc_free(b.shader);
    }
    nir_shader *run(LLVMValueRef metadata, AIRNIRMetrics &stats) {
        interface(metadata);
        std::set<LLVMValueRef> visited;
        std::function<void(LLVMValueRef)> prepare = [&](LLVMValueRef v) {
            if (!visited.insert(v).second)
                return;
            if (LLVMIsAGlobalVariable(v) || LLVMIsAAllocaInst(v)) {
                get(v);
                return;
            }
            if (LLVMIsAConstantExpr(v))
                for (unsigned n = 0; n < unsigned(LLVMGetNumOperands(v)); ++n)
                    prepare(LLVMGetOperand(v, n));
        };
        unsigned instructions = 0;
        for (auto block = LLVMGetFirstBasicBlock(f); block; block = LLVMGetNextBasicBlock(block))
            for (auto i = LLVMGetFirstInstruction(block); i; i = LLVMGetNextInstruction(i)) {
                if (++instructions > 4096)
                    fail("NIR instruction limit");
                if (LLVMIsAAllocaInst(i))
                    prepare(i);
                for (unsigned n = 0; n < unsigned(LLVMGetNumOperands(i)); ++n)
                    prepare(LLVMGetOperand(i, n));
            }
        flow();
        for (auto block = LLVMGetFirstBasicBlock(f); block; block = LLVMGetNextBasicBlock(block)) {
            ++stats.blocks;
            for (auto i = LLVMGetFirstInstruction(block); i; i = LLVMGetNextInstruction(i))
                ++stats.instructions;
        }
        for (unsigned slot = 0; slot < 8; ++slot)
            if (!(reflection.bufferMask & (1u << slot)))
                reflection.strides[slot] = 0;
        nir_shader_gather_info(b.shader, b.impl);
        auto result = b.shader;
        b.shader = nullptr;
        return result;
    }
};
const nir_shader_compiler_options Direct::options = {};
void Direct::interface(LLVMValueRef metadata) {
    auto desc = node(metadata);
    if (desc.size() != 3)
        fail("NIR entry metadata");
    auto declarations = node(desc[2]);
    auto result = LLVMGetReturnType(LLVMGlobalGetValueType(f));
    auto out = node(desc[1]);
    if (!stage && (LLVMGetTypeKind(result) != LLVMVoidTypeKind || !out.empty()))
        fail("NIR kernel return metadata");
    unsigned results = LLVMGetTypeKind(result) == LLVMVoidTypeKind     ? 0
                       : LLVMGetTypeKind(result) == LLVMStructTypeKind ? LLVMCountStructElementTypes(result)
                       : stage                                         ? 1
                                                                       : 0;
    if (results != out.size())
        fail("NIR output metadata count");
    std::set<unsigned> colorSlots;
    bool position = false, depth = false, mask = false;
    for (unsigned n = 0; n < out.size(); ++n) {
        auto md = out[n];
        if (!AIRSpecialization::enabled(md)) {
            outputs.push_back(nullptr);
            continue;
        }
        auto type =
            LLVMGetTypeKind(result) == LLVMStructTypeKind ? LLVMStructGetTypeAtIndex(result, n) : result;
        nir_variable *var = nullptr;
        if (has(md, "air.raster_order_group"))
            fail("NIR raster order group unsupported");
        if (stage == 1 && has(md, "air.vertex_output"))
            var = varying(md, type, true);
        else if (stage == 1 && has(md, "air.position")) {
            if (position || !floating(type) || half(type) || lanes(type) != 4 ||
                text(field(md, "air.arg_type_name")) != "float4")
                fail("NIR vertex position output");
            position = true;
            var = nir_variable_create(b.shader, nir_var_shader_out, glsl_vec4_type(), "gl_Position");
            var->data.location = VARYING_SLOT_POS;
        } else if (stage == 2 && has(md, "air.render_target")) {
            auto fields = node(md);
            unsigned k = 0;
            while (k < fields.size() && text(fields[k]) != "air.render_target")
                ++k;
            unsigned target = number(fields.at(k + 1));
            if (target >= 8 || number(fields.at(k + 2)) || !colorSlots.insert(target).second)
                fail("NIR color target");
            if (!floating(type) || lanes(type) > 4 ||
                text(field(md, "air.arg_type_name")) != airFloatName(half(type), lanes(type)))
                fail("NIR color output type");
            var = nir_variable_create(b.shader, nir_var_shader_out, glsl_vec4_type(),
                                      fragmentColorName(target).c_str());
            var->data.location = FRAG_RESULT_DATA0 + target;
        } else if (stage == 2 && has(md, "air.depth")) {
            auto qualifier = text(field(md, "air.depth_qualifier"));
            if (depth || LLVMGetTypeKind(type) != LLVMFloatTypeKind ||
                text(field(md, "air.arg_type_name")) != "float" ||
                (qualifier != "air.any" && qualifier != "air.less" && qualifier != "air.greater"))
                fail("NIR depth output");
            depth = true;
            var = nir_variable_create(b.shader, nir_var_shader_out, glsl_float_type(), "gl_FragDepth");
            var->data.location = FRAG_RESULT_DEPTH;
            var->data.depth_layout = qualifier == "air.less"      ? FRAG_DEPTH_LAYOUT_LESS
                                     : qualifier == "air.greater" ? FRAG_DEPTH_LAYOUT_GREATER
                                                                  : FRAG_DEPTH_LAYOUT_ANY;
        } else if (stage == 2 && has(md, "air.sample_mask")) {
            if (mask || !integer(type, 32) || text(field(md, "air.arg_type_name")) != "uint")
                fail("NIR sample mask output");
            mask = true;
            var = nir_variable_create(b.shader, nir_var_shader_out, glsl_int_type(), "gl_SampleMask");
            var->data.location = FRAG_RESULT_SAMPLE_MASK;
        } else
            fail("NIR unsupported output semantic");
        outputs.push_back(var);
    }
    if (stage == 1 && !position)
        fail("NIR missing vertex position");
    if (stage == 1) {
        auto pointSize = nir_variable_create(b.shader, nir_var_shader_out, glsl_float_type(), "gl_PointSize");
        pointSize->data.location = VARYING_SLOT_PSIZ;
        nir_store_var(&b, pointSize, nir_imm_float(&b, 1.0f), 1);
    }

    GraphicsTextureBindings textureSlots;
    for (auto md : declarations)
        if (AIRSpecialization::enabled(md) && has(md, "air.texture"))
            if (!textureSlots.add(number(field(md, "air.location_index"))))
                fail("NIR texture slots");
    textureSlots.prepare();
    reflection.textureBindings = textureSlots.encoding();
    std::set<unsigned> seen, bufferSlots, sharedSlots, samplerSlots, framebufferSlots, attributeSlots;
    std::set<std::string> systemInputs;
    for (auto md : declarations) {
        auto fields = node(md);
        unsigned at = number(fields.at(0));
        if (at >= LLVMCountParams(f) || !seen.insert(at).second)
            fail("NIR argument index");
        auto a = LLVMGetParam(f, at);
        auto t = LLVMTypeOf(a);
        NV value;
        if (!AIRSpecialization::enabled(md)) {
            value.kind = NV::Unavailable;
            values[a] = value;
            continue;
        }
        if (has(md, "air.raster_order_group"))
            fail("NIR raster order group unsupported");
        if (has(md, "air.buffer")) {
            if (LLVMGetTypeKind(t) != LLVMPointerTypeKind)
                fail("NIR buffer parameter type");
            unsigned slot = number(field(md, "air.location_index"));
            auto space = LLVMGetPointerAddressSpace(t);
            if (slot >= 8 || (space != 1 && space != 2 && space != 3) || (space == 3 && stage) ||
                !(space == 3 ? sharedSlots : bufferSlots).insert(slot).second ||
                LLVMPointerSizeForAS(layout, space) != 8 ||
                (field(md, "air.address_space") && number(field(md, "air.address_space")) != space))
                fail("NIR buffer metadata");
            bool readonly = space == 2 || has(md, "air.read");
            if ((!readonly && !has(md, "air.write") && !has(md, "air.read_write")) ||
                (stage == 1 && !readonly) || (space == 3 && readonly))
                fail("NIR buffer access");
            auto kind = text(field(md, "air.arg_type_name"));
            unsigned n = bufferComponents(kind),
                     stride = n ? (n == 3 ? 4 : n) * (kind.rfind("half", 0) == 0 ? 2 : 4) : 4;
            if ((!n && (kind.empty() || !field(md, "air.arg_type_size") ||
                        !number(field(md, "air.arg_type_size")) ||
                        number(field(md, "air.arg_type_size")) > 4096)) ||
                (n && field(md, "air.arg_type_size") && number(field(md, "air.arg_type_size")) != stride))
                fail("NIR buffer layout");
            rootIDs[a] = rootIDs.size() + 1;
            buffers[a] = {slot, stride, readonly};
            if (space == 3) {
                if (!dynamicShared)
                    dynamicShared = nir_variable_create(
                        b.shader, nir_var_mem_shared,
                        glsl_array_type(glsl_uint_type(), QGPU_MAX_THREADGROUP_BYTES / 4, 0), "_qdynamic");
                buffers[a].storage = dynamicShared;
                buffers[a].bytes = QGPU_MAX_THREADGROUP_BYTES;
                buffers[a].dynamicSlot = slot;
            } else
                reflection.strides[slot] = stride;
            value.kind = NV::Pointer;
            value.root = a;
            value.def = addr(0);
        } else if (has(md, "air.texture")) {
            if (LLVMGetTypeKind(t) != LLVMPointerTypeKind || LLVMGetPointerAddressSpace(t) != 1)
                fail("NIR texture parameter type");
            AIRTextureBinding binding;
            binding.slot = textureSlots.physical(number(field(md, "air.location_index")));
            if (!airTextureMetadata(text(field(md, "air.arg_type_name")), binding) ||
                (stage && (binding.unsignedInteger || (binding.access != 1 && binding.access != 4))))
                fail("NIR frontend texture metadata");
            textures[a] = binding;
            if (binding.unsignedInteger)
                reflection.imageUintMask |= 1u << binding.slot;
            value.kind = NV::Texture;
            value.root = a;
        } else if (has(md, "air.sampler")) {
            if (LLVMGetTypeKind(t) != LLVMPointerTypeKind || LLVMGetPointerAddressSpace(t) != 2)
                fail("NIR sampler parameter type");
            unsigned slot = number(field(md, "air.location_index"));
            if (slot >= 8 || !samplerSlots.insert(slot).second)
                fail("NIR sampler slot");
            samplers[a] = slot;
            value.kind = NV::Sampler;
            value.root = a;
        } else if (stage == 1 && has(md, "air.vertex_input")) {
            unsigned slot = number(field(md, "air.location_index"));
            auto kind = text(field(md, "air.arg_type_name"));
            bool sign = kind.rfind("int", 0) == 0;
            if (slot >= QGPU_MAX_ATTRIBUTES || !attributeSlots.insert(slot).second || lanes(t) > 4 ||
                (!floating(t) && !integer(scalar(t), 32)) ||
                kind != (floating(t) ? airFloatName(half(t), lanes(t))
                                     : (sign ? "int" : "uint") +
                                           (lanes(t) == 1 ? std::string() : std::to_string(lanes(t)))))
                fail("NIR vertex attribute");
            auto var = nir_variable_create(b.shader, nir_var_shader_in,
                                           glsl_vector_type(floating(t) ? GLSL_TYPE_FLOAT
                                                            : sign      ? GLSL_TYPE_INT
                                                                        : GLSL_TYPE_UINT,
                                                            lanes(t)),
                                           ("_qattrib" + std::to_string(slot)).c_str());
            var->data.location = VERT_ATTRIB_GENERIC0 + slot;
            value.def = nir_load_var(&b, var);
            if (half(t))
                value.def = nir_f2f16(&b, value.def);
        } else if (stage == 1 && (has(md, "air.vertex_id") || has(md, "air.instance_id"))) {
            bool instance = has(md, "air.instance_id");
            if (!systemInputs.insert(instance ? "instance" : "vertex").second ||
                (!integer(t, 32) && (instance || !integer(t, 16))))
                fail("NIR vertex system input");
            value.def = instance ? nir_iadd(&b, nir_load_instance_id(&b), nir_load_base_instance(&b))
                                 : nir_load_vertex_id(&b);
            if (integer(t, 16))
                value.def = nir_u2u16(&b, value.def);
            value.index = instance ? 2 : 1;
        } else if (stage == 2 && has(md, "air.fragment_input")) {
            value.def = nir_f2fN(&b, nir_load_var(&b, varying(md, t, false)), bits(t));
        } else if (stage == 2 && (has(md, "air.sample_id") || has(md, "air.sample_mask"))) {
            bool id = has(md, "air.sample_id");
            if (!integer(t, 32) || text(field(md, "air.arg_type_name")) != "uint" ||
                has(md, "air.post_depth_coverage") || (id && has(md, "air.sample_mask")) ||
                !systemInputs.insert(id ? "sample_id" : "sample_mask").second)
                fail("NIR fragment sample input");
            value.def = id ? nir_load_sample_id(&b) : nir_load_sample_mask_in(&b);
        } else if (stage == 2 && has(md, "air.position")) {
            if (!floating(t) || half(t) || lanes(t) != 4 || text(field(md, "air.arg_type_name")) != "float4")
                fail("NIR fragment position type");
            value.def = nir_load_frag_coord(&b);
        } else if (stage == 2 && has(md, "air.render_target")) {
            unsigned k = 0;
            while (k < fields.size() && text(fields[k]) != "air.render_target")
                ++k;
            unsigned slot = number(fields.at(k + 1));
            if (slot >= 8 || !framebufferSlots.insert(slot).second || !floating(t) || lanes(t) > 4 ||
                text(field(md, "air.arg_type_name")) != airFloatName(half(t), lanes(t)))
                fail("NIR frontend framebuffer input type/slot");
            auto raw = framebuffer(slot);
            if (lanes(t) != 4)
                raw = nir_channels(&b, raw, (1u << lanes(t)) - 1);
            value.def = nir_f2fN(&b, raw, bits(t));
        } else if (!stage && !computeInputSemantic([&](const char *key) {
                                  return has(md, std::string("air.") + key);
                              }).empty()) {
            auto input =
                computeInputSemantic([&](const char *key) { return has(md, std::string("air.") + key); });
            if (!systemInputs.insert(input).second || !integer(scalar(t), 32) || lanes(t) > 3 ||
                (input == "thread_index_in_threadgroup" && lanes(t) != 1))
                fail("NIR compute system input");
            nir_def *v = nullptr;
            auto dispatch = [&](unsigned index) {
                reflection.flags |= QGPU_DISPATCH_GEOMETRY;
                return nativeConstants(QGPU_DISPATCH_CONSTANT_BASE + index * 4);
            };
            if (input == "thread_position_in_grid")
                v = nir_iadd(&b, nir_channels(&b, dispatch(0), 7), nir_load_global_invocation_id(&b, 32));
            else if (input == "threadgroup_position_in_grid")
                v = nir_iadd(&b, nir_channels(&b, dispatch(1), 7), nir_load_workgroup_id(&b));
            else if (input == "threads_per_grid")
                v = nir_channels(&b, dispatch(2), 7);
            else if (input == "threadgroups_per_grid")
                v = nir_channels(&b, dispatch(3), 7);
            else if (input == "dispatch_threads_per_threadgroup")
                v = nir_vec3(&b, c(dispatch(0), 3), c(dispatch(1), 3), c(dispatch(2), 3));
            else if (input == "threads_per_threadgroup")
                v = nir_load_workgroup_size(&b);
            else if (input == "thread_position_in_threadgroup")
                v = nir_load_local_invocation_id(&b);
            else if (input == "thread_index_in_threadgroup")
                v = nir_load_local_invocation_index(&b);
            else
                fail("NIR compute semantic");
            value.def = lanes(t) == 1 ? c(v, 0) : nir_channels(&b, v, (1u << lanes(t)) - 1);
        } else
            fail("NIR frontend unsupported argument semantic");
        values[a] = value;
    }
    if (seen.size() != LLVMCountParams(f))
        fail("NIR incomplete arguments");
}
void Direct::call(LLVMValueRef i) {
    auto function = LLVMGetCalledValue(i);
    auto n = name(function);
    unsigned count = LLVMGetNumArgOperands(i);
    auto t = LLVMTypeOf(i);
    if (!LLVMIsAFunction(function) || LLVMCountBasicBlocks(function))
        fail("NIR indirect/uninlined call");
    if (n.rfind("llvm.lifetime.", 0) == 0 || n.rfind("llvm.dbg.", 0) == 0)
        return;
    for (unsigned width = 2; width <= 4; ++width)
        for (bool all : {false, true})
            if (n == std::string(all ? "air.all.v" : "air.any.v") + std::to_string(width) + "i1") {
                auto input = count ? LLVMTypeOf(LLVMGetOperand(i, 0)) : t;
                if (count != 1 || !integer(t, 1) || lanes(input) != width || !integer(scalar(input), 1))
                    fail("NIR boolean reduction signature");
                auto v = arg(i, 0), result = c(v, 0);
                for (unsigned lane = 1; lane < width; ++lane)
                    result = all ? nir_iand(&b, result, c(v, lane)) : nir_ior(&b, result, c(v, lane));
                set(i, result);
                return;
            }
    if (n == "air.discard_fragment") {
        if (stage != 2 || count || LLVMGetTypeKind(t) != LLVMVoidTypeKind)
            fail("NIR fragment discard signature");
        nir_demote(&b);
        return;
    }
    if (n == "air.wg.barrier") {
        if (stage || count != 2 || !integer(LLVMTypeOf(LLVMGetOperand(i, 0)), 32) ||
            !integer(LLVMTypeOf(LLVMGetOperand(i, 1)), 32) || number(LLVMGetOperand(i, 0)) > 7 ||
            number(LLVMGetOperand(i, 1)) != 1)
            fail("NIR workgroup barrier signature");
        unsigned flags = number(LLVMGetOperand(i, 0));
        _nir_barrier_indices memory{};
        memory.memory_scope = SCOPE_WORKGROUP;
        memory.memory_semantics = NIR_MEMORY_ACQ_REL;
        memory.memory_modes =
            nir_variable_mode((flags & 5 ? nir_var_mem_ssbo : 0) | (flags & 2 ? nir_var_mem_shared : 0));
        if (memory.memory_modes)
            _nir_build_barrier(&b, memory);
        _nir_barrier_indices control{};
        control.execution_scope = SCOPE_WORKGROUP;
        control.memory_scope = SCOPE_WORKGROUP;
        control.memory_modes = nir_var_mem_shared;
        control.memory_semantics = NIR_MEMORY_ACQ_REL;
        _nir_build_barrier(&b, control);
        return;
    }
    AIRAtomic operation;
    if (airAtomic(n, operation)) {
        unsigned wanted = operation.op == "cas" ? 7 : operation.op == "load" ? 4 : 5;
        unsigned order = operation.op == "cas" ? 3 : operation.op == "load" ? 1 : 2;
        auto immediate = [&](unsigned at, unsigned width, uint64_t v) {
            auto value = LLVMGetOperand(i, at);
            return integer(LLVMTypeOf(value), width) && LLVMIsAConstantInt(value) && number(value) == v;
        };
        if (stage == 1 || count != wanted || !immediate(order, 32, 0) ||
            (operation.op == "cas" && !immediate(order + 1, 32, 0)) ||
            !immediate(count - 2, 32, operation.local ? 1 : 2) ||
            !integer(LLVMTypeOf(LLVMGetOperand(i, count - 1)), 1) ||
            !LLVMIsAConstantInt(LLVMGetOperand(i, count - 1)) ||
            LLVMGetTypeKind(LLVMTypeOf(LLVMGetOperand(i, 0))) != LLVMPointerTypeKind ||
            LLVMGetPointerAddressSpace(LLVMTypeOf(LLVMGetOperand(i, 0))) != (operation.local ? 3u : 1u))
            fail("NIR atomic intrinsic scope/order");
        if (operation.op == "store" ? LLVMGetTypeKind(t) != LLVMVoidTypeKind : !integer(t, 32))
            fail("NIR atomic result type");
        if (operation.op != "load" &&
            !integer(LLVMTypeOf(LLVMGetOperand(i, operation.op == "cas" ? 2 : 1)), 32))
            fail("NIR atomic operand type");
        auto pointer = get(LLVMGetOperand(i, 0));
        if (operation.op == "cas") {
            auto expected = get(LLVMGetOperand(i, 1));
            auto old = atomic(pointer, "cas", arg(i, 2), privateAccess(expected, 32, 1));
            privateAccess(expected, 32, 1, old);
            set(i, old);
        } else {
            auto op = operation.op == "load" ? "or" : operation.op == "store" ? "xchg" : operation.op;
            auto old =
                atomic(pointer, op, operation.op == "load" ? u(0) : arg(i, 1), nullptr, operation.sign);
            if (operation.op != "store")
                set(i, old);
        }
        return;
    }
    if (n == "air.get_read_sampler") {
        if (count || LLVMGetTypeKind(t) != LLVMPointerTypeKind || LLVMGetPointerAddressSpace(t) != 2)
            fail("NIR read sampler signature");
        NV v;
        v.kind = NV::ReadSampler;
        values[i] = v;
        return;
    }
    if (n == "air.pack.unorm4x8.v4f16" || n == "air.pack.unorm4x8.v4f32") {
        if (count != 1 || !integer(t, 32) || !floating(LLVMTypeOf(LLVMGetOperand(i, 0))) ||
            lanes(LLVMTypeOf(LLVMGetOperand(i, 0))) != 4 ||
            half(LLVMTypeOf(LLVMGetOperand(i, 0))) != (n == "air.pack.unorm4x8.v4f16"))
            fail("NIR unorm pack signature");
        set(i, nir_pack_unorm_4x8(&b, nir_f2f32(&b, arg(i, 0))));
        return;
    }
    if (n.rfind("air.convert.", 0) == 0) {
        std::vector<std::string> parts;
        size_t begin = 0;
        for (size_t end; (end = n.find('.', begin)) != std::string::npos; begin = end + 1)
            parts.push_back(n.substr(begin, end - begin));
        parts.push_back(n.substr(begin));
        if (parts.size() != 6 || count != 1 || (parts[2] != "f" && parts[2] != "s" && parts[2] != "u") ||
            (parts[4] != "f" && parts[4] != "s" && parts[4] != "u"))
            fail("NIR conversion signature");
        auto source = LLVMGetOperand(i, 0);
        auto sourceType = LLVMTypeOf(source);
        if (lanes(t) != lanes(sourceType) || lanes(t) > 4)
            fail("NIR conversion shape");
        auto spelling = [&](LLVMTypeRef type, bool fp) {
            if (fp ? !floating(type)
                   : !(integer(scalar(type), 8) || integer(scalar(type), 16) || integer(scalar(type), 32) ||
                       integer(scalar(type), 64) || integer(scalar(type), 1)))
                fail("NIR conversion type");
            return fp ? airFloatSuffix(half(type), lanes(type))
                      : (lanes(type) == 1 ? std::string() : "v" + std::to_string(lanes(type))) + "i" +
                            std::to_string(bits(type));
        };
        if (parts[3] != spelling(t, parts[2] == "f") || parts[5] != spelling(sourceType, parts[4] == "f"))
            fail("NIR conversion mangling");
        set(i,
            floatConvert(arg(i, 0), LLVMTypeOf(LLVMGetOperand(i, 0)), t, parts[4] == "s", parts[2] == "s"));
        return;
    }
    if (n == "air.fast_sincos.f32" || n == "air.sincos.f32") {
        if (count != 2 || !floating(t) || half(t) || lanes(t) != 1 || LLVMTypeOf(LLVMGetOperand(i, 0)) != t)
            fail("NIR sincos");
        auto output = get(LLVMGetOperand(i, 1));

        auto x = arg(i, 0);
        privateAccess(output, 32, 1, nir_fcos(&b, x));
        set(i, nir_fsin(&b, x));
        return;
    }
    AIRTextureCall tc;
    if (airTextureOperation(n, tc)) {
        if (!airTextureLayout(tc, count))
            fail("NIR texture signature");
        auto operand = [&](unsigned at) { return LLVMGetOperand(i, at); };
        auto isInt = [&](unsigned at, unsigned width) { return integer(LLVMTypeOf(operand(at)), width); };
        auto immediate = [&](unsigned at, unsigned width, uint64_t v) {
            return isInt(at, width) && LLVMIsAConstantInt(operand(at)) && number(operand(at)) == v;
        };
        auto zero = [&](unsigned at) {
            return at == UINT32_MAX || (LLVMIsAConstant(operand(at)) && LLVMIsNull(operand(at)));
        };
        auto handle = get(operand(0));
        if (handle.kind != NV::Texture || !textures.count(handle.root))
            fail("NIR texture binding");
        auto binding = textures.at(handle.root);
        if (binding.shape != tc.shape)
            fail("NIR texture shape mismatch");
        bool read = tc.op == AIRTextureCall::Read, write = tc.op == AIRTextureCall::Write,
             sampleOp = tc.op >= AIRTextureCall::Sample;
        auto texelMatches = [&](LLVMTypeRef type) {
            return lanes(type) == 4 &&
                   (tc.unsignedInteger ? integer(scalar(type), 32) : floating(type) && half(type) == tc.half);
        };
        if ((read || write || sampleOp) &&
            (binding.half != tc.half || binding.unsignedInteger != tc.unsignedInteger))
            fail("NIR texture type mismatch");
        if (!read && !write && !sampleOp) {
            if (!integer(t, 32) || (tc.lod != UINT32_MAX && !isInt(tc.lod, 32)))
                fail("NIR texture query type");
            auto info = imageInfo(binding), level = tc.lod == UINT32_MAX ? u(0) : arg(i, tc.lod);
            nir_def *result;
            auto array = !stage && (binding.shape & 1) ? imageInfo(binding, true) : nullptr;
            if (tc.op == AIRTextureCall::Width || tc.op == AIRTextureCall::Height) {
                result = c(imageSize(info, level), tc.op == AIRTextureCall::Width ? 0 : 1);
                if (array)
                    result = nir_bcsel(&b, nir_ine_imm(&b, c(array, 0), 0), result, u(0));
            } else if (tc.op == AIRTextureCall::MipLevels) {
                result = nir_iand_imm(&b, nir_ushr_imm(&b, c(info, 3), 8), 255);
                if (array)
                    result = nir_bcsel(&b, nir_ine_imm(&b, c(array, 0), 0), result, u(0));
            } else {
                result = stage ? c(info, 2) : c(imageInfo(binding, true), 0);
                if (!stage && (binding.shape & 2))
                    result = nir_udiv_imm(&b, result, 6);
            }
            set(i, result);
            return;
        }
        if (tc.face != UINT32_MAX && !isInt(tc.face, 32))
            fail("NIR texture face type");
        if (tc.layer != UINT32_MAX && !isInt(tc.layer, 32))
            fail("NIR texture layer type");
        nir_def *offset = nullptr;
        if (tc.offsetFlag != UINT32_MAX &&
            (!isInt(tc.offsetFlag, 1) || !LLVMIsAConstantInt(operand(tc.offsetFlag))))
            fail("NIR texture offset flag");
        if (tc.offset != UINT32_MAX) {
            auto type = LLVMTypeOf(operand(tc.offset));
            if (lanes(type) != 2 || !integer(scalar(type), 32))
                fail("NIR texture offset type");
            if (!zero(tc.offset)) {
                if (!sampleOp || !LLVMIsAConstant(operand(tc.offset)) || tc.offsetFlag == UINT32_MAX ||
                    !immediate(tc.offsetFlag, 1, 1))
                    fail("NIR texture offset mode");
                for (unsigned k = 0; k < 2; ++k) {
                    auto component = LLVMConstExtractElement(
                        operand(tc.offset),
                        LLVMConstInt(LLVMInt32TypeInContext(LLVMGetTypeContext(type)), k, false));
                    if (!LLVMIsAConstantInt(component) || LLVMConstIntGetSExtValue(component) < -8 ||
                        LLVMConstIntGetSExtValue(component) > 7)
                        fail("NIR texture offset range");
                }
                offset = arg(i, tc.offset);
            }
        }
        if (!immediate(tc.access, 32, binding.access == 4 ? 0 : binding.access))
            fail("NIR texture access mismatch");
        auto result = [&](nir_def *value) {
            if (LLVMGetTypeKind(t) == LLVMStructTypeKind) {
                if (LLVMCountStructElementTypes(t) != 2 || !texelMatches(LLVMStructGetTypeAtIndex(t, 0)) ||
                    !integer(LLVMStructGetTypeAtIndex(t, 1), 8))
                    fail("NIR texture result structure");
                values.at(i).members.at(1).kind = NV::Unavailable;
                NV out;
                out.kind = NV::Aggregate;
                NV unavailable;
                unavailable.kind = NV::Unavailable;
                out.members = {data(tc.unsignedInteger ? value : nir_f2fN(&b, value, tc.half ? 16 : 32)),
                               unavailable};
                set(i, out);
            } else {
                if (!texelMatches(t))
                    fail("NIR texture result type");
                set(i, tc.unsignedInteger ? value : nir_f2fN(&b, value, tc.half ? 16 : 32));
            }
        };
        if (read || write) {
            auto coordinate = LLVMTypeOf(operand(tc.coord));
            if (lanes(coordinate) != 2 || !integer(scalar(coordinate), 32) || !isInt(tc.lod, 32) ||
                (write ? (stage || !(binding.access & 2)) : (binding.access != 4 && !(binding.access & 1))))
                fail("NIR texture read/write stage/access");
            if (tc.currentRead && get(operand(1)).kind != NV::ReadSampler)
                fail("NIR texture read sampler");
            if (write &&
                (LLVMGetTypeKind(t) != LLVMVoidTypeKind || !texelMatches(LLVMTypeOf(operand(tc.value)))))
                fail("NIR texture write value");
            auto value = write ? arg(i, tc.value) : nullptr;
            if (write && !tc.unsignedInteger)
                value = nir_f2f32(&b, value);
            auto r = imageReadWrite(binding, arg(i, tc.coord), arg(i, tc.lod),
                                    tc.layer == UINT32_MAX ? nullptr : arg(i, tc.layer),
                                    tc.face == UINT32_MAX ? nullptr : arg(i, tc.face), value);
            if (!write)
                result(r);
            return;
        }
        bool gradient = tc.op == AIRTextureCall::Gradient, cube = binding.shape & 2;
        auto sampler = get(operand(1));
        auto coordType = LLVMTypeOf(operand(tc.coord));
        if (binding.access != 4 || sampler.kind != NV::Sampler || !samplers.count(sampler.root) ||
            lanes(coordType) != (cube ? 3u : 2u) || LLVMGetTypeKind(scalar(coordType)) != LLVMFloatTypeKind ||
            LLVMGetTypeKind(LLVMTypeOf(operand(tc.minLod))) != LLVMFloatTypeKind || !zero(tc.minLod))
            fail("NIR sample operands");
        unsigned slot = samplers.at(sampler.root);
        const QGPUConstantSampler *literal = slot >= 8 ? &literals.at(slot - 8) : nullptr;
        if (cube && literal && !literal->normalized)
            fail("NIR cube sampler coordinates");
        bool level = false;
        nir_def *lod = nir_imm_float(&b, 0), *dx = nullptr, *dy = nullptr;
        if (gradient) {
            if (LLVMTypeOf(operand(tc.dx)) != coordType || LLVMTypeOf(operand(tc.dy)) != coordType)
                fail("NIR sample gradient types");
            dx = arg(i, tc.dx);
            dy = arg(i, tc.dy);
        } else {
            if (!isInt(tc.levelFlag, 1) || !LLVMIsAConstantInt(operand(tc.levelFlag)) ||
                LLVMGetTypeKind(LLVMTypeOf(operand(tc.lod))) != LLVMFloatTypeKind)
                fail("NIR sample LOD mode");
            level = number(operand(tc.levelFlag));
            if (stage != 2 && !level && !zero(tc.lod))
                fail("NIR nonfragment implicit bias");
            lod = arg(i, tc.lod);
        }
        if (literal) {
            reflection.constantSamplerMask |= 1u << (slot - 8);
            reflection.constantSamplers[slot - 8] = *literal;
        }
        result(sampleGeneral(binding, slot, arg(i, tc.coord), lod, level,
                             tc.layer == UINT32_MAX ? nullptr : arg(i, tc.layer), dx, dy, offset, literal));
        return;
    }
    if (n.rfind("air.", 0) != 0 && n.rfind("llvm.", 0) != 0)
        fail("NIR uninlined function " + n);
    std::string extra;
    unsigned extraCount = airExtraMath(n, airFloatSuffix(half(t), lanes(t)), extra);
    if (extraCount) {
        if (count != extraCount || !floating(t) || lanes(t) > 4)
            fail("NIR extra math signature");
        for (unsigned j = 0; j < count; ++j)
            if (LLVMTypeOf(LLVMGetOperand(i, j)) != t)
                fail("NIR extra math operand");
        auto x = arg(i, 0);
        nir_def *result = nullptr;
        if (extra == "fmin3" || extra == "fmax3") {
            auto extrema = [&](nir_def *a, nir_def *d) {
                auto value = extra == "fmin3" ? nir_fmin(&b, a, d) : nir_fmax(&b, a, d);
                return nir_bcsel(&b, nir_fneu(&b, a, a), d, nir_bcsel(&b, nir_fneu(&b, d, d), a, value));
            };
            result = extrema(x, extrema(arg(i, 1), arg(i, 2)));
        } else {
            x = nir_f2f32(&b, x);
            if (extra == "exp")
                result = nir_fexp2(&b, nir_fmul_imm(&b, x, 1.4426950408889634));
            else if (extra == "log")
                result = nir_fmul_imm(&b, nir_flog2(&b, x), 0.6931471805599453);
            else if (extra == "powr")
                result = nir_fpow(&b, x, nir_f2f32(&b, arg(i, 1)));
            else if (extra == "atan2")
                result = nir_atan2(&b, x, nir_f2f32(&b, arg(i, 1)));
            result = nir_f2fN(&b, result, bits(t));
        }
        set(i, result);
        return;
    }
    if (!count || !floating(t) || lanes(t) > 4)
        fail("NIR math return signature");
    auto inputType = LLVMTypeOf(LLVMGetOperand(i, 0));
    if (!floating(inputType) || half(inputType) != half(t) || lanes(inputType) > 4)
        fail("NIR math operand signature");
    auto matches = [&](const std::string &op) {
        auto suffix = airFloatSuffix(half(inputType), lanes(inputType));
        bool match = n == "air." + op + "." + suffix || n == "air.fast_" + op + "." + suffix ||
                     n == "llvm." + op + "." + suffix;
        if (match) {
            unsigned expected = (op == "clamp" || op == "mix" || op == "fma")   ? 3
                                : (op == "fmin" || op == "fmax" || op == "dot") ? 2
                                                                                : 1;
            if (count != expected || (op == "dot" ? (lanes(t) != 1) : (t != inputType)))
                fail("NIR math signature");
            for (unsigned j = 1; j < count; ++j)
                if (LLVMTypeOf(LLVMGetOperand(i, j)) != inputType)
                    fail("NIR math operand shape");
        }
        return match;
    };
    nir_def *a = count ? arg(i, 0) : nullptr, *d = count > 1 ? arg(i, 1) : nullptr,
            *e = count > 2 ? arg(i, 2) : nullptr, *r = nullptr;
    if (matches("saturate"))
        r = nir_fsat(&b, a);
    else if (matches("clamp"))
        r = nir_fmin(&b, nir_fmax(&b, a, d), e);
    else if (matches("fmin"))
        r = nir_fmin(&b, a, d);
    else if (matches("fmax"))
        r = nir_fmax(&b, a, d);
    else if (matches("dot"))
        r = nir_fdot(&b, a, d);
    else if (matches("mix"))
        r = nir_flrp(&b, a, d, e);
    else if (matches("fma"))
        r = nir_ffma(&b, a, d, e);
    else if (matches("sqrt"))
        r = nir_fsqrt(&b, a);
    else if (matches("rsqrt"))
        r = nir_frsq(&b, a);
    else if (matches("fract"))
        r = nir_ffract(&b, a);
    else if (matches("floor"))
        r = nir_ffloor(&b, a);
    else if (matches("ceil"))
        r = nir_fceil(&b, a);
    else if (matches("trunc"))
        r = nir_ftrunc(&b, a);
    else if (matches("fabs"))
        r = nir_fabs(&b, a);
    else if (matches("sin"))
        r = nir_fsin(&b, a);
    else if (matches("cos"))
        r = nir_fcos(&b, a);
    else if (matches("exp2"))
        r = nir_fexp2(&b, a);
    else if (matches("log2"))
        r = nir_flog2(&b, a);
    else
        fail("NIR frontend unsupported intrinsic " + n);
    set(i, r);
}
void Direct::instruction(LLVMValueRef i) {
    auto op = LLVMGetInstructionOpcode(i);
    auto t = LLVMTypeOf(i);
    if (op == LLVMPHI || branch(i) || op == LLVMSwitch)
        return;
    if (op == LLVMAtomicRMW) {
        if (!integer(t, 32) || LLVMGetOrdering(i) != LLVMAtomicOrderingMonotonic)
            fail("NIR atomicrmw requires relaxed int32");
        const std::map<LLVMAtomicRMWBinOp, std::pair<std::string, bool>> ops = {
            {LLVMAtomicRMWBinOpXchg, {"xchg", false}}, {LLVMAtomicRMWBinOpAdd, {"add", false}},
            {LLVMAtomicRMWBinOpSub, {"sub", false}},   {LLVMAtomicRMWBinOpAnd, {"and", false}},
            {LLVMAtomicRMWBinOpOr, {"or", false}},     {LLVMAtomicRMWBinOpXor, {"xor", false}},
            {LLVMAtomicRMWBinOpMin, {"min", true}},    {LLVMAtomicRMWBinOpMax, {"max", true}},
            {LLVMAtomicRMWBinOpUMin, {"min", false}},  {LLVMAtomicRMWBinOpUMax, {"max", false}}};
        auto operation = ops.find(LLVMGetAtomicRMWBinOp(i));
        if (operation == ops.end())
            fail("NIR atomicrmw operation");
        set(i, atomic(get(LLVMGetOperand(i, 0)), operation->second.first, arg(i, 1), nullptr,
                      operation->second.second));
        return;
    }
    if (op == LLVMAtomicCmpXchg) {
        if (!integer(LLVMTypeOf(LLVMGetOperand(i, 1)), 32) ||
            LLVMGetCmpXchgSuccessOrdering(i) != LLVMAtomicOrderingMonotonic ||
            LLVMGetCmpXchgFailureOrdering(i) != LLVMAtomicOrderingMonotonic)
            fail("NIR cmpxchg requires relaxed int32");
        auto compare = arg(i, 1), old = atomic(get(LLVMGetOperand(i, 0)), "cas", arg(i, 2), compare);
        NV result;
        result.kind = NV::Aggregate;
        result.members = {data(old), data(nir_ieq(&b, old, compare))};
        set(i, result);
        return;
    }
    if (op == LLVMGetElementPtr) {
        gep(i);
        return;
    }
    if (op == LLVMAlloca) {
        get(i);
        return;
    }
    if (op == LLVMLoad || op == LLVMStore) {
        memory(i, op == LLVMStore);
        return;
    }
    if (op == LLVMCall) {
        call(i);
        return;
    }
    if (op == LLVMRet) {
        if (!stage || !LLVMGetNumOperands(i))
            return;
        auto value = get(LLVMGetOperand(i, 0));
        if (value.kind != NV::Aggregate) {
            NV aggregate;
            aggregate.kind = NV::Aggregate;
            aggregate.members.push_back(value);
            value = aggregate;
        }
        if (value.members.size() != outputs.size())
            fail("NIR return shape");
        for (unsigned n = 0; n < outputs.size(); ++n)
            if (outputs[n]) {
                auto d = read(value.members[n]);
                auto var = outputs[n];
                if (glsl_get_base_type(var->type) == GLSL_TYPE_FLOAT)
                    d = nir_f2f32(&b, d);
                unsigned width = glsl_get_vector_elements(var->type);
                if (d->num_components != width) {
                    if (stage != 2 || var->data.location < FRAG_RESULT_DATA0 || width != 4 ||
                        d->num_components > 4)
                        fail("NIR output width");
                    std::vector<nir_def *> components;
                    for (unsigned k = 0; k < 4; ++k)
                        components.push_back(k < d->num_components ? c(d, k)
                                                                   : nir_imm_float(&b, k == 3 ? 1 : 0));
                    d = vec(components);
                }
                nir_store_var(&b, var, d, (1u << width) - 1);
            }
        return;
    }
    if (op == LLVMExtractValue || op == LLVMInsertValue) {
        NV value = get(LLVMGetOperand(i, 0)), *part = &value;
        auto indices = LLVMGetIndices(i);
        for (unsigned n = 0; n < LLVMGetNumIndices(i); ++n) {
            if (part->kind != NV::Aggregate)
                fail("NIR aggregate access");
            part = &part->members.at(indices[n]);
        }
        if (op == LLVMExtractValue)
            set(i, *part);
        else {
            *part = get(LLVMGetOperand(i, 1));
            set(i, value);
        }
        return;
    }
    if (op == LLVMExtractElement) {
        set(i, nir_vector_extract(&b, arg(i, 0), nir_u2u32(&b, arg(i, 1))));
        return;
    }
    if (op == LLVMInsertElement || op == LLVMShuffleVector) {
        std::vector<nir_def *> parts;
        unsigned size = lanes(LLVMTypeOf(LLVMGetOperand(i, 0)));
        for (unsigned n = 0; n < lanes(t); ++n) {
            if (op == LLVMInsertElement)
                parts.push_back(nir_bcsel(&b, nir_ieq_imm(&b, arg(i, 2), n), arg(i, 1), c(arg(i, 0), n)));
            else {
                int mask = LLVMGetMaskValue(i, n);
                parts.push_back(mask < 0 ? nir_imm_zero(&b, 1, bits(t))
                                         : c(arg(i, unsigned(mask) >= size ? 1 : 0), unsigned(mask) % size));
            }
        }
        set(i, vec(parts));
        return;
    }
    if (op == LLVMSelect) {
        if (LLVMGetTypeKind(t) == LLVMPointerTypeKind) {
            if (!values.count(i))
                fail("NIR pointer select requires bounded storage provenance");
            auto value = values.at(i);
            value.cell = nullptr;
            value.def = nir_bcsel(&b, arg(i, 0), pointerPayload(get(LLVMGetOperand(i, 1))),
                                  pointerPayload(get(LLVMGetOperand(i, 2))));
            set(i, value);
            return;
        }
        set(i, nir_bcsel(&b, arg(i, 0), arg(i, 1), arg(i, 2)));
        return;
    }
    if (op == LLVMFCmp) {
        set(i, fcmp(LLVMGetFCmpPredicate(i), arg(i, 0), arg(i, 1)));
        return;
    }
    if (op == LLVMICmp) {
        if (LLVMGetTypeKind(LLVMTypeOf(LLVMGetOperand(i, 0))) == LLVMPointerTypeKind) {
            auto predicate = LLVMGetICmpPredicate(i);
            if (predicate != LLVMIntEQ && predicate != LLVMIntNE)
                fail("NIR pointer ordering unsupported");
            auto a = pointerPayload(get(LLVMGetOperand(i, 0))), d = pointerPayload(get(LLVMGetOperand(i, 1)));
            auto equal = nir_ball_iequal(&b, a, d);
            set(i, predicate == LLVMIntEQ ? equal : nir_inot(&b, equal));
            return;
        }
        nir_def *a = arg(i, 0), *d = arg(i, 1), *r = nullptr;
        switch (LLVMGetICmpPredicate(i)) {
        case LLVMIntEQ:
            r = nir_ieq(&b, a, d);
            break;
        case LLVMIntNE:
            r = nir_ine(&b, a, d);
            break;
        case LLVMIntULT:
            r = nir_ult(&b, a, d);
            break;
        case LLVMIntULE:
            r = nir_uge(&b, d, a);
            break;
        case LLVMIntUGT:
            r = nir_ult(&b, d, a);
            break;
        case LLVMIntUGE:
            r = nir_uge(&b, a, d);
            break;
        case LLVMIntSLT:
            r = nir_ilt(&b, a, d);
            break;
        case LLVMIntSLE:
            r = nir_ige(&b, d, a);
            break;
        case LLVMIntSGT:
            r = nir_ilt(&b, d, a);
            break;
        case LLVMIntSGE:
            r = nir_ige(&b, a, d);
            break;
        default:
            fail("NIR integer predicate");
        }
        set(i, r);
        return;
    }
    if (op == LLVMZExt || op == LLVMSExt || op == LLVMTrunc || op == LLVMUIToFP || op == LLVMSIToFP ||
        op == LLVMFPToUI || op == LLVMFPToSI || op == LLVMFPTrunc || op == LLVMFPExt || op == LLVMBitCast) {
        auto from = LLVMTypeOf(LLVMGetOperand(i, 0));
        auto v = get(LLVMGetOperand(i, 0));
        if (op == LLVMBitCast) {
            if (LLVMGetTypeKind(t) == LLVMPointerTypeKind)
                set(i, v);
            else {
                if (lanes(from) * bits(from) != lanes(t) * bits(t))
                    fail("NIR bitcast width");
                set(i, nir_bitcast_vector(&b, read(v), bits(t)));
            }
            return;
        }
        if (lanes(from) != lanes(t))
            fail("NIR reshaping cast unsupported");
        if (op == LLVMSExt && integer(scalar(from), 1)) {
            set(i, nir_ineg(&b, nir_b2iN(&b, read(v), bits(t))));
            return;
        }
        set(i, floatConvert(read(v), from, t, op == LLVMSExt || op == LLVMSIToFP, op == LLVMFPToSI));
        return;
    }
    if (integer(scalar(t), 1) && op != LLVMAnd && op != LLVMOr && op != LLVMXor)
        fail("NIR boolean arithmetic supports only and/or/xor");
    auto a = arg(i, 0);
    nir_def *d = LLVMGetNumOperands(i) > 1 ? arg(i, 1) : nullptr, *r = nullptr;
    switch (op) {
    case LLVMAdd:
        r = nir_iadd(&b, a, d);
        break;
    case LLVMSub:
        r = nir_isub(&b, a, d);
        break;
    case LLVMMul:
        r = nir_imul(&b, a, d);
        break;
    case LLVMUDiv:
        r = nir_udiv(&b, a, d);
        break;
    case LLVMSDiv:
        r = nir_idiv(&b, a, d);
        break;
    case LLVMURem:
        r = nir_umod(&b, a, d);
        break;
    case LLVMSRem:
        r = nir_irem(&b, a, d);
        break;
    case LLVMFAdd:
        r = nir_fadd(&b, a, d);
        break;
    case LLVMFSub:
        r = nir_fsub(&b, a, d);
        break;
    case LLVMFMul:
        r = nir_fmul(&b, a, d);
        break;
    case LLVMFDiv:
        r = nir_fdiv(&b, a, d);
        break;
    case LLVMFNeg:
        r = nir_fneg(&b, a);
        break;
    case LLVMAnd:
        r = nir_iand(&b, a, d);
        break;
    case LLVMOr:
        r = nir_ior(&b, a, d);
        break;
    case LLVMXor:
        r = nir_ixor(&b, a, d);
        break;
    case LLVMShl:
        r = nir_ishl(&b, a, nir_u2u32(&b, d));
        break;
    case LLVMLShr:
        r = nir_ushr(&b, a, nir_u2u32(&b, d));
        break;
    case LLVMAShr:
        r = nir_ishr(&b, a, nir_u2u32(&b, d));
        break;
    default:
        fail("NIR frontend unsupported opcode " + std::to_string(op));
    }
    set(i, r);
}
void Direct::flow() {
    for (auto block = LLVMGetFirstBasicBlock(f); block; block = LLVMGetNextBasicBlock(block)) {
        ids[block] = blocks.size();
        blocks.push_back(block);
    }
    if (blocks.empty() || blocks.size() > 128)
        fail("NIR CFG limit");
    unsigned count = blocks.size();
    std::vector<unsigned> color(count);
    std::set<unsigned> live;
    bool cyclic = false;
    auto successors = [&](unsigned at) {
        std::vector<unsigned> v;
        auto term = LLVMGetBasicBlockTerminator(blocks[at]);
        for (unsigned n = 0; n < LLVMGetNumSuccessors(term); ++n)
            v.push_back(ids.at(LLVMGetSuccessor(term, n)));
        return v;
    };
    std::function<void(unsigned)> visit = [&](unsigned at) {
        if (color[at]) {
            cyclic |= color[at] == 1;
            return;
        }
        color[at] = 1;
        live.insert(at);
        for (auto next : successors(at))
            visit(next);
        color[at] = 2;
    };
    visit(0);
    auto all = live;
    all.insert(count);
    postdom.assign(count + 1, all);
    postdom[count] = {count};
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto at : live) {
            auto next = successors(at);
            if (next.empty())
                next.push_back(count);
            auto v = postdom[next[0]];
            for (unsigned n = 1; n < next.size(); ++n) {
                std::set<unsigned> intersection;
                std::set_intersection(v.begin(), v.end(), postdom[next[n]].begin(), postdom[next[n]].end(),
                                      std::inserter(intersection, intersection.end()));
                v = intersection;
            }
            v.insert(at);
            if (v != postdom[at]) {
                postdom[at] = v;
                changed = true;
            }
        }
    }
    join.assign(count, count);
    for (auto at : live)
        for (auto candidate : postdom[at])
            if (candidate != at) {
                bool nearest = true;
                for (auto other : postdom[at])
                    if (other != at && other != candidate && !postdom[candidate].count(other))
                        nearest = false;
                if (nearest) {
                    join[at] = candidate;
                    break;
                }
            }
    initializeSSA = cyclic;
    for (auto at : live)
        for (auto i = LLVMGetFirstInstruction(blocks[at]); i; i = LLVMGetNextInstruction(i)) {
            auto type = LLVMTypeOf(i);
            if (LLVMGetTypeKind(type) != LLVMVoidTypeKind && LLVMGetTypeKind(type) != LLVMPointerTypeKind)
                values[i] = reserve(type);
        }
    std::map<LLVMValueRef, std::set<LLVMValueRef>> provenance;
    for (const auto &root : rootIDs)
        provenance[root.first].insert(root.first);
    bool pointerChange = true;
    while (pointerChange) {
        pointerChange = false;
        for (auto at : live)
            for (auto i = LLVMGetFirstInstruction(blocks[at]); i; i = LLVMGetNextInstruction(i)) {
                if (LLVMGetTypeKind(LLVMTypeOf(i)) != LLVMPointerTypeKind)
                    continue;
                auto op = LLVMGetInstructionOpcode(i);
                if (op != LLVMPHI && op != LLVMSelect && op != LLVMGetElementPtr && op != LLVMBitCast)
                    continue;
                auto &roots = provenance[i];
                auto before = roots.size();
                unsigned n = op == LLVMPHI ? LLVMCountIncoming(i) : op == LLVMSelect ? 3 : 1;
                for (unsigned k = op == LLVMSelect ? 1 : 0; k < n; ++k) {
                    auto source = op == LLVMPHI ? LLVMGetIncomingValue(i, k) : LLVMGetOperand(i, k);
                    if (LLVMIsAConstantPointerNull(source) || LLVMIsUndef(source) || LLVMIsPoison(source))
                        provenance[source].insert(nullptr);
                    const auto &incoming = provenance[source];
                    roots.insert(incoming.begin(), incoming.end());
                }
                pointerChange |= before != roots.size();
            }
    }
    for (auto at : live)
        for (auto i = LLVMGetFirstInstruction(blocks[at]); i; i = LLVMGetNextInstruction(i)) {
            if (LLVMGetTypeKind(LLVMTypeOf(i)) != LLVMPointerTypeKind || provenance[i].empty() ||
                LLVMIsAAllocaInst(i))
                continue;
            NV value;
            value.kind = NV::Pointer;
            value.roots = provenance[i];
            if (value.roots.size() == 1)
                value.root = *value.roots.begin();
            value.cell = nir_local_variable_create(b.impl, glsl_vector_type(GLSL_TYPE_UINT, 3),
                                                   ("ptr" + std::to_string(serial++)).c_str());
            nir_store_var(&b, value.cell, nir_imm_zero(&b, 3, 32), 7);
            values[i] = value;
        }
    // Resource reflection follows instance-derived offsets through casts and
    // cyclic PHIs, independently of numeric width and block emission order.
    bool indexChange = true;
    while (indexChange) {
        indexChange = false;
        for (auto at : live)
            for (auto i = LLVMGetFirstInstruction(blocks[at]); i; i = LLVMGetNextInstruction(i)) {
                if (!values.count(i) || values[i].index == 2)
                    continue;
                switch (LLVMGetInstructionOpcode(i)) {
                case LLVMZExt:
                case LLVMSExt:
                case LLVMTrunc:
                case LLVMBitCast:
                case LLVMGetElementPtr:
                case LLVMAdd:
                case LLVMSub:
                case LLVMMul:
                case LLVMShl:
                case LLVMLShr:
                case LLVMAShr:
                case LLVMAnd:
                case LLVMOr:
                case LLVMXor:
                case LLVMSelect:
                case LLVMPHI:
                    for (unsigned k = 0; k < unsigned(LLVMGetNumOperands(i)); ++k) {
                        auto source = LLVMGetOperand(i, k);
                        if (values.count(source) && values[source].index == 2) {
                            values[i].index = 2;
                            indexChange = true;
                            break;
                        }
                    }
                    break;
                default:
                    break;
                }
            }
    }
    auto edge = [&](unsigned from, unsigned to) {
        std::vector<std::pair<NV, NV>> copies;
        for (auto phi = LLVMGetFirstInstruction(blocks[to]); phi && LLVMGetInstructionOpcode(phi) == LLVMPHI;
             phi = LLVMGetNextInstruction(phi))
            for (unsigned n = 0; n < LLVMCountIncoming(phi); ++n)
                if (LLVMGetIncomingBlock(phi, n) == blocks[from]) {
                    NV v = get(LLVMGetIncomingValue(phi, n));
                    std::function<NV(NV)> snapshot = [&](NV value) {
                        if (value.kind == NV::Data)
                            return data(read(value));
                        if (value.kind == NV::Pointer) {
                            value.def = pointerPayload(value);
                            value.cell = nullptr;
                            value.root = nullptr;
                        }
                        if (value.kind == NV::Aggregate)
                            for (auto &member : value.members)
                                member = snapshot(member);
                        return value;
                    };
                    v = snapshot(v);
                    copies.push_back({values.at(phi), v});
                }
        for (auto &p : copies)
            copy(p.first, p.second);
    };
    struct NaturalLoop {
        std::set<unsigned> body;
        unsigned exit = 0;
    };
    std::map<unsigned, NaturalLoop> loops;
    bool structured = cyclic;
    if (cyclic) {
        std::vector<std::set<unsigned>> dominators(count, live), predecessors(count);
        dominators[0] = {0};
        for (auto at : live)
            for (auto next : successors(at))
                predecessors[next].insert(at);
        bool progress = true;
        while (progress) {
            progress = false;
            for (auto at : live)
                if (at) {
                    auto common = live;
                    for (auto pred : predecessors[at]) {
                        std::set<unsigned> intersection;
                        std::set_intersection(common.begin(), common.end(), dominators[pred].begin(),
                                              dominators[pred].end(),
                                              std::inserter(intersection, intersection.end()));
                        common = std::move(intersection);
                    }
                    common.insert(at);
                    if (common != dominators[at]) {
                        dominators[at] = std::move(common);
                        progress = true;
                    }
                }
        }
        std::set<std::pair<unsigned, unsigned>> backedges;
        for (auto at : live)
            for (auto next : successors(at))
                if (dominators[at].count(next)) {
                    backedges.insert({at, next});
                    auto &loop = loops[next];
                    loop.body.insert(next);
                    std::vector<unsigned> pending{at};
                    while (!pending.empty()) {
                        auto member = pending.back();
                        pending.pop_back();
                        if (!loop.body.insert(member).second)
                            continue;
                        for (auto pred : predecessors[member])
                            pending.push_back(pred);
                    }
                }
        for (auto &entry : loops) {
            auto &loop = entry.second;
            std::set<unsigned> exits;
            for (auto member : loop.body) {
                for (auto pred : predecessors[member])
                    if (member != entry.first && !loop.body.count(pred))
                        structured = false;
                for (auto next : successors(member))
                    if (!loop.body.count(next))
                        exits.insert(next);
            }
            if (exits.size() != 1)
                structured = false;
            else
                loop.exit = *exits.begin();
        }
        // Removing natural backedges must leave a DAG. Other cyclic graphs
        // use Mesa's structurizer below instead of duplicating cyclic paths.
        std::vector<unsigned> colors(count);
        std::function<void(unsigned)> check = [&](unsigned at) {
            if (colors[at]) {
                if (colors[at] == 1)
                    structured = false;
                return;
            }
            colors[at] = 1;
            for (auto next : successors(at))
                if (!backedges.count({at, next}))
                    check(next);
            colors[at] = 2;
        };
        check(0);
        structured &= !loops.empty();
    }
    if (cyclic && !structured) {
        // Emit each LLVM block once, including its structured memory guards.
        // Then flatten those guards and connect LLVM edges in unstructured NIR.
        // Mesa's structurizer handles multiple exits and irreducible SCCs;
        // the common reducible path below retains its compact natural loops.
        std::map<unsigned, nir_block *> entries;
        std::map<nir_block *, unsigned> transfers;
        auto transfer = [&](unsigned from, unsigned to) {
            edge(from, to);
            transfers[nir_cursor_current_block(b.cursor)] = to;
            nir_jump(&b, nir_jump_return); // temporary boundary, replaced below
        };
        auto preamble = nir_cursor_current_block(b.cursor);
        for (auto at : live) {
            auto region = nir_push_if(&b, nir_imm_true(&b));
            entries[at] = nir_if_first_then_block(region);
            for (auto i = LLVMGetFirstInstruction(blocks[at]); i; i = LLVMGetNextInstruction(i))
                instruction(i);
            auto term = LLVMGetBasicBlockTerminator(blocks[at]);
            auto op = LLVMGetInstructionOpcode(term);
            if (op == LLVMRet)
                nir_jump(&b, nir_jump_return);
            else if (branch(term)) {
                if (LLVMGetNumSuccessors(term) == 1)
                    transfer(at, ids.at(LLVMGetSuccessor(term, 0)));
                else {
                    nir_push_if(&b, read(get(LLVMGetCondition(term))));
                    transfer(at, ids.at(LLVMGetSuccessor(term, 0)));
                    nir_push_else(&b, nullptr);
                    transfer(at, ids.at(LLVMGetSuccessor(term, 1)));
                    nir_pop_if(&b, nullptr);
                }
            } else if (op == LLVMSwitch) {
                auto selector = arg(term, 0);
                std::function<void(unsigned)> alternative = [&](unsigned n) {
                    if (n == LLVMGetNumSuccessors(term)) {
                        transfer(at, ids.at(LLVMGetSuccessor(term, 0)));
                        return;
                    }
                    nir_push_if(&b, nir_ieq_imm(&b, selector, number(switchCase(term, n))));
                    transfer(at, ids.at(LLVMGetSuccessor(term, n)));
                    nir_push_else(&b, nullptr);
                    alternative(n + 1);
                    nir_pop_if(&b, nullptr);
                };
                alternative(1);
            } else
                fail("NIR unsupported terminator");
            nir_pop_if(&b, region);
        }
        struct FlatBlock {
            nir_block *block, *yes, *no;
            nir_def *condition;
        };
        std::vector<FlatBlock> flat;
        nir_foreach_block(block, b.impl) {
            auto next = nir_cf_node_next(&block->cf_node);
            auto condition =
                next && next->type == nir_cf_node_if ? nir_cf_node_as_if(next)->condition.ssa : nullptr;
            flat.push_back({block, block->successors[0], block->successors[1], condition});
        }
        // Snapshot first: removing a jump updates successors and predecessors.
        for (auto &item : flat)
            if (nir_block_ends_in_jump(item.block))
                nir_instr_remove(nir_block_last_instr(item.block));
        for (auto &item : flat) {
            auto block = item.block;
            exec_node_remove(&block->cf_node.node);
            block->cf_node.parent = &b.impl->cf_node;
            block->successors[0] = block->successors[1] = nullptr;
            _mesa_set_clear(block->predecessors, nullptr);
        }
        // Detached if nodes still hold condition uses; drop these before the
        // old CF tree becomes unreachable so SSA liveness stays accurate.
        std::function<void(exec_list *)> dropConditions = [&](exec_list *list) {
            foreach_list_typed(nir_cf_node, node, node, list) {
                if (node->type == nir_cf_node_if) {
                    auto nif = nir_cf_node_as_if(node);
                    list_del(&nif->condition.use_link);
                    nif->condition = nir_src_init();
                    dropConditions(&nif->then_list);
                    dropConditions(&nif->else_list);
                } else if (node->type == nir_cf_node_loop) {
                    auto loop = nir_cf_node_as_loop(node);
                    dropConditions(&loop->body);
                    dropConditions(&loop->continue_list);
                }
            }
        };
        dropConditions(&b.impl->body);
        exec_list_make_empty(&b.impl->body);
        _mesa_set_clear(b.impl->end_block->predecessors, nullptr);
        b.impl->structured = false;
        for (auto &item : flat)
            exec_list_push_tail(&b.impl->body, &item.block->cf_node.node);
        for (auto &item : flat) {
            b.cursor = nir_after_block(item.block);
            if (item.block == preamble)
                nir_goto(&b, entries.at(0));
            else if (transfers.count(item.block))
                nir_goto(&b, entries.at(transfers.at(item.block)));
            else if (item.no && item.no != item.yes)
                nir_goto_if(&b, item.yes, item.condition, item.no);
            else
                nir_goto(&b, item.yes ? item.yes : b.impl->end_block);
        }
        nir_metadata_preserve(b.impl, nir_metadata_none);
        nir_sort_unstructured_blocks(b.impl);
        nir_lower_goto_ifs(b.shader);
        b.cursor = nir_after_impl(b.impl);
        return;
    }
    std::vector<unsigned> activeLoops;
    auto loopJump = [&](unsigned next) {
        if (activeLoops.empty())
            return false;
        auto header = activeLoops.back();
        if (next == header) {
            nir_jump(&b, nir_jump_continue);
            return true;
        }
        if (next == loops.at(header).exit) {
            nir_jump(&b, nir_jump_break);
            return true;
        }
        return false;
    };
    std::function<void(unsigned, unsigned)> path;
    path = [&](unsigned at, unsigned stop) {
        while (at != stop && at != count) {
            if (structured && loops.count(at) &&
                std::find(activeLoops.begin(), activeLoops.end(), at) == activeLoops.end()) {
                auto header = at;
                activeLoops.push_back(header);
                nir_push_loop(&b);
                path(header, count);
                nir_pop_loop(&b, nullptr);
                activeLoops.pop_back();
                at = loops.at(header).exit;
                continue;
            }
            if (++visits > 512)
                fail("NIR CFG duplication limit");
            for (auto i = LLVMGetFirstInstruction(blocks[at]); i; i = LLVMGetNextInstruction(i))
                instruction(i);
            auto term = LLVMGetBasicBlockTerminator(blocks[at]);
            auto op = LLVMGetInstructionOpcode(term);
            if (op == LLVMRet) {
                if (!activeLoops.empty())
                    nir_jump(&b, nir_jump_return);
                return;
            }
            if (branch(term) && LLVMGetNumSuccessors(term) == 1) {
                unsigned next = ids.at(LLVMGetSuccessor(term, 0));
                edge(at, next);
                if (loopJump(next))
                    return;
                at = next;
                continue;
            }
            unsigned merge = join[at];
            if (branch(term)) {
                unsigned yes = ids.at(LLVMGetSuccessor(term, 0)), no = ids.at(LLVMGetSuccessor(term, 1));
                nir_push_if(&b, read(get(LLVMGetCondition(term))));
                edge(at, yes);
                if (!loopJump(yes))
                    path(yes, merge);
                nir_push_else(&b, nullptr);
                edge(at, no);
                if (!loopJump(no))
                    path(no, merge);
                nir_pop_if(&b, nullptr);
            } else if (op == LLVMSwitch) {
                auto selector = arg(term, 0);
                unsigned branches = LLVMGetNumSuccessors(term);
                std::function<void(unsigned)> alternative = [&](unsigned n) {
                    if (n == branches) {
                        unsigned next = ids.at(LLVMGetSuccessor(term, 0));
                        edge(at, next);
                        if (!loopJump(next))
                            path(next, merge);
                        return;
                    }
                    auto key = switchCase(term, n);
                    nir_push_if(&b, nir_ieq_imm(&b, selector, number(key)));
                    unsigned next = ids.at(LLVMGetSuccessor(term, n));
                    edge(at, next);
                    if (!loopJump(next))
                        path(next, merge);
                    nir_push_else(&b, nullptr);
                    alternative(n + 1);
                    nir_pop_if(&b, nullptr);
                };
                alternative(1);
            } else
                fail("NIR unsupported terminator");
            if (loopJump(merge))
                return;
            at = merge;
        }
    };
    path(0, count);
}
void mergeReflection(QGPUProgramImage &image, const QGPUProgramDescription &reflection) {
    for (unsigned variant = 0; variant < (image.info.stage == Shader::Fragment ? 4u : 1u); ++variant) {
        auto &p = variant == 0   ? image.info
                  : variant == 1 ? image.msaa.info
                  : variant == 2 ? image.constantWindow.info
                                 : image.msaaConstantWindow.info;
        if (!p.codeWords)
            continue;
        p.bufferMask = reflection.bufferMask;
        p.writeMask = reflection.writeMask;
        p.instanceMask = reflection.instanceMask;
        p.textureBindings = reflection.textureBindings;
        if (p.framebufferMask & ~reflection.framebufferMask)
            fail("NIR undeclared framebuffer");
        p.depthTextureMask =
            reflection.depthTextureMask & (p.imageMask | p.sampleMask | p.fetchMask | p.queryMask);
        if (p.varyingCount > reflection.varyingCount)
            fail("NIR varying count mismatch");
        p.varyingCount = reflection.varyingCount;
        for (unsigned n = 0; n < QGPU_MAX_VARYINGS; ++n) {
            if (n >= p.varyingCount) {
                p.varyings[n] = {};
                continue;
            }
            const auto &v = reflection.varyings[n];
            if (p.varyings[n].components &&
                (p.varyings[n].components != v.components || p.varyings[n].mode != v.mode))
                fail("NIR varying interface mismatch");
            memcpy(p.varyings[n].semantic, v.semantic, sizeof(v.semantic));
            p.varyings[n].components = v.components;
            p.varyings[n].mode = v.mode;
        }
        memcpy(p.strides, reflection.strides, sizeof(p.strides));
    }
    if (image.constantWindow.info.codeWords &&
        (!JIT::compatibleConstantWindow(image.info, image.constantWindow.info) ||
         !JIT::compatibleConstantWindow(image.msaa.info, image.msaaConstantWindow.info))) {
        image.constantWindow = {};
        image.msaaConstantWindow = {};
    }
}
} // namespace
} // namespace Compiler
} // namespace GPU
namespace GPU {
namespace Compiler {
static bool compileAIRNIRModule(const void *bytes, size_t size, const std::string &selected,
                                const std::vector<FunctionConstantValue> &constants, uint64_t registry,
                                QGPUProgramImage &out, std::string &error, AIRNIRMetrics &stats,
                                int (*backend)(nir_shader *, const QGPUProgramDescription *,
                                               QGPUProgramImage *, char *, unsigned),
                                const std::function<void(nir_shader *)> &inspect = {}) {
    nir_shader *nir = nullptr;
    stats = {};
    error.clear();
    glsl_type_singleton_init_or_ref();
    try {
        auto start = Clock::now();
        Module m;
        initializeModule(bytes, size, m);
        std::string chosen = selected;
        if (chosen.empty()) {
            for (const char *key : {"air.kernel", "air.vertex", "air.fragment"}) {
                unsigned count = LLVMGetNamedMetadataNumOperands(m.module, key);
                if (!count)
                    continue;
                if (count != 1 || !chosen.empty())
                    fail("NIR single-image API requires a selected entry");
                LLVMValueRef metadata;
                LLVMGetNamedMetadataOperands(m.module, key, &metadata);
                auto fields = node(metadata);
                if (fields.empty() || !LLVMIsAFunction(fields[0]))
                    fail("NIR entry metadata");
                chosen = name(fields[0]);
            }
            if (chosen.empty())
                fail("NIR entry absent");
        }
        AIRSpecialization::State specialization(m.module, chosen);
        if (specialization.present())
            specialization.apply(constants);
        specialization.lowerHelpers();
        auto specialized = Clock::now();
        LLVMValueRef metadata = nullptr;
        unsigned stage = 0;
        for (unsigned s = 0; s < 3; ++s) {
            auto key = s == 0 ? "air.kernel" : s == 1 ? "air.vertex" : "air.fragment";
            unsigned count = LLVMGetNamedMetadataNumOperands(m.module, key);
            std::vector<LLVMValueRef> entries(count);
            LLVMGetNamedMetadataOperands(m.module, key, entries.data());
            for (auto entry : entries)
                if (name(node(entry).at(0)) == chosen) {
                    if (metadata)
                        fail("NIR duplicate entry");
                    metadata = entry;
                    stage = s;
                }
        }
        if (!metadata)
            fail("NIR entry absent");
        Direct emitter(node(metadata).at(0), m.layout, stage);
        nir = emitter.run(metadata, stats);
        auto emitted = Clock::now();
        if (inspect)
            inspect(nir);
        char diagnostic[4096] = {};
        bool ok = backend(nir, &emitter.reflection, &out, diagnostic, sizeof(diagnostic));
        ralloc_free(nir);
        nir = nullptr;
        if (!ok)
            fail(std::string("NIR backend: ") + diagnostic);
        out.registry = registry;
        out.version = JIT::Version;
        out.gpu = 540;
        if (chosen.size() >= sizeof(out.name))
            fail("NIR name limit");
        memcpy(out.name, chosen.c_str(), chosen.size() + 1);
        mergeReflection(out, emitter.reflection);
        if (!JIT::valid(out, registry)) {
            fail("NIR output exceeds production JIT profile base=" +
                 std::to_string(JIT::validDescription(out.info)) +
                 " msaa=" + std::to_string(JIT::validDescription(out.msaa.info)) + " flags=" +
                 std::to_string(out.info.flags) + " sampleFlags=" + std::to_string(out.info.sampleFlags) +
                 " const=" + std::to_string(out.info.constlen) +
                 " regs=" + std::to_string(out.info.fullRegs));
        }
        auto finished = Clock::now();
        stats.parseMS = elapsed(start, specialized);
        stats.emitMS = elapsed(specialized, emitted);
        stats.backendMS = elapsed(emitted, finished);
        glsl_type_singleton_decref();
        return true;
    } catch (const std::exception &e) {
        if (nir)
            ralloc_free(nir);
        glsl_type_singleton_decref();
        error = e.what();
        return false;
    }
}
} // namespace Compiler
} // namespace GPU
