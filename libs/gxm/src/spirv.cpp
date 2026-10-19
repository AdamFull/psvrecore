#include "psvrecore/gxm/spirv.hpp"

#include <bit>
#include <cstring>

#include "core/foundation/strings/format.h"
#include "spirv/unified1/spirv.h"

namespace psvrecore::gxm::spirv {

Builder::Builder() = default;

void Builder::emit(nx::vector<u32>& out, u32 opcode, std::span<const u32> operands) {
    out.push_back(static_cast<u32>(operands.size() + 1) << 16 | opcode);
    out.insert(out.end(), operands.begin(), operands.end());
}

void Builder::emit(nx::vector<u32>& out, u32 opcode, std::initializer_list<u32> operands) {
    emit(out, opcode, std::span<const u32>(operands.begin(), operands.size()));
}

void Builder::string_words(nx::vector<u32>& out, const char* text) {
    const usize n = std::strlen(text) + 1;  // (with its terminating zero)
    for (usize i = 0; i < n; i += 4) {
        u32 w = 0;
        for (usize j = 0; j < 4 && i + j < n; ++j) w |= static_cast<u32>(static_cast<u8>(text[i + j])) << (8 * j);
        out.push_back(w);
    }
}

void Builder::capability(u32 cap) { emit(capabilities_, SpvOpCapability, {cap}); }

void Builder::extension(const char* name) {
    nx::vector<u32> words;
    string_words(words, name);
    emit(extensions_, SpvOpExtension, words);
}

Id Builder::glsl() {
    if (!glsl_) {
        glsl_ = next_++;
        nx::vector<u32> words{glsl_};
        string_words(words, "GLSL.std.450");
        emit(imports_, SpvOpExtInstImport, words);
    }
    return glsl_;
}

void Builder::entry_point(u32 model, Id function, const char* name, std::span<const Id> interface) {
    nx::vector<u32> words{model, function};
    string_words(words, name);
    words.insert(words.end(), interface.begin(), interface.end());
    emit(entries_, SpvOpEntryPoint, words);
}

void Builder::execution_mode(Id function, u32 mode, std::initializer_list<u32> args) {
    nx::vector<u32> words{function, mode};
    words.insert(words.end(), args.begin(), args.end());
    emit(modes_, SpvOpExecutionMode, words);
}

Id Builder::type(u32 opcode, std::initializer_list<u32> operands) {
    nx::string key = nx::format("{}", opcode);
    for (u32 o : operands) key += nx::format(",{}", o);
    if (const auto it = types_.find(key); it != types_.end()) return it->second;
    const Id id = next_++;
    nx::vector<u32> words{id};
    words.insert(words.end(), operands.begin(), operands.end());
    emit(globals_, opcode, words);
    types_[key] = id;
    return id;
}

Id Builder::t_void() { return type(SpvOpTypeVoid, {}); }
Id Builder::t_bool() { return type(SpvOpTypeBool, {}); }
Id Builder::t_uint() { return type(SpvOpTypeInt, {32, 0}); }
Id Builder::t_int() { return type(SpvOpTypeInt, {32, 1}); }
Id Builder::t_float() { return type(SpvOpTypeFloat, {32}); }
Id Builder::t_int64() { return type(SpvOpTypeInt, {64, 1}); }
Id Builder::t_uint64() { return type(SpvOpTypeInt, {64, 0}); }
Id Builder::t_vector(Id component, u32 count) { return type(SpvOpTypeVector, {component, count}); }
Id Builder::t_array(Id element, u32 count) { return type(SpvOpTypeArray, {element, c_uint(count)}); }
Id Builder::t_runtime_array(Id element) {
    // (not deduplicated: an array stride decoration is the caller's)
    const Id id = next_++;
    emit(globals_, SpvOpTypeRuntimeArray, {id, element});
    return id;
}

Id Builder::t_struct(std::initializer_list<Id> members) {
    const Id id = next_++;
    nx::vector<u32> words{id};
    words.insert(words.end(), members.begin(), members.end());
    emit(globals_, SpvOpTypeStruct, words);
    return id;
}

Id Builder::t_pointer(u32 storage, Id t) { return type(SpvOpTypePointer, {storage, t}); }

Id Builder::t_function(Id result, std::initializer_list<Id> params) {
    nx::string key = nx::format("fn{}", result);
    for (Id p : params) key += nx::format(",{}", p);
    if (const auto it = types_.find(key); it != types_.end()) return it->second;
    const Id id = next_++;
    nx::vector<u32> words{id, result};
    words.insert(words.end(), params.begin(), params.end());
    emit(globals_, SpvOpTypeFunction, words);
    types_[key] = id;
    return id;
}

Id Builder::c_uint(u32 v) {
    const nx::string key = nx::format("u{}", v);
    if (const auto it = constants_.find(key); it != constants_.end()) return it->second;
    const Id id = next_++;
    emit(globals_, SpvOpConstant, {t_uint(), id, v});
    constants_[key] = id;
    return id;
}

Id Builder::c_int(i32 v) {
    const nx::string key = nx::format("i{}", v);
    if (const auto it = constants_.find(key); it != constants_.end()) return it->second;
    const Id id = next_++;
    emit(globals_, SpvOpConstant, {t_int(), id, static_cast<u32>(v)});
    constants_[key] = id;
    return id;
}

Id Builder::c_float(f32 v) {
    const u32 bits = std::bit_cast<u32>(v);
    const nx::string key = nx::format("f{}", bits);
    if (const auto it = constants_.find(key); it != constants_.end()) return it->second;
    const Id id = next_++;
    emit(globals_, SpvOpConstant, {t_float(), id, bits});
    constants_[key] = id;
    return id;
}

Id Builder::c_bool(bool v) {
    const nx::string key = v ? "true" : "false";
    if (const auto it = constants_.find(key); it != constants_.end()) return it->second;
    const Id id = next_++;
    emit(globals_, v ? SpvOpConstantTrue : SpvOpConstantFalse, {t_bool(), id});
    constants_[key] = id;
    return id;
}

Id Builder::c_composite(Id t, std::initializer_list<Id> parts) {
    nx::string key = nx::format("c{}", t);
    for (Id p : parts) key += nx::format(",{}", p);
    if (const auto it = constants_.find(key); it != constants_.end()) return it->second;
    const Id id = next_++;
    nx::vector<u32> words{t, id};
    words.insert(words.end(), parts.begin(), parts.end());
    emit(globals_, SpvOpConstantComposite, words);
    constants_[key] = id;
    return id;
}

Id Builder::c_int64(i64 v) {
    const nx::string key = nx::format("l{}", v);
    if (const auto it = constants_.find(key); it != constants_.end()) return it->second;
    const Id id = next_++;
    const u64 bits = static_cast<u64>(v);
    emit(globals_, SpvOpConstant, {t_int64(), id, static_cast<u32>(bits), static_cast<u32>(bits >> 32)});
    constants_[key] = id;
    return id;
}

Id Builder::c_null(Id t) {
    const nx::string key = nx::format("n{}", t);
    if (const auto it = constants_.find(key); it != constants_.end()) return it->second;
    const Id id = next_++;
    emit(globals_, SpvOpConstantNull, {t, id});
    constants_[key] = id;
    return id;
}

Id Builder::variable(Id pointer_type, u32 storage, Id initializer) {
    const Id id = next_++;
    emit(globals_, SpvOpVariable, {pointer_type, id, storage, initializer});
    return id;
}

Id Builder::variable(Id pointer_type, u32 storage) {
    const Id id = next_++;
    emit(globals_, SpvOpVariable, {pointer_type, id, storage});
    return id;
}

void Builder::decorate(Id target, u32 decoration, std::initializer_list<u32> args) {
    nx::vector<u32> words{target, decoration};
    words.insert(words.end(), args.begin(), args.end());
    emit(decorations_, SpvOpDecorate, words);
}

void Builder::member_decorate(Id t, u32 member, u32 decoration, std::initializer_list<u32> args) {
    nx::vector<u32> words{t, member, decoration};
    words.insert(words.end(), args.begin(), args.end());
    emit(decorations_, SpvOpMemberDecorate, words);
}

void Builder::name(Id target, const char* text) {
    nx::vector<u32> words{target};
    string_words(words, text);
    emit(names_, SpvOpName, words);
}

Id Builder::begin_function(Id result_type, Id function_type) {
    const Id id = next_++;
    emit(function_, SpvOpFunction, {result_type, id, SpvFunctionControlMaskNone, function_type});
    in_function_ = true;
    first_block_ = true;
    return id;
}

Id Builder::local(Id pointer_type) {
    const Id id = next_++;
    emit(locals_, SpvOpVariable, {pointer_type, id, SpvStorageClassFunction});
    return id;
}

void Builder::local(Id id, Id pointer_type, Id initializer) {
    emit(locals_, SpvOpVariable, {pointer_type, id, SpvStorageClassFunction, initializer});
}

void Builder::block(Id label) {
    if (first_block_) {
        emit(function_, SpvOpLabel, {label});
        first_block_ = false;
    } else {
        emit(body_, SpvOpLabel, {label});
    }
}

void Builder::end_function() {
    emit(body_, SpvOpFunctionEnd, {});
    in_function_ = false;
}

Id Builder::op(u32 opcode, Id result_type, std::span<const u32> operands) {
    const Id id = next_++;
    nx::vector<u32> words{result_type, id};
    words.insert(words.end(), operands.begin(), operands.end());
    emit(body_, opcode, words);
    return id;
}

Id Builder::op(u32 opcode, Id result_type, std::initializer_list<u32> operands) {
    return op(opcode, result_type, std::span<const u32>(operands.begin(), operands.size()));
}

void Builder::op0(u32 opcode, std::initializer_list<u32> operands) { emit(body_, opcode, operands); }
void Builder::op0(u32 opcode, std::span<const u32> operands) { emit(body_, opcode, operands); }

Id Builder::ext(Id result_type, u32 instruction, std::initializer_list<Id> operands) {
    nx::vector<u32> words{glsl(), instruction};
    words.insert(words.end(), operands.begin(), operands.end());
    return op(SpvOpExtInst, result_type, words);
}

nx::vector<u32> Builder::module() const {
    nx::vector<u32> out{SpvMagicNumber, 0x00010300u /* SPIR-V 1.3 (Vulkan 1.1) */, 0x00000000u, next_, 0};
    auto add = [&](const nx::vector<u32>& v) { out.insert(out.end(), v.begin(), v.end()); };
    add(capabilities_);
    add(extensions_);
    add(imports_);
    emit(out, SpvOpMemoryModel, {SpvAddressingModelLogical, SpvMemoryModelGLSL450});
    add(entries_);
    add(modes_);
    add(names_);
    add(decorations_);
    add(globals_);
    add(function_);
    add(locals_);
    add(body_);
    return out;
}

}  // namespace psvrecore::gxm::spirv
