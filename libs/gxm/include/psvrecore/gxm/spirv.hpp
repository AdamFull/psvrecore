#pragma once

// A small SPIR-V module builder: types and constants deduplicated, global variables, decorations, one function
// with its blocks, and the instructions the shader translator emits. Ids are allocated as things are created;
// module() lays the sections out in the order SPIR-V requires.

#include <initializer_list>
#include <span>

#include "core/foundation/containers/hash_map.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::gxm::spirv {

using Id = u32;

class Builder {
public:
    Builder();

    // Module header.
    void capability(u32 cap);
    void extension(const char* name);
    Id glsl();  // the GLSL.std.450 instruction set
    void entry_point(u32 model, Id function, const char* name, std::span<const Id> interface);
    void execution_mode(Id function, u32 mode, std::initializer_list<u32> args = {});

    // Types (deduplicated).
    Id t_void();
    Id t_bool();
    Id t_uint();
    Id t_int();
    Id t_float();
    Id t_int64();
    Id t_uint64();
    Id t_vector(Id component, u32 count);
    Id t_array(Id element, u32 count);
    Id t_runtime_array(Id element);
    Id t_struct(std::initializer_list<Id> members);  // (not deduplicated: structs carry decorations)
    Id t_pointer(u32 storage, Id type);
    Id t_function(Id result, std::initializer_list<Id> params = {});
    // OpTypeImage (dim: SpvDim*, sampled: 1 with a sampler, 2 storage or subpass data; format: SpvImageFormat*).
    Id t_image(Id sampled_type, u32 dim, u32 depth, u32 arrayed, u32 multisampled, u32 sampled, u32 format);
    Id t_sampled_image(Id image);

    // Constants (deduplicated).
    Id c_uint(u32 v);
    Id c_int(i32 v);
    Id c_float(f32 v);
    Id c_bool(bool v);
    Id c_composite(Id type, std::initializer_list<Id> parts);
    Id c_int64(i64 v);
    Id c_null(Id type);

    // Global variables and decorations.
    Id variable(Id pointer_type, u32 storage);  // a module-scope variable
    Id variable(Id pointer_type, u32 storage, Id initializer);
    void decorate(Id target, u32 decoration, std::initializer_list<u32> args = {});
    void member_decorate(Id type, u32 member, u32 decoration, std::initializer_list<u32> args = {});
    void name(Id target, const char* text);

    // The function and its blocks. Function-scope variables go at the start of the first block.
    Id begin_function(Id result_type, Id function_type);
    Id local(Id pointer_type);
    void local(Id id, Id pointer_type, Id initializer);  // a function-scope variable with an id from reserve()
    Id reserve() { return next_++; }  // an id for something created later
    Id new_label() { return next_++; }
    void block(Id label);  // starts a block
    void end_function();

    // Instructions: op(opcode, result type, operands) returns the result id; op0 has no result.
    Id op(u32 opcode, Id result_type, std::initializer_list<u32> operands);
    Id op(u32 opcode, Id result_type, std::span<const u32> operands);
    void op0(u32 opcode, std::initializer_list<u32> operands);
    void op0(u32 opcode, std::span<const u32> operands);
    Id ext(Id result_type, u32 instruction, std::initializer_list<Id> operands);  // GLSL.std.450

    // The module as words.
    nx::vector<u32> module() const;

private:
    static void emit(nx::vector<u32>& out, u32 opcode, std::span<const u32> operands);
    static void emit(nx::vector<u32>& out, u32 opcode, std::initializer_list<u32> operands);
    static void string_words(nx::vector<u32>& out, const char* text);
    Id type(u32 opcode, std::initializer_list<u32> operands);

    Id next_ = 1;
    Id glsl_ = 0;
    nx::vector<u32> capabilities_, extensions_, imports_, entries_, modes_, names_, decorations_, globals_;
    nx::vector<u32> function_, locals_, body_;
    nx::hash_map<nx::string, Id> types_;
    nx::hash_map<nx::string, Id> constants_;
    bool in_function_ = false;
    bool first_block_ = false;
};

}  // namespace psvrecore::gxm::spirv
