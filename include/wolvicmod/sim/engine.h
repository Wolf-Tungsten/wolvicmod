#pragma once

#include <string>
#include <vector>

#include "wolvicmod/core/action.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/dbg/audit.h"
#include "wolvicmod/elab/graph.h"
#include "wolvicmod/sim/errors.h"

namespace wolvicmod {

namespace detail {

// Iterate a combinational cycle to its steady state (§4.3).
inline void runSccGroup(SimState::SccGroup& g, size_t cap, bool audit) {
    for (size_t iter = 0;; ++iter) {
        for (Entity* s : g.signals) s->snapshotSave();
        for (Action* a : g.actions) {
            if (audit)
                auditRun(a);
            else
                a->run();
        }
        bool changed = false;
        for (Entity* s : g.signals) changed |= s->snapshotChanged();
        if (!changed) return;
        if (iter + 1 >= cap) {
            std::string msg = "combinational cycle did not converge after " +
                              std::to_string(cap) + " iterations (§4.3), cycle:";
            for (Entity* s : g.signals) msg += "\n  " + s->hierPath();
            fail(msg);
        }
    }
}

}  // namespace detail

// One eval() = rounds of two phases (§5.2): combinational evaluation in
// topological order (Updates only record intents), then state update applying
// intents per priority chain with all writes taking effect together (NBA).
// Loops until a round commits nothing; eval() is idempotent (§5.1).
inline void Module::eval() {
    if (sim_ == nullptr) detail::fail("eval(): call elaborate() first ('" + hierPath() + "')");
    auto& sim = *sim_;

    std::vector<const Entity*> committed;
    for (size_t round = 0;; ++round) {
        if (sim.trace != nullptr) sim.trace->onRound(sim.evalCount, round);

        // Phase 1: combinational evaluation.
        for (auto& item : sim.execOrder) {
            if (item.single != nullptr) {
                if (sim.auditReads)
                    detail::auditRun(item.single);
                else
                    item.single->run();
            } else {
                detail::runSccGroup(*sim.groups[item.group], sim.sccIterCap, sim.auditReads);
            }
        }

        // Round trace (§6.3): edge hits and activated Updates of this round.
        if (sim.trace != nullptr) {
            for (auto& chain : sim.chains)
                for (Action* a : chain.updates) {
                    if (a->edgeSeen()) sim.trace->onEdge(a->edgeDesc());
                    if (a->intentActive()) sim.trace->onActivate(a);
                }
        }

        // Phase 2: state update — priority chains, later-registered first so
        // the earliest-registered (highest priority) lands last and wins.
        committed.clear();
        for (auto& chain : sim.chains) {
            size_t activeCount = 0;
            for (Action* a : chain.updates) activeCount += a->intentActive() ? 1 : 0;
            if (activeCount == 0) continue;
            if (sim.assertUpdateMutex && activeCount > 1)
                detail::fail("Update mutex assertion (§6.4): " +
                             std::to_string(activeCount) + " Updates activated on '" +
                             chain.updates.front()->target()->hierPath() +
                             "' in one round (registration-order priority would apply; "
                             "this is only an error because assertUpdateMutexOn() is set)");
            for (auto it = chain.updates.rbegin(); it != chain.updates.rend(); ++it)
                if ((*it)->intentActive()) (*it)->applyIntent();
            chain.updates.front()->finalizeTarget();
            const Entity* tgt = chain.updates.front()->target();
            committed.push_back(tgt);
            if (sim.trace != nullptr) sim.trace->onCommit(tgt);
        }
        if (committed.empty()) {
            ++sim.evalCount;
            return;
        }

        if (round + 1 >= sim.roundCap) {
            std::string msg = "eval() did not converge after " + std::to_string(sim.roundCap) +
                              " rounds (zero-delay oscillation, §5.3); states updated every round:";
            for (const Entity* e : committed) msg += "\n  " + e->hierPath();
            detail::fail(msg);
        }
    }
}

}  // namespace wolvicmod
