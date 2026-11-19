#include "psvrecore/analysis/cfg.hpp"

#include <algorithm>

#include "psvrecore/analysis/flow.hpp"
#include "core/foundation/containers/hash_set.h"

namespace psvrecore::analysis {

using arm::Op;
using arm::OperandKind;

bool is_return_form(const arm::Instruction& in) {
    const auto& o = in.operands;
    switch (in.op) {
        case Op::Bx: return o[0].kind == OperandKind::Reg && o[0].reg == arm::kLr;
        case Op::Mov:
            return in.operand_count == 2 && o[0].kind == OperandKind::Reg && o[0].reg == arm::kPc &&
                   o[1].kind == OperandKind::Reg && o[1].reg == arm::kLr;
        case Op::Pop: return (o[0].imm & (1u << arm::kPc)) != 0;
        case Op::Ldm: case Op::Ldmib: case Op::Ldmda: case Op::Ldmdb:
            return o[0].reg == arm::kSp && (o[1].imm & (1u << arm::kPc)) != 0 && !(in.flags & arm::kUserRegs);
        case Op::Ldr:
            return o[0].kind == OperandKind::Reg && o[0].reg == arm::kPc && o[1].kind == OperandKind::Mem && o[1].reg == arm::kSp;
        default: return false;
    }
}

bool constant_word(const Program& program, u32 address, u32 size, u32& value) {
    const auto& image = *program.image;
    // (a word the loader patches with an imported variable's address is not known now)
    for (u32 at = address >= 3 ? address - 3 : 0; at < address + size; ++at)
        if (program.variable_uses.contains(at)) return false;
    const auto* seg = image.segment_of(address);
    if (!seg || seg->writable() || !image.contains(address, size)) return false;
    value = size == 4 ? image.u32_at(address) : size == 2 ? image.u16_at(address) : image.u8_at(address);
    return true;
}

bool constant_branch_target(const Program& program, const arm::Instruction& in, u32& target) {
    if (in.op != Op::Ldr || in.operands[0].kind != OperandKind::Reg || in.operands[0].reg != arm::kPc) return false;
    const auto& m = in.operands[1];
    if (m.kind != OperandKind::Mem || m.reg != arm::kPc || (m.flags & arm::kMemIndexReg) || (m.flags & arm::kMemWriteBack))
        return false;
    const u32 base = pc_value(in) & ~3u;
    return constant_word(program, (m.flags & arm::kMemSubtract) ? base - m.imm : base + m.imm, 4, target);
}

namespace {

// The constant a register holds at instruction `i` of `fn` when the straight-line code just before it (that no
// branch enters) puts one there: a literal load (ldr rN, =x) or movw/movt (not an imported variable's address).
bool constant_register_before(const Program& program, const Function& fn, const nx::flat_hash_set<u32>& labels, usize i,
                              u8 reg, u32& value) {
    if (reg == arm::kPc || reg == arm::kSp) return false;
    bool have_high = false;
    u32 high = 0;
    for (usize steps = 0; steps < 8 && i > 0; ++steps) {
        const arm::Instruction& next = fn.code[i];
        if (labels.contains(next.address) || next.address == fn.start()) return false;
        const arm::Instruction& in = fn.code[--i];
        if (in.address + in.size != next.address || is_conditional(in) || flow_of(in).kind != FlowKind::Next) return false;
        bool writes = in.operand_count >= 1 && in.operands[0].kind == OperandKind::Reg && in.operands[0].reg == reg;
        for (u8 k = 0; k < in.operand_count; ++k) {
            const auto& o = in.operands[k];
            if (o.kind == OperandKind::RegList && (o.imm & (1u << reg))) writes = true;
            if (o.kind == OperandKind::Mem && o.reg == reg && (o.flags & arm::kMemWriteBack)) writes = true;
        }
        if (in.op == Op::Push || in.op == Op::Stm || in.op == Op::Stmdb || in.op == Op::Stmda || in.op == Op::Stmib)
            writes = in.operands[0].kind == OperandKind::Reg && in.operands[0].reg == reg && (in.flags & arm::kWriteBack);
        if (!writes) continue;
        if (program.variable_uses.contains(in.address)) return false;
        if (in.op == Op::Movt && in.operands[1].kind == OperandKind::Imm && !have_high) {
            have_high = true, high = in.operands[1].imm << 16;
            continue;
        }
        if (in.op == Op::Movw && in.operands[1].kind == OperandKind::Imm) {
            value = (have_high ? high : 0) | (in.operands[1].imm & 0xFFFFu);
            return have_high;  // (movw alone: a small number, not a code address)
        }
        if (in.op == Op::Ldr && !have_high && in.operands[1].kind == OperandKind::Mem && in.operands[1].reg == arm::kPc &&
            !(in.operands[1].flags & (arm::kMemIndexReg | arm::kMemWriteBack))) {
            const u32 base = pc_value(in) & ~3u;
            return constant_word(program, (in.operands[1].flags & arm::kMemSubtract) ? base - in.operands[1].imm : base + in.operands[1].imm,
                                 4, value);
        }
        return false;
    }
    return false;
}

}  // namespace

Cfg build_cfg(const Program& program, const Function& fn) {
    Cfg g;
    const auto& code = fn.code;
    const usize n = code.size();
    g.nodes.resize(n);
    if (n == 0) return g;
    const auto index_of = [&](u32 address) -> u32 {
        const auto it = std::lower_bound(code.begin(), code.end(), address,
                                         [](const arm::Instruction& in, u32 a) { return in.address < a; });
        return it != code.end() && it->address == address ? static_cast<u32>(it - code.begin()) : kNoIndex;
    };
    g.entry = index_of(fn.start());
    nx::flat_hash_set<u32> noreturn(fn.noreturn_calls.begin(), fn.noreturn_calls.end());
    const nx::flat_hash_set<u32> labels(fn.labels.begin(), fn.labels.end());
    // A branch to a known code address (Emitter::branch_to): an import, a tail call, or a place in the function.
    const auto branch_edge = [&](u32 target_code) -> Edge {
        if (auto stub = program.import_stubs.find(target_code & ~1u); stub != program.import_stubs.end())
            return {EdgeKind::ImportTail, stub->second};
        if (target_code != fn.entry && program.function_at(target_code)) return {EdgeKind::TailCall, target_code};
        const u32 i = index_of(target_code & ~1u);
        return i == kNoIndex ? Edge{EdgeKind::Trap, 0} : Edge{EdgeKind::Next, i};
    };
    for (usize i = 0; i < n; ++i) {
        const arm::Instruction& in = code[i];
        CfgNode& node = g.nodes[i];
        node.first_edge = static_cast<u32>(g.edges.size());
        node.conditional = is_conditional(in);
        const Flow f = flow_of(in);
        // Where control continues after the instruction (Emitter::function): on into another function, the next
        // instruction, a goto to it, or a trap where code discovery did not go.
        const u32 next = in.address + in.size;
        Edge cont;
        if (auto into = fn.runs_into.find(in.address); into != fn.runs_into.end()) cont = branch_edge(into->second);
        else if (i + 1 < n && code[i + 1].address == next) cont = {EdgeKind::Next, static_cast<u32>(i + 1)};
        else if (const u32 at = index_of(next); at != kNoIndex) cont = {EdgeKind::Next, at};
        else cont = {EdgeKind::Trap, 0};
        const auto add = [&](Edge e) { g.edges.push_back(e); };
        switch (f.kind) {
            case FlowKind::Next:
                if (in.op == Op::Svc) node.call = CallKind::System;
                add(cont);
                break;
            case FlowKind::Call:
            case FlowKind::IndirectCall:
                if (f.kind == FlowKind::IndirectCall) {
                    node.call = CallKind::Indirect;
                } else if (auto stub = program.import_stubs.find(f.target & ~1u); stub != program.import_stubs.end()) {
                    node.call = CallKind::Import;
                    node.call_target = stub->second;
                } else if (program.function_at(f.target)) {
                    node.call = CallKind::Direct;
                    node.call_target = f.target;
                } else {
                    node.call = CallKind::Indirect;
                }
                // (a call to a function that never returns: a trap after it, when it executes)
                add(noreturn.contains(in.address) ? Edge{EdgeKind::Trap, 0} : cont);
                break;
            case FlowKind::Branch:
                add(branch_edge(f.target));
                if (in.op == Op::Cbz || in.op == Op::Cbnz) add(cont);
                break;
            case FlowKind::IndirectBranch: {
                if (auto t = fn.tables.find(in.address); t != fn.tables.end()) {
                    for (u32 target : t->second.targets) {
                        if (target != fn.entry && program.function_at(target)) {
                            add({EdgeKind::TailCall, target});
                        } else {
                            const u32 at = index_of(target & ~1u);
                            add(at == kNoIndex ? Edge{EdgeKind::Trap, 0} : Edge{EdgeKind::Next, at});
                        }
                    }
                    add({EdgeKind::Trap, 0});  // (an index outside the table whose entry leads nowhere known)
                    break;
                }
                u32 target = 0;
                // (bx rN after ldr rN, =f or movw/movt: a veneer's constant, as ldr pc, =f is)
                const bool constant_register = in.op == Op::Bx && in.operands[0].kind == OperandKind::Reg &&
                                               in.operands[0].reg != arm::kLr &&
                                               constant_register_before(program, fn, labels, i, in.operands[0].reg, target);
                if (constant_register || constant_branch_target(program, in, target)) {
                    // psv_jump to a constant: the function there (a veneer's), unless it is the return address.
                    add({EdgeKind::Return, 0});
                    if (program.function_at(target)) add({EdgeKind::TailCall, target});
                    else if (auto stub = program.import_stubs.find(target); stub != program.import_stubs.end())
                        add({EdgeKind::ImportTail, stub->second});
                    else add({EdgeKind::IndirectJump, 0});
                    break;
                }
                add({is_return_form(in) ? EdgeKind::Return : EdgeKind::IndirectJump, 0});
                break;
            }
            case FlowKind::Trap: add({EdgeKind::Trap, 0}); break;
        }
        node.edge_count = static_cast<u32>(g.edges.size()) - node.first_edge;
        if (node.conditional) node.skip = cont;
    }

    // Basic blocks: a block starts at the entry, at every target of an edge other than the one from the instruction
    // before, and after every instruction whose only way on is not the next instruction.
    nx::vector<u8> leader(n, 0);
    const auto plain = [&](usize i) {  // control goes only to the next instruction, through no call
        const CfgNode& node = g.nodes[i];
        if (node.call != CallKind::None || node.edge_count != 1) return false;
        const Edge& e = g.edges[node.first_edge];
        if (e.kind != EdgeKind::Next || e.to != i + 1) return false;
        return !node.conditional || (node.skip.kind == EdgeKind::Next && node.skip.to == i + 1);
    };
    if (g.entry != kNoIndex) leader[g.entry] = 1;
    leader[0] = 1;
    for (usize i = 0; i < n; ++i) {
        const CfgNode& node = g.nodes[i];
        const auto mark = [&](const Edge& e) {
            if (e.kind == EdgeKind::Next && e.to != i + 1) leader[e.to] = 1;
        };
        for (const Edge* e = g.begin(node); e != g.end(node); ++e) mark(*e);
        if (node.conditional) mark(node.skip);
        if (!plain(i) && i + 1 < n) leader[i + 1] = 1;
    }
    g.block_of.assign(n, 0);
    for (usize i = 0; i < n; ++i) {
        if (leader[i]) g.blocks.push_back(BasicBlock{static_cast<u32>(i), static_cast<u32>(i), {}, {}});
        g.blocks.back().last = static_cast<u32>(i);
        g.block_of[i] = static_cast<u32>(g.blocks.size() - 1);
    }
    for (usize b = 0; b < g.blocks.size(); ++b) {
        const CfgNode& node = g.nodes[g.blocks[b].last];
        auto& succ = g.blocks[b].successors;
        const auto link = [&](const Edge& e) {
            if (e.kind != EdgeKind::Next) return;
            const u32 to = g.block_of[e.to];
            if (std::find(succ.begin(), succ.end(), to) == succ.end()) succ.push_back(to);
        };
        for (const Edge* e = g.begin(node); e != g.end(node); ++e) link(*e);
        if (node.conditional) link(node.skip);
        for (u32 s : succ) g.blocks[s].predecessors.push_back(static_cast<u32>(b));
    }
    return g;
}

}  // namespace psvrecore::analysis
