#pragma once

#include <algorithm>
#include <cstdint>
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

// Everything the simulation engine needs (§4.3).
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
    };
    std::vector<Chain> chains;

    size_t sccIterCap = 100;    // steady-state iteration limit (§4.3)
    size_t roundCap = 10000;    // zero-delay oscillation limit (§5.3)

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
inline void buildExecOrder(const FlatModel& flat, SimState& sim) {
    const uint32_t E = static_cast<uint32_t>(flat.entities.size());
    const uint32_t A = static_cast<uint32_t>(flat.actions.size());
    const uint32_t N = E + A;

    std::vector<std::vector<uint32_t>> adj(N);
    std::vector<std::vector<uint32_t>> inEdges(N);
    for (uint32_t j = 0; j < A; ++j) {
        const Action* a = flat.actions[j];
        const uint32_t an = E + j;
        if (a->kind() == Action::Kind::Assign) {
            const uint32_t t = a->target()->flatIndex();
            adj[an].push_back(t);
            inEdges[t].push_back(an);
        }
        for (const Entity* r : a->reads()) {
            const uint32_t rn = r->flatIndex();
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
    std::queue<uint32_t> q;
    for (uint32_t c = 0; c < comps.size(); ++c)
        if (indeg[c] == 0) q.push(c);
    std::vector<uint32_t> topo;
    topo.reserve(comps.size());
    while (!q.empty()) {
        const uint32_t c = q.front();
        q.pop();
        topo.push_back(c);
        for (uint32_t w : cadj[c])
            if (--indeg[w] == 0) q.push(w);
    }

    // Emit execution items; entity nodes produce no work.
    for (const uint32_t c : topo) {
        if (!nonTrivial[c]) {
            const uint32_t v = comps[c][0];
            if (v >= E) sim.execOrder.push_back({flat.actions[v - E], SIZE_MAX});
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
}

// Group Updates by target state in registration order (§4.3).
inline void buildUpdateChains(const FlatModel& flat, SimState& sim) {
    std::unordered_map<Entity*, size_t> chainOf;
    for (Action* a : flat.actions) {
        if (a->kind() != Action::Kind::Update) continue;
        auto [it, inserted] = chainOf.emplace(a->target(), sim.chains.size());
        if (inserted) sim.chains.emplace_back();
        sim.chains[it->second].updates.push_back(a);
    }
}

}  // namespace wolvicmod::detail
