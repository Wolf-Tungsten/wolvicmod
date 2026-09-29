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

// Type-agnostic byte ops for the phase-2 fast commit (§5.2): fixed small
// sizes compile to single moves; anything larger reaches memcpy/memcmp — one
// shared call target for every chain and value type, so the branch predictor
// never sees a diverse indirect target on the commit path.
inline void bytesCopy(char* d, const char* s, uint32_t n) {
    switch (n) {
        case 1: d[0] = s[0]; return;
        case 2: __builtin_memcpy(d, s, 2); return;
        case 4: __builtin_memcpy(d, s, 4); return;
        case 8: __builtin_memcpy(d, s, 8); return;
        default: __builtin_memcpy(d, s, n);
    }
}
inline bool bytesEqual(const char* a, const char* b, uint32_t n) {
    switch (n) {
        case 1: return a[0] == b[0];
        case 2: return __builtin_memcmp(a, b, 2) == 0;
        case 4: return __builtin_memcmp(a, b, 4) == 0;
        case 8: return __builtin_memcmp(a, b, 8) == 0;
        default: return __builtin_memcmp(a, b, n) == 0;
    }
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
// Dirty-driven (§5.2): value changes set one bit per dependent in the
// activity bitmap; phase 1 scans the bitmap in execOrder order (=
// topological order), so downstream dirtied during the scan is picked up by
// the same pass. The bit OR is idempotent — dedup is free — and the write
// site is a plain array store, no queue bookkeeping. Bool write sites report
// the transition direction, so Updates watching only the opposite edge are
// not even activated (their edge history is advanced at the write site).
// Phase 2 is bitmap-driven too: a rising intent flag sets its priority
// chain's bit, so the state-update phase visits only committing chains.
// execOrder itself is emitted type-greedy (buildExecOrder), so consecutive
// dispatches usually share one vtable target. The first round after
// elaboration has every bit set.
// Trace mode forces a full sweep so every action's trace state stays current.
inline void Module::eval() {
    if (sim_ == nullptr) detail::fail("eval(): call elaborate() first ('" + hierPath() + "')");
    auto& sim = *sim_;
    const bool fullSweep = sim.trace != nullptr;

    for (size_t round = 0;; ++round) {
        if (sim.trace != nullptr) sim.trace->onRound(sim.evalCount, round);

        // Phase 1: combinational evaluation.
        if (!fullSweep) {
            for (size_t w = 0; w < sim.actBits.size(); ++w) {
                while (sim.actBits[w] != 0) {
                    const uint32_t pos = static_cast<uint32_t>(w * 64) +
                                         static_cast<uint32_t>(__builtin_ctzll(sim.actBits[w]));
                    sim.actBits[w] &= sim.actBits[w] - 1;
                    const auto& item = sim.execOrder[pos];
                    if (item.single != nullptr) {
                        Action* a = item.single;
                        if (sim.auditReads)
                            detail::auditRun(a);
                        else
                            a->run();
                    } else {
                        auto& g = *sim.groups[item.group];
                        detail::runSccGroup(g, sim.sccIterCap, sim.auditReads);
                        // No extra dirty marking needed: AssignAction::run()
                        // already dirties every member signal whose value
                        // actually changed.
                    }
                }
            }
        } else {
            for (auto& item : sim.execOrder) {
                if (item.single != nullptr) {
                    Action* a = item.single;
                    if (sim.auditReads)
                        detail::auditRun(a);
                    else
                        a->run();
                } else {
                    auto& g = *sim.groups[item.group];
                    detail::runSccGroup(g, sim.sccIterCap, sim.auditReads);
                }
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

        // Phase 2: state update — only chains whose Updates raised an intent
        // (bitmap bit or queue entry set when the flag went active), in
        // chain-registration order; within a chain later-registered first so
        // the earliest-registered (highest priority) lands last and wins.
        sim.committed.clear();
        sim.committedN = 0;
        const auto commitChain = [&](uint32_t ci) {
            auto& chain = sim.chains[ci];
            if (chain.reg != nullptr) {
                // Fast commit: apply active intents by byte copy (reverse
                // registration order, so the highest priority lands last),
                // then commit cur <- next with an inline byte compare — no
                // per-member virtual calls on this chain.
                if (chain.memberCommit) {
                    // Member-bitmap commit (§5.2): member intent state lives
                    // in the chain descriptor — no member Action is
                    // dereferenced here at all. Descending bit index is
                    // reverse registration order, so the highest-priority
                    // (earliest-registered) intent is applied last and wins.
                    uint64_t mb = chain.memberBits;
                    if (mb == 0) return;  // intent settled back during iteration
                    if (sim.assertUpdateMutex && __builtin_popcountll(mb) > 1)
                        detail::fail("Update mutex assertion (§6.4): " +
                                     std::to_string(__builtin_popcountll(mb)) +
                                     " Updates activated on '" + chain.reg->hierPath() +
                                     "' in one round (registration-order priority would apply; "
                                     "this is only an error because assertUpdateMutexOn() is set)");
                    while (mb != 0) {
                        const uint32_t i =
                            63u - static_cast<uint32_t>(__builtin_clzll(mb));
                        mb &= ~(uint64_t{1} << i);
                        detail::bytesCopy(chain.next, chain.intentSrcs[i], chain.size);
                    }
                    chain.memberBits = 0;
                } else {
                    size_t activeCount = 0;
                    for (Action* a : chain.updates) activeCount += a->intentActive() ? 1 : 0;
                    if (activeCount == 0) return;  // intent settled back during iteration
                    if (sim.assertUpdateMutex && activeCount > 1)
                        detail::fail("Update mutex assertion (§6.4): " +
                                     std::to_string(activeCount) + " Updates activated on '" +
                                     chain.reg->hierPath() +
                                     "' in one round (registration-order priority would apply; "
                                     "this is only an error because assertUpdateMutexOn() is set)");
                    for (auto it = chain.updates.rbegin(); it != chain.updates.rend(); ++it) {
                        Action* a = *it;
                        if (a->intentActive())
                            detail::bytesCopy(chain.next, a->fastIntentSrc(), chain.size);
                    }
                    for (Action* a : chain.updates) a->clearIntent();
                }
                if (chain.bitwiseEq) {
                    // Exact byte compare (no padding bits): a byte-equal
                    // commit dirties nothing, matching commitNext's ==
                    // short-circuit; a bool commit reports the direction.
                    if (!detail::bytesEqual(chain.cur, chain.next, chain.size)) {
                        detail::bytesCopy(chain.cur, chain.next, chain.size);
                        if (chain.isBool)
                            chain.reg->markDirtyBool(chain.next[0] != 0);
                        else
                            chain.reg->markDirty();
                    }
                } else {
                    chain.typedCommit(chain.reg);
                }
                ++sim.committedN;
                if (sim.committed.size() < 64) sim.committed.push_back(chain.reg);
                if (sim.trace != nullptr) sim.trace->onCommit(chain.reg);
                return;
            }
            size_t activeCount = 0;
            for (Action* a : chain.updates) activeCount += a->intentActive() ? 1 : 0;
            if (activeCount == 0) return;  // intent settled back during iteration
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
            ++sim.committedN;
            if (sim.committed.size() < 64) sim.committed.push_back(tgt);
            if (sim.trace != nullptr) sim.trace->onCommit(tgt);
        };
        // Ascending bit order == chain-registration order, so round traces
        // keep a deterministic commit order.
        for (size_t w = 0; w < sim.chainBits.size(); ++w) {
            while (sim.chainBits[w] != 0) {
                const uint32_t ci = static_cast<uint32_t>(w * 64) +
                                    static_cast<uint32_t>(__builtin_ctzll(sim.chainBits[w]));
                sim.chainBits[w] &= sim.chainBits[w] - 1;
                commitChain(ci);
            }
        }
        if (sim.committedN == 0) {
            ++sim.evalCount;
            return;
        }

        if (round + 1 >= sim.roundCap) {
            std::string msg = "eval() did not converge after " + std::to_string(sim.roundCap) +
                              " rounds (zero-delay oscillation, §5.3); states updated every round:";
            for (const Entity* e : sim.committed) msg += "\n  " + e->hierPath();
            if (sim.committedN > sim.committed.size())
                msg += "\n  ... (" + std::to_string(sim.committedN - sim.committed.size()) + " more)";
            detail::fail(msg);
        }
    }
}

}  // namespace wolvicmod
