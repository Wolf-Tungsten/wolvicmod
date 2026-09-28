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

// Dirty-driven skip rule (§5.2 fast path): run the action when it never ran,
// or when one of its reads changed after its previous run. markRan stamps the
// post-run clock, which is >= the dirty generation of everything the run
// consumed (topological order guarantees producers ran earlier in the round).
inline bool actionDirty(const SimState& sim, const Action* a) {
    if (a->lastRunGen() == 0) return true;
    const uint32_t off = a->readIdxOff(), n = a->readIdxN();
    for (uint32_t i = 0; i < n; ++i)
        if (sim.dirtyGens[sim.readIdxPool[off + i]] > a->lastRunGen()) return true;
    return false;
}

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
//
// Fast path (dirty-driven): an action runs only when one of its reads changed
// since its previous run; unchanged values do not propagate (change detection
// at Assign stores, Reg commits, Mem row writes and external input drives).
// The fixpoint reached is identical to full re-evaluation: a skipped action
// is a pure function of reads that have not changed, so its output and side
// effects (edge history, intent flag) are unchanged as well. Trace mode
// forces full evaluation so every action's trace state stays current.
inline void Module::eval() {
    if (sim_ == nullptr) detail::fail("eval(): call elaborate() first ('" + hierPath() + "')");
    auto& sim = *sim_;
    const bool skipQuiescent = sim.dirtyEval && sim.trace == nullptr;

    std::vector<const Entity*> committed;
    for (size_t round = 0;; ++round) {
        if (sim.trace != nullptr) sim.trace->onRound(sim.evalCount, round);

        // Phase 1: combinational evaluation.
        for (auto& item : sim.execOrder) {
            if (item.single != nullptr) {
                Action* a = item.single;
                if (skipQuiescent && !detail::actionDirty(sim, a)) continue;
                if (sim.auditReads)
                    detail::auditRun(a);
                else
                    a->run();
                a->markRan(sim.clock);
            } else {
                auto& g = *sim.groups[item.group];
                if (skipQuiescent) {
                    bool anyDirty = false;
                    for (Action* a : g.actions)
                        if (detail::actionDirty(sim, a)) {
                            anyDirty = true;
                            break;
                        }
                    if (!anyDirty) continue;
                }
                detail::runSccGroup(g, sim.sccIterCap, sim.auditReads);
                for (Action* a : g.actions) a->markRan(sim.clock);
                // No extra dirty marking needed: AssignAction::run() already
                // dirties every member signal whose value actually changed.
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
            for (Action* a : chain.updates) a->clearIntent();
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

// §5.2: dirty-driven scheduling on (default) / off (full re-evaluation).
inline void Module::dirtyEvalOn() {
    if (sim_ == nullptr) detail::fail("dirtyEvalOn(): call elaborate() first ('" + hierPath() + "')");
    sim_->dirtyEval = true;
}

inline void Module::dirtyEvalOff() {
    if (sim_ == nullptr) detail::fail("dirtyEvalOff(): call elaborate() first ('" + hierPath() + "')");
    sim_->dirtyEval = false;
}

}  // namespace wolvicmod
