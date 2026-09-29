#pragma once

// Activation statistics (diagnostic): enabled by WV_STATS=1 in the
// environment. One predictable branch per hook when disabled. Counters are
// process-global (the engine is single-instance per process in every current
// use), printed to stderr at exit.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace wolvicmod::detail {

struct SimStats {
    uint64_t evals = 0;
    uint64_t rounds = 0;
    uint64_t assignRuns = 0;
    uint64_t updateRuns = 0;
    uint64_t updEdgeHit = 0;        // edge seen, guard not consulted yet
    uint64_t updEdgeGuardTrue = 0;  // edge + guard -> intent computed
    uint64_t updNoEdge = 0;         // activated by read-set dirty, no edge
    uint64_t commits = 0;           // phase-2 chain commits
};

inline SimStats& simStats() {
    static SimStats s;
    return s;
}

inline bool statsEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("WV_STATS");
        if (v == nullptr || v[0] == '\0' || v[0] == '0') return false;
        std::atexit([] {
            const SimStats& s = simStats();
            std::fprintf(stderr,
                         "[wv-stats] evals=%lu rounds=%lu assignRuns=%lu updateRuns=%lu\n"
                         "[wv-stats]   upd: edge+guard=%lu edgeOnly=%lu noEdge=%lu\n"
                         "[wv-stats]   commits=%lu  (runs/eval: assign=%.1f update=%.1f)\n",
                         (unsigned long)s.evals, (unsigned long)s.rounds,
                         (unsigned long)s.assignRuns, (unsigned long)s.updateRuns,
                         (unsigned long)s.updEdgeGuardTrue,
                         (unsigned long)(s.updEdgeHit - s.updEdgeGuardTrue),
                         (unsigned long)s.updNoEdge, (unsigned long)s.commits,
                         s.evals ? double(s.assignRuns) / s.evals : 0.0,
                         s.evals ? double(s.updateRuns) / s.evals : 0.0);
        });
        return true;
    }();
    return on;
}

inline void statEval() {
    if (statsEnabled()) ++simStats().evals;
}
inline void statRound() {
    if (statsEnabled()) ++simStats().rounds;
}
inline void statAssignRun() {
    if (statsEnabled()) ++simStats().assignRuns;
}
inline void statUpdateRun(bool edge, bool guardPass) {
    if (statsEnabled()) {
        ++simStats().updateRuns;
        if (edge) {
            ++simStats().updEdgeHit;
            if (guardPass) ++simStats().updEdgeGuardTrue;
        } else {
            ++simStats().updNoEdge;
        }
    }
}
inline void statCommit() {
    if (statsEnabled()) ++simStats().commits;
}

}  // namespace wolvicmod::detail
