#include "psvrecore/analysis/vfp.hpp"

#include <algorithm>
#include <bit>

#include "core/foundation/containers/hash_map.h"
#include "core/foundation/containers/hash_set.h"
#include "core/foundation/strings/format.h"

namespace psvrecore::analysis {

nx::string vfp_names(VfpSet s) {
    nx::string out;
    const auto add = [&](nx::string_view name) {
        if (!out.empty()) out += ' ';
        out += name;
    };
    for (u32 n = 0; n < 32; ++n) {
        const u64 d = d_words(n) & s.words;
        if (d == d_words(n)) add(nx::format("d{}", n));
        else if (d) add(nx::format("s{}", std::countr_zero(d)));
    }
    if (s.fpscr == kFpscrAll) add("fpscr");
    else if (s.fpscr == kFpscrNzcv) add("fpscr.nzcv");
    else if (s.fpscr == kFpscrRest) add("fpscr.rest");
    return out.empty() ? nx::string("none") : out;
}

namespace {

struct Fn {
    u32 code = 0;
    const Function* fn = nullptr;
    Cfg cfg;
    nx::vector<VfpEffect> effect;
    nx::vector<u32> callers;
    // The summary: words of whole D registers and FPSCR parts it (and its callees) may read or write in the context.
    VfpSet reads, writes;
    bool unknown = false;  // reads and writes everything
    bool queued = false;
};

class Solver {
public:
    Solver(const Program& program, const VfpOptions& options) : program_(program), options_(options) {}
    VfpAnalysis run();

private:
    u32 index(u32 code) const {
        auto it = index_.find(code);
        return it == index_.end() ? kNoIndex : it->second;
    }
    // What a call node may read (and write) of the context, in words of whole D registers.
    void call_sets(const CfgNode& node, VfpSet& touch, VfpSet& write) const {
        if (node.call == CallKind::Direct && options_.summaries) {
            const u32 t = index(node.call_target);
            if (t != kNoIndex && !fns_[t].unknown) {
                touch = fns_[t].reads | fns_[t].writes;
                write = fns_[t].writes;
                return;
            }
        }
        touch = write = kAllVfp;
    }
    void solve(const Fn& f, FunctionVfp& r) const;

    const Program& program_;
    const VfpOptions& options_;
    nx::vector<Fn> fns_;
    nx::flat_hash_map<u32, u32> index_;
};

// Per function: the view, the dirty units forward, the live units backward.
void Solver::solve(const Fn& f, FunctionVfp& r) const {
    const Cfg& g = f.cfg;
    const usize n = g.nodes.size();
    // How each of d0-d15 is held: as two floats when its instructions access it a word at a time at least as often as
    // whole (a D register's double then costs a pack or an unpack, an S register's float an extract or an insert).
    u32 s_count[16] = {}, d_count[16] = {};
    for (const VfpEffect& e : f.effect)
        for (u32 k = 0; k < 16; ++k) {
            s_count[k] += (e.s_access >> k) & 1;
            d_count[k] += (e.d_access >> k) & 1;
        }
    r.s_view = 0;
    for (u32 k = 0; k < 16; ++k)
        if (s_count[k] && s_count[k] >= d_count[k]) r.s_view |= 1u << k;
    r.trap_store.assign(n, {});
    r.call_store.assign(n, {});
    r.call_load.assign(n, {});
    r.exit_store.assign(n, {});
    r.after_store.assign(n, {});
    r.live_before.assign(n, {});
    r.may_trap.assign(n, 0);
    r.calls_keeping = 0;
    if (n == 0 || g.entry == kNoIndex) return;

    // Each instruction's effect in units: a unit written in part is read.
    nx::vector<VfpSet> uses(n), defs(n), touch(n), write(n);
    nx::vector<u8> sync(n, 0);
    for (usize i = 0; i < n; ++i) {
        const VfpEffect& e = f.effect[i];
        const VfpSet d = r.units(e.defs);
        uses[i] = r.units(e.uses) | r.units(d.minus(e.defs));
        defs[i] = d;
        r.may_trap[i] = e.traps;
        const CfgNode& node = g.nodes[i];
        if (node.call != CallKind::None) {
            sync[i] = 1;
            VfpSet t, w;
            call_sets(node, t, w);
            touch[i] = r.units(t), write[i] = r.units(w);
        } else if (!e.context.empty()) {
            sync[i] = 1;
            touch[i] = write[i] = r.units(e.context);
        }
    }

    // Forward: the units that may be dirty. `post`: after the instruction executes (its effect, its call); `after`:
    // the continuation after its translation (and the skip edge of a conditional one).
    const usize nb = g.blocks.size();
    nx::vector<VfpSet> at(nb);
    nx::vector<u8> reached(nb, 0);
    const u32 entry_block = g.block_of[g.entry];
    reached[entry_block] = 1;
    const auto step = [&](u32 i, VfpSet s, VfpSet& post, VfpSet& after, bool record) {
        const CfgNode& node = g.nodes[i];
        if (record && r.may_trap[i]) r.trap_store[i] = s;
        post = s | defs[i];
        if (sync[i]) {
            if (record) r.call_store[i] = post & touch[i];
            post = post.minus(touch[i]);
        } else if (record) {
            r.exit_store[i] = post;
        }
        const bool has_cont = node.cont_edge != kNoIndex && node.cont_edge < node.edge_count;
        after = has_cont ? post : VfpSet{};
        if (node.conditional) after |= s;
        if (record && (has_cont || node.conditional)) {
            const Edge& c = node.conditional ? node.skip : g.edges[node.first_edge + node.cont_edge];
            if (c.kind != EdgeKind::Next) r.after_store[i] = after;
        }
    };
    const auto edges_out = [&](u32 i, const VfpSet& post, const VfpSet& after, auto&& to) {
        const CfgNode& node = g.nodes[i];
        for (u32 k = 0; k < node.edge_count; ++k) {
            const Edge& e = g.edges[node.first_edge + k];
            if (e.kind == EdgeKind::Next) to(e.to, k == node.cont_edge ? after : post);
        }
        if (node.conditional && node.skip.kind == EdgeKind::Next) to(node.skip.to, after);
    };
    nx::vector<u32> work{entry_block};
    nx::vector<u8> queued(nb, 0);
    queued[entry_block] = 1;
    const auto run_block = [&](u32 b, bool record) {
        VfpSet s = at[b];
        const BasicBlock& blk = g.blocks[b];
        VfpSet post, after;
        for (u32 i = blk.first;; ++i) {
            step(i, s, post, after, record);
            if (i == blk.last) break;
            VfpSet next;
            edges_out(i, post, after, [&](u32, const VfpSet& from) { next |= from; });
            s = next;
        }
        edges_out(blk.last, post, after, [&](u32 to_index, const VfpSet& from) {
            const u32 to = g.block_of[to_index];
            const VfpSet joined = at[to] | from;
            if ((joined == at[to] && reached[to]) || record) return;
            at[to] = joined;
            reached[to] = 1;
            if (!queued[to]) queued[to] = 1, work.push_back(to);
        });
    };
    while (!work.empty()) {
        const u32 b = work.back();
        work.pop_back();
        queued[b] = 0;
        run_block(b, false);
    }
    for (u32 b = 0; b < nb; ++b)
        if (reached[b]) run_block(b, true);

    // Backward: the live units.
    nx::vector<VfpSet>& live = r.live_before;
    for (bool changed = true; changed;) {
        changed = false;
        for (usize i = n; i-- > 0;) {
            const CfgNode& node = g.nodes[i];
            const auto edge_use = [&](const Edge& e, bool is_cont) -> VfpSet {
                if (e.kind == EdgeKind::Next) return live[e.to];
                if (is_cont) return r.after_store[i];
                return e.kind == EdgeKind::Trap ? VfpSet{} : r.exit_store[i];
            };
            VfpSet exec;
            for (u32 k = 0; k < node.edge_count; ++k) exec |= edge_use(g.edges[node.first_edge + k], k == node.cont_edge);
            if (sync[i]) {
                r.call_load[i] = exec & write[i];
                exec = exec.minus(write[i]) | r.call_store[i];
            }
            VfpSet b = uses[i] | exec.minus(defs[i]);
            if (r.may_trap[i]) b |= r.trap_store[i];
            if (node.conditional) b |= edge_use(node.skip, true);
            if (!(b == live[i])) live[i] = b, changed = true;
        }
    }
    r.load_at_entry = live[g.entry];
    VfpSet touched = r.load_at_entry;
    for (usize i = 0; i < n; ++i) {
        touched |= uses[i] | defs[i] | r.trap_store[i] | r.call_store[i] | r.call_load[i] | r.exit_store[i] | r.after_store[i];
        // (a call after which a live unit stays in its local: one the callee does not write)
        if (sync[i] && g.nodes[i].call != CallKind::None) {
            VfpSet exec;
            for (u32 k = 0; k < g.nodes[i].edge_count; ++k) {
                const Edge& e = g.edges[g.nodes[i].first_edge + k];
                if (e.kind == EdgeKind::Next) exec |= live[e.to];
            }
            if (!exec.minus(write[i]).empty()) ++r.calls_keeping;
        }
    }
    r.touched = r.units(touched);
}

VfpAnalysis Solver::run() {
    VfpAnalysis out;
    for (const auto& [code, fn] : program_.functions) {
        index_[code] = static_cast<u32>(fns_.size());
        Fn f;
        f.code = code;
        f.fn = &fn;
        fns_.push_back(std::move(f));
    }
    for (Fn& f : fns_) {
        f.cfg = build_cfg(program_, *f.fn);
        const usize n = f.fn->code.size();
        const nx::vector<VfpEffect>* given = nullptr;
        if (options_.effects)
            if (auto it = options_.effects->find(f.code); it != options_.effects->end() && it->second.size() == n) given = &it->second;
        // (without the generator's record of an instruction, nothing is known of it: it reads and writes everything
        // in the context)
        f.effect.assign(n, VfpEffect{});
        for (usize k = 0; k < n; ++k) {
            if (given) f.effect[k] = (*given)[k];
            else f.effect[k].context = kAllVfp;
        }
        // The function's own part of its summary, and what makes it unknown.
        const FunctionRegisters* regs = options_.registers ? options_.registers->at(f.code) : nullptr;
        f.unknown = !options_.summaries || !given || n == 0 || f.cfg.entry == kNoIndex || !regs || !regs->returns_to_lr;
        for (usize k = 0; k < n && !f.unknown; ++k) {
            const VfpEffect& e = f.effect[k];
            const CfgNode& node = f.cfg.nodes[k];
            f.reads |= e.uses | e.defs | e.context;
            f.writes |= e.defs | e.context;
            if (node.call == CallKind::Import || node.call == CallKind::Indirect || node.call == CallKind::System) f.unknown = true;
            const auto edge = [&](const Edge& e) {
                if (e.kind == EdgeKind::IndirectJump || e.kind == EdgeKind::ImportTail) f.unknown = true;
            };
            for (const Edge* e = f.cfg.begin(node); e != f.cfg.end(node); ++e) edge(*e);
            if (node.conditional) edge(node.skip);
        }
        f.reads.words = whole_d(f.reads.words);
        f.writes.words = whole_d(f.writes.words);
        if (f.unknown) f.reads = f.writes = kAllVfp;
    }
    // Callers (direct calls and tail calls), then the least fixed point of the summaries.
    for (u32 i = 0; i < fns_.size(); ++i) {
        const Fn& f = fns_[i];
        nx::flat_hash_set<u32> callees;
        for (const CfgNode& node : f.cfg.nodes) {
            if (node.call == CallKind::Direct) callees.insert(index(node.call_target));
            for (const Edge* e = f.cfg.begin(node); e != f.cfg.end(node); ++e)
                if (e->kind == EdgeKind::TailCall) callees.insert(index(e->to));
            if (node.conditional && node.skip.kind == EdgeKind::TailCall) callees.insert(index(node.skip.to));
        }
        nx::vector<u32> sorted(callees.begin(), callees.end());
        std::sort(sorted.begin(), sorted.end());
        for (u32 c : sorted) {
            if (c == kNoIndex) {
                // (a call or tail call to a function the program does not have: nothing known)
                fns_[i].unknown = true;
                fns_[i].reads = fns_[i].writes = kAllVfp;
            } else {
                fns_[c].callers.push_back(i);
            }
        }
    }
    {
        nx::vector<u32> work;
        for (u32 i = static_cast<u32>(fns_.size()); i-- > 0;) work.push_back(i), fns_[i].queued = true;
        while (!work.empty()) {
            const u32 i = work.back();
            work.pop_back();
            Fn& f = fns_[i];
            f.queued = false;
            if (f.unknown) continue;
            VfpSet reads = f.reads, writes = f.writes;
            bool unknown = false;
            for (const CfgNode& node : f.cfg.nodes) {
                const auto take = [&](u32 code) {
                    const u32 t = index(code);
                    if (t == kNoIndex || fns_[t].unknown) unknown = true;
                    else reads |= fns_[t].reads, writes |= fns_[t].writes;
                };
                if (node.call == CallKind::Direct) take(node.call_target);
                for (const Edge* e = f.cfg.begin(node); e != f.cfg.end(node); ++e)
                    if (e->kind == EdgeKind::TailCall) take(e->to);
                if (node.conditional && node.skip.kind == EdgeKind::TailCall) take(node.skip.to);
            }
            if (unknown) reads = writes = kAllVfp;
            if (reads == f.reads && writes == f.writes && unknown == f.unknown) continue;
            f.reads = reads, f.writes = writes, f.unknown = unknown;
            for (u32 c : f.callers)
                if (!fns_[c].queued) fns_[c].queued = true, work.push_back(c);
        }
    }
    for (const Fn& f : fns_) {
        FunctionVfp& r = out.functions[f.code];
        r.reads = f.reads;
        r.writes = f.writes;
        solve(f, r);
    }
    return out;
}

}  // namespace

VfpAnalysis vfp_analysis(const Program& program, const VfpOptions& options) { return Solver(program, options).run(); }

}  // namespace psvrecore::analysis
