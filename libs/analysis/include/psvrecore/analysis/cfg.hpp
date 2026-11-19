#pragma once

// A function's control-flow graph, as the code generator translates its control flow: one node per instruction of
// Function::code, its edges where control goes after it, and its basic blocks. Dataflow over guest state (the
// condition flags today: flags.hpp; registers next) runs on it.
//
// Each node is an instruction in three parts, in this order: its condition (a conditional instruction that does not
// execute goes on along its `skip` edge), its own effect, then a call (a node is at most one call), then its edges.
// An edge goes to another instruction of the function, or out of it:
// - Return: a branch through a register or memory of the form functions return with (bx lr, pop {.., pc},
//   ldm sp.., {.., pc}, ldr pc, [sp..], mov pc, lr). The generated code returns when the address is the one the
//   function was called with (`ret`), and otherwise jumps there through the runtime (psv_jump);
// - IndirectJump: any other branch through a register or memory (bx r3, ldr pc, [r0], add pc, ...): psv_jump, or a
//   return when it goes to `ret`;
// - TailCall: on into another function of the program (a branch to its entry, a table entry, running into it, or a
//   constant indirect jump: ldr pc, =f, or bx rN just after ldr rN, =f or movw/movt), which returns where this
//   function would;
// - ImportTail: the same into an imported function;
// - Trap: the program stops (undefined or untranslatable instructions, udf, bkpt, exception returns, a table index
//   outside its table, control reaching code discovery did not follow, a call to a function that never returns
//   returning).
// Code the generator translates to a trap of its own (an instruction it does not implement) keeps the edges its flow
// has: the graph may have paths the generated code does not, never the reverse.

#include "psvrecore/analysis/program.hpp"
#include "core/foundation/core/foundation.h"

namespace psvrecore::analysis {

inline constexpr u32 kNoIndex = 0xFFFFFFFFu;

enum class EdgeKind : u8 { Next, Return, IndirectJump, TailCall, ImportTail, Trap };

struct Edge {
    EdgeKind kind = EdgeKind::Trap;
    u32 to = 0;  // Next: an instruction index; TailCall: a function's code address; ImportTail: an import's index
};

enum class CallKind : u8 {
    None,
    Direct,    // bl/blx to a function of the program: call_target is its code address
    Import,    // to an imported function's stub: call_target is the import's index
    Indirect,  // blx rN, or bl/blx to an address no function starts at (the runtime's lookup, psv_call)
    System,    // svc
};

struct CfgNode {
    u32 first_edge = 0;   // into Cfg::edges: where control goes after the instruction executes
    u32 edge_count = 0;
    Edge skip;            // conditional instructions: where control goes when the condition fails
    bool conditional = false;
    // The continuation the generator emits after the instruction's own translation (the next instruction, a goto,
    // running into another function, a trap where discovery did not go): which of the edges it is (kNoIndex: none;
    // the skip edge of a conditional instruction is always it).
    u32 cont_edge = 0xFFFFFFFFu;
    CallKind call = CallKind::None;
    u32 call_target = 0;
};

struct BasicBlock {
    u32 first = 0, last = 0;                    // instruction indices, inclusive
    nx::vector<u32> successors, predecessors;   // block indices (edges inside the function)
};

struct Cfg {
    nx::vector<CfgNode> nodes;      // one per instruction of Function::code
    nx::vector<Edge> edges;
    nx::vector<BasicBlock> blocks;  // in address order of their first instruction
    nx::vector<u32> block_of;       // instruction index -> block index
    u32 entry = kNoIndex;           // the instruction at the function's entry (kNoIndex: a function without code)

    const Edge* begin(const CfgNode& n) const { return edges.data() + n.first_edge; }
    const Edge* end(const CfgNode& n) const { return edges.data() + n.first_edge + n.edge_count; }
};

Cfg build_cfg(const Program& program, const Function& fn);

// Whether an indirect branch has the form of a return (see EdgeKind::Return).
bool is_return_form(const arm::Instruction& in);

// A word of the module at generation time: in a read-only segment, not patched by the loader (an imported
// variable's address). The code generator folds such loads (literal pools).
bool constant_word(const Program& program, u32 address, u32 size, u32& value);

// The constant address an indirect branch goes to, when it loads one from a literal (ldr pc, [pc, #k]).
bool constant_branch_target(const Program& program, const arm::Instruction& in, u32& target);

}  // namespace psvrecore::analysis
