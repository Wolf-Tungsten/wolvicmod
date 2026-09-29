#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <queue>
#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

#include "wolvicmod/core/action.h"
#include "wolvicmod/elab/flatten.h"
#include "wolvicmod/sim/errors.h"

namespace wolvicmod::detail {

// Waveform sink (§6.3): the user drives sampling explicitly via
// waveDump(time) — Verilator-style, time advancement is fully user-controlled.
// Implemented by wave/fst.h when WOLVICMOD_WITH_FST.
class WaveDumperBase {
public:
    virtual ~WaveDumperBase() = default;
    virtual void dump(uint64_t time) = 0;
};

// Round-level trace sink (§6.3): edge hits, activated Updates, and committed
// state writes of every round, for backtracing. Implemented by dbg/trace.h.
class TraceSinkBase {
public:
    virtual ~TraceSinkBase() = default;
    virtual void onRound(uint64_t evalCount, size_t round) = 0;
    virtual void onEdge(const std::string& desc) = 0;
    virtual void onActivate(const Action* a) = 0;
    virtual void onCommit(const Entity* e) = 0;
};

// Everything the simulation engine needs (§4.3). Dirty-driven scheduling
// (§5.2): value changes notify dependents by setting one bit per dependent
// in a static-order activity bitmap (Flat); phase 1 scans it in execOrder
// order (= topological), phase 2 scans a chain bitmap likewise. Trace mode
// forces a full sweep so every action's trace state stays current.
struct SimState {
    // A non-trivial strongly connected component of the combinational
    // evaluation graph: iterated to a steady state at run time.
    struct SccGroup {
        std::vector<Action*> actions;  // Assigns, in registration order
        std::vector<Entity*> signals;  // member signals, watched for convergence
    };
    struct ExecItem {
        Action* single = nullptr;
        size_t group = SIZE_MAX;  // index into groups when single == nullptr
    };
    // Actions in topological execution order; trivial components collapse to
    // one item, cycles to one group item.
    std::vector<ExecItem> execOrder;
    std::vector<std::unique_ptr<SccGroup>> groups;

    // Sequential priority chains (§4.3): Updates sharing a target state, in
    // registration order (earliest = highest priority).
    struct Chain {
        std::vector<Action*> updates;
        // Phase-2 fast commit binding (§5.2): set when the target is a Reg
        // and every member published a byte-copyable intent — phase 2 then
        // applies intents and detects the change with type-agnostic byte ops,
        // no per-member virtual calls. reg == nullptr selects the slow path.
        Entity* reg = nullptr;
        char* cur = nullptr;   // Reg<T>::cur_
        char* next = nullptr;  // Reg<T>::next_ (shared intent landing slot)
        uint32_t size = 0;
        bool isBool = false;
        bool bitwiseEq = false;  // memcmp is an exact change test for T
        Action::TypedCommitFn typedCommit = nullptr;  // per-type fallback commit
        // Member-bitmap commit (§5.2): member intent state lives in the chain
        // descriptor — bit i of memberBits mirrors updates[i]'s intent
        // (registration order) and intentSrcs[i] is its intent blob address —
        // so phase 2 applies and clears intents without dereferencing any
        // member Action. Requires the fast-commit binding and <= 64 members.
        uint64_t memberBits = 0;
        std::vector<const char*> intentSrcs;
        bool memberCommit = false;
    };
    std::vector<Chain> chains;

    size_t sccIterCap = 100;    // steady-state iteration limit (§4.3)
    size_t roundCap = 10000;    // zero-delay oscillation limit (§5.3)

    // Flat dispatch (§5.2): entities OR dependent bits into actBits and
    // Updates OR chain bits into chainBits when their intent rises.
    std::vector<uint64_t> actBits;    // per execOrder position
    std::vector<uint64_t> chainBits;  // per priority chain
    std::vector<const Entity*> committed;  // per-round scratch: first 64 commits (diagnostics)
    size_t committedN = 0;                 // commits this round (convergence test)
    std::vector<uint32_t> depPool;      // dependent exec positions, CSR
    std::vector<uint32_t> depOff;       // per entity flat index, size E+1
    // Edge-direction-filtered dispatch (§5.2 push): per entity, dependents
    // watching only one edge direction. CSR pools bound via bindEdgeWatches.
    std::vector<Entity::EdgeWatchDep> posWatchPool;
    std::vector<Entity::EdgeWatchDep> negWatchPool;

    std::unique_ptr<WaveDumperBase> wave;  // waveform sink (§6.3)
    std::unique_ptr<TraceSinkBase> trace;  // round-level trace sink (§6.3)
    uint64_t evalCount = 0;                // eval() calls so far

    bool auditReads = false;        // read-set accounting (§6.4)
    bool assertUpdateMutex = false; // Update mutual-exclusion assertion (§6.4)
};

namespace graph_impl {

class Tarjan {
public:
    explicit Tarjan(const std::vector<std::vector<uint32_t>>& adj)
        : adj_(adj), index_(adj.size(), kUnset), low_(adj.size(), 0), onStack_(adj.size(), false) {}

    std::vector<std::vector<uint32_t>> run() {
        for (uint32_t v = 0; v < adj_.size(); ++v)
            if (index_[v] == kUnset) strongConnect(v);
        return std::move(comps_);
    }

private:
    static constexpr uint32_t kUnset = UINT32_MAX;

    void strongConnect(uint32_t v) {
        index_[v] = low_[v] = next_++;
        stack_.push(v);
        onStack_[v] = true;
        for (uint32_t w : adj_[v]) {
            if (index_[w] == kUnset) {
                strongConnect(w);
                low_[v] = std::min(low_[v], low_[w]);
            } else if (onStack_[w]) {
                low_[v] = std::min(low_[v], index_[w]);
            }
        }
        if (low_[v] == index_[v]) {
            comps_.emplace_back();
            while (true) {
                uint32_t w = stack_.top();
                stack_.pop();
                onStack_[w] = false;
                comps_.back().push_back(w);
                if (w == v) break;
            }
        }
    }

    const std::vector<std::vector<uint32_t>>& adj_;
    std::vector<uint32_t> index_, low_;
    std::vector<bool> onStack_;
    std::stack<uint32_t> stack_;
    std::vector<std::vector<uint32_t>> comps_;
    uint32_t next_ = 0;
};

}  // namespace graph_impl

// Build the combinational evaluation graph (§4.3) and the topological
// execution order. Nodes 0..E-1 are entities, E..E+A-1 are actions. Edges:
// assign -> driven signal; entity -> every action reading it.
// canon[] maps aliased entities (identity-connect targets) to their
// canonical source; eliminated actions contribute no edges at all.
inline void buildExecOrder(const FlatModel& flat, SimState& sim,
                           const std::vector<uint32_t>& canon) {
    const uint32_t E = static_cast<uint32_t>(flat.entities.size());
    const uint32_t A = static_cast<uint32_t>(flat.actions.size());
    const uint32_t N = E + A;

    std::vector<std::vector<uint32_t>> adj(N);
    std::vector<std::vector<uint32_t>> inEdges(N);
    for (uint32_t j = 0; j < A; ++j) {
        const Action* a = flat.actions[j];
        if (a->eliminated()) continue;
        const uint32_t an = E + j;
        if (a->kind() == Action::Kind::Assign) {
            const uint32_t t = canon[a->target()->flatIndex()];
            adj[an].push_back(t);
            inEdges[t].push_back(an);
        }
        for (const Entity* r : a->reads()) {
            const uint32_t rn = canon[r->flatIndex()];
            adj[rn].push_back(an);
            inEdges[an].push_back(rn);
        }
    }

    const auto comps = graph_impl::Tarjan(adj).run();

    std::vector<uint32_t> compOf(N, 0);
    for (uint32_t c = 0; c < comps.size(); ++c)
        for (uint32_t v : comps[c]) compOf[v] = c;

    // Non-trivial components: size > 1, or a single node with a self-loop.
    std::vector<bool> nonTrivial(comps.size(), false);
    for (uint32_t c = 0; c < comps.size(); ++c) {
        if (comps[c].size() > 1) {
            nonTrivial[c] = true;
        } else {
            const uint32_t v = comps[c][0];
            if (std::find(adj[v].begin(), adj[v].end(), v) != adj[v].end()) nonTrivial[c] = true;
        }
    }

    // Condensation DAG + Kahn topological order (sources first).
    std::vector<std::vector<uint32_t>> cadj(comps.size());
    std::vector<uint32_t> indeg(comps.size(), 0);
    for (uint32_t v = 0; v < N; ++v)
        for (uint32_t w : adj[v])
            if (compOf[v] != compOf[w]) cadj[compOf[v]].push_back(compOf[w]);
    for (auto& outs : cadj) {
        std::sort(outs.begin(), outs.end());
        outs.erase(std::unique(outs.begin(), outs.end()), outs.end());
        for (uint32_t w : outs) ++indeg[w];
    }
    // Condensation DAG + topological emission (sources first). The scheduler
    // is type-greedy (§5.2): among ready components prefer the one whose
    // action shares the vtable of the action emitted just before —
    // consecutive same-type dispatches give the phase-1 virtual call a
    // repetitive target (BTB-friendly) and walk same-size malloc clusters.
    // Any linear extension of the condensation DAG preserves the
    // producer-before-consumer invariant the Flat scan relies on
    // (re-asserted below when the reverse map is built).
    std::vector<uint32_t> topo;
    topo.reserve(comps.size());
    {
        // vtable pointer = exact action instantiation type identity.
        const auto vptrOf = [&](const Action* a) {
            const void* tag = nullptr;
            static_assert(sizeof(tag) == sizeof(uintptr_t));
            std::memcpy(&tag, a, sizeof(tag));
            return tag;
        };
        // Ready components, split by what they emit: entity nodes and
        // eliminated actions produce no exec item and are emitted as soon as
        // ready (they only unblock consumers); action nodes also queue per
        // exact type; SCC groups queue in arrival order only and break the
        // current type run. seq keeps global arrival order as the fallback
        // choice; entries emitted via byTag are skipped lazily.
        std::queue<uint32_t> freeNodes;
        std::unordered_map<const void*, std::queue<uint32_t>> byTag;
        std::queue<uint32_t> seq;
        std::vector<char> emitted(comps.size(), 0);
        const void* lastTag = nullptr;
        size_t remaining = comps.size();
        const auto pushComp = [&](uint32_t c) {
            if (!nonTrivial[c]) {
                const uint32_t v = comps[c][0];
                if (v < E || flat.actions[v - E]->eliminated()) {
                    freeNodes.push(c);
                    return;
                }
                byTag[vptrOf(flat.actions[v - E])].push(c);
            }
            seq.push(c);
        };
        const auto emit = [&](uint32_t c) {
            emitted[c] = 1;
            topo.push_back(c);
            --remaining;
            for (uint32_t w : cadj[c])
                if (--indeg[w] == 0) pushComp(w);
        };
        for (uint32_t c = 0; c < comps.size(); ++c)
            if (indeg[c] == 0) pushComp(c);
        while (remaining > 0) {
            while (!freeNodes.empty()) {
                const uint32_t c = freeNodes.front();
                freeNodes.pop();
                emit(c);
            }
            if (remaining == 0) break;  // the free-node drain may finish the graph
            uint32_t c = UINT32_MAX;
            if (lastTag != nullptr) {
                auto it = byTag.find(lastTag);
                if (it != byTag.end()) {
                    auto& tq = it->second;
                    // A comp stays queued here after a fallback emission too —
                    // skip stale entries just like the seq fallback does.
                    while (!tq.empty() && emitted[tq.front()]) tq.pop();
                    if (!tq.empty()) {
                        c = tq.front();
                        tq.pop();
                    }
                }
            }
            if (c == UINT32_MAX) {
                while (!seq.empty() && emitted[seq.front()]) seq.pop();
                if (seq.empty())
                    fail("internal: topological scheduler ran out of ready components");
                c = seq.front();
                seq.pop();
            }
            lastTag = nonTrivial[c] ? nullptr : vptrOf(flat.actions[comps[c][0] - E]);
            emit(c);
        }
    }
    if (topo.size() != comps.size())
        fail("internal: topological scheduler emitted " + std::to_string(topo.size()) +
             " of " + std::to_string(comps.size()) + " components");

    // Emit execution items; entity nodes produce no work, and eliminated
    // (alias-resolved) actions are dropped entirely.
    for (const uint32_t c : topo) {
        if (!nonTrivial[c]) {
            const uint32_t v = comps[c][0];
            if (v >= E && !flat.actions[v - E]->eliminated())
                sim.execOrder.push_back({flat.actions[v - E], SIZE_MAX});
            continue;
        }
        auto group = std::make_unique<SimState::SccGroup>();
        for (const uint32_t v : comps[c]) {
            if (v >= E)
                group->actions.push_back(flat.actions[v - E]);
            else
                group->signals.push_back(flat.entities[v]);
        }
        std::sort(group->actions.begin(), group->actions.end(),
                  [](const Action* x, const Action* y) { return x->regIndex() < y->regIndex(); });
        // Steady-state iteration requires comparable signal values (§4.3).
        std::string paths;
        for (const Entity* s : group->signals) {
            if (!s->snapshotSupported())
                fail("combinational cycle involves '" + s->hierPath() +
                     "', whose type is not equality-comparable; steady-state "
                     "iteration requires operator== (§4.3)");
            paths += (paths.empty() ? "" : " -> ") + s->hierPath();
        }
        sim.execOrder.push_back({nullptr, sim.groups.size()});
        sim.groups.push_back(std::move(group));
    }

    // ---- flat-dispatch wiring (§5.2) ----
    const uint32_t nItems = static_cast<uint32_t>(sim.execOrder.size());
    const auto forEachRead = [&](const SimState::ExecItem& item, auto&& f) {
        if (item.single != nullptr) {
            for (const Entity* r : item.single->reads()) f(r);
        } else {
            for (const Action* a : sim.groups[item.group]->actions)
                for (const Entity* r : a->reads()) f(r);
        }
    };
    // entity flat index -> producer exec position (Assign drivers only;
    // UINT32_MAX for sources: root inputs, Reg/Mem, constants).
    std::vector<uint32_t> producerOf(E, UINT32_MAX);
    for (uint32_t pos = 0; pos < nItems; ++pos) {
        const auto& item = sim.execOrder[pos];
        if (item.single != nullptr) {
            if (item.single->kind() == Action::Kind::Assign)
                producerOf[canon[item.single->target()->flatIndex()]] = pos;
        } else {
            for (const Action* a : sim.groups[item.group]->actions)
                if (a->kind() == Action::Kind::Assign)
                    producerOf[canon[a->target()->flatIndex()]] = pos;
        }
    }
    // Reverse dependency map (CSR): entity -> exec positions reading it.
    // Self-notification is excluded: an item never needs to be woken by its
    // own output — for an SCC group these are the internal cycle edges,
    // whose propagation runSccGroup already handles by internal iteration.
    //
    // Edge-direction filtering (§5.2 push): a read that is an eligible
    // one-direction edge watch (Action::edgeWatches) moves from the
    // always-push map into the entity's pos/neg watch list, so a transition
    // in the opposite direction no longer wakes the Update. The filter
    // relies on one transition per entity per round, so entities driven from
    // inside an SCC group (rewritten by every steady-state iteration) are
    // excluded.
    const auto watchOf = [&](const SimState::ExecItem& item,
                             uint32_t ri) -> const Action::EdgeWatch* {
        if (item.single == nullptr) return nullptr;
        for (const auto& w : item.single->edgeWatches())
            if (canon[w.sig->flatIndex()] == ri) return &w;
        return nullptr;
    };
    const auto filterable = [&](uint32_t pos, uint32_t ri) -> const Action::EdgeWatch* {
        const uint32_t p = producerOf[ri];
        if (p != UINT32_MAX && sim.execOrder[p].single == nullptr) return nullptr;
        return watchOf(sim.execOrder[pos], ri);
    };
    std::vector<std::vector<Entity::EdgeWatchDep>> posW(E), negW(E);
    sim.depOff.assign(E + 1, 0);
    for (uint32_t pos = 0; pos < nItems; ++pos)
        forEachRead(sim.execOrder[pos], [&](const Entity* r) {
            const uint32_t ri = canon[r->flatIndex()];
            if (producerOf[ri] != pos && filterable(pos, ri) == nullptr)
                ++sim.depOff[ri + 1];
        });
    for (uint32_t i = 0; i < E; ++i) sim.depOff[i + 1] += sim.depOff[i];
    sim.depPool.resize(sim.depOff[E]);
    std::vector<uint32_t> cursor(sim.depOff.begin(), sim.depOff.end() - 1);
    for (uint32_t pos = 0; pos < nItems; ++pos)
        forEachRead(sim.execOrder[pos], [&](const Entity* r) {
            const uint32_t ri = canon[r->flatIndex()];
            const uint32_t p = producerOf[ri];
            if (p == pos) return;
            // Flat-dispatch correctness (§5.2): the activity bitmap is scanned
            // in execOrder order, so a producer must precede its consumers —
            // a backward edge would be set after its position was scanned and
            // silently skipped until the next round. Topological order
            // guarantees this; assert it once here rather than at run time.
            if (p != UINT32_MAX && p > pos)
                fail("exec order is not topological: '" + r->hierPath() +
                     "' is produced at position " + std::to_string(p) +
                     " but consumed at " + std::to_string(pos));
            if (const auto* w = filterable(pos, ri)) {
                auto& pool = (w->kind == EdgeKind::Posedge) ? posW[ri] : negW[ri];
                pool.push_back({pos, w->prev});
            } else {
                sim.depPool[cursor[ri]++] = pos;
            }
        });
    // Flat activity bitmap: one bit per execOrder position. Allocated to its
    // final size before binding (entities hold the data pointer).
    sim.actBits.assign((nItems + 63) / 64, 0);
    for (uint32_t i = 0; i < E; ++i)
        flat.entities[i]->bindDeps(sim.depPool.data() + sim.depOff[i],
                                   sim.depOff[i + 1] - sim.depOff[i], sim.actBits.data());
    // Flatten the per-entity watch lists into CSR pools and bind them.
    std::vector<uint32_t> posOff(E + 1, 0), negOff(E + 1, 0);
    for (uint32_t i = 0; i < E; ++i) {
        posOff[i + 1] = posOff[i] + static_cast<uint32_t>(posW[i].size());
        negOff[i + 1] = negOff[i] + static_cast<uint32_t>(negW[i].size());
    }
    sim.posWatchPool.resize(posOff[E]);
    sim.negWatchPool.resize(negOff[E]);
    for (uint32_t i = 0; i < E; ++i) {
        std::copy(posW[i].begin(), posW[i].end(), sim.posWatchPool.begin() + posOff[i]);
        std::copy(negW[i].begin(), negW[i].end(), sim.negWatchPool.begin() + negOff[i]);
        flat.entities[i]->bindEdgeWatches(
            posOff[i + 1] > posOff[i] ? sim.posWatchPool.data() + posOff[i] : nullptr,
            posOff[i + 1] - posOff[i],
            negOff[i + 1] > negOff[i] ? sim.negWatchPool.data() + negOff[i] : nullptr,
            negOff[i + 1] - negOff[i]);
    }
}

// Group Updates by target state in registration order (§4.3). Also binds the
// commit notification: when an Update's intent flag rises it sets its chain
// bit, so phase 2 visits only committing chains.
inline void buildUpdateChains(const FlatModel& flat, SimState& sim) {
    std::unordered_map<Entity*, size_t> chainOf;
    for (Action* a : flat.actions) {
        if (a->kind() != Action::Kind::Update) continue;
        auto [it, inserted] = chainOf.emplace(a->target(), sim.chains.size());
        if (inserted) sim.chains.emplace_back();
        sim.chains[it->second].updates.push_back(a);
    }
    // Allocated to its final size before binding (actions hold the pointer).
    sim.chainBits.assign((sim.chains.size() + 63) / 64, 0);
    for (Action* a : flat.actions) {
        if (a->kind() != Action::Kind::Update) continue;
        a->bindCommit(static_cast<uint32_t>(chainOf[a->target()]), sim.chainBits.data());
    }
    // Phase-2 fast commit binding (§5.2): a chain whose target is a Reg and
    // whose members all published a byte-copyable intent applies and commits
    // with type-agnostic byte ops instead of per-member virtual calls; with
    // <= 64 members the member intent state moves into the chain descriptor's
    // memberBits bitmap (+ dense intentSrcs table), so phase 2 never
    // dereferences a member Action.
    for (auto& chain : sim.chains) {
        Entity* tgt = chain.updates.front()->target();
        if (tgt->kind() != EntityKind::Reg) continue;
        void* cur = tgt->valueStorage();
        void* next = tgt->nextStorage();
        const uint32_t size = chain.updates.front()->fastIntentSize();
        if (cur == nullptr || next == nullptr || size == 0) continue;
        bool allFast = chain.updates.front()->fastIntentSrc() != nullptr;
        for (Action* a : chain.updates)
            allFast = allFast && (a->fastIntentSrc() != nullptr && a->fastIntentSize() == size);
        if (!allFast) continue;
        chain.reg = tgt;
        chain.cur = static_cast<char*>(cur);
        chain.next = static_cast<char*>(next);
        chain.size = size;
        chain.isBool = tgt->isBoolReg();
        chain.bitwiseEq = tgt->bitwiseEqOk();
        chain.typedCommit = chain.updates.front()->typedCommitFn();
        // Member-bitmap commit (§5.2): bind each member's intent mirror bit
        // and blob address. sim.chains is fully built by now, so the
        // memberBits address bound into the Actions stays valid. Chains with
        // > 64 members keep the per-member polling fast path.
        if (chain.updates.size() <= 64) {
            chain.memberCommit = true;
            chain.intentSrcs.reserve(chain.updates.size());
            for (uint32_t i = 0; i < chain.updates.size(); ++i) {
                chain.intentSrcs.push_back(chain.updates[i]->fastIntentSrc());
                chain.updates[i]->bindMemberBit(&chain.memberBits, uint64_t{1} << i);
            }
        }
    }
}

}  // namespace wolvicmod::detail
