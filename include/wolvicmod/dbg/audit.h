#pragma once

#include <string>
#include <unordered_set>

#include "wolvicmod/core/action.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/elab/graph.h"
#include "wolvicmod/sim/errors.h"

namespace wolvicmod::detail {

// Read-set accounting (§6.4): run one action while recording every entity
// read through the framework's access paths, then compare against the
// declared read set. A read outside the declaration is a stealth read and
// fails at the first simulation.
inline void auditRun(Action* a) {
    AuditFrame frame{a, {}};
    AuditFrame* prev = auditFrame();
    auditFrame() = &frame;
    a->run();
    auditFrame() = prev;

    if (frame.accessed.empty()) return;
    const std::unordered_set<const Entity*> declared(a->reads().begin(), a->reads().end());
    std::unordered_set<const Entity*> reported;
    std::string stolen;
    for (const Entity* e : frame.accessed)
        if (!declared.count(e) && reported.insert(e).second) stolen += "\n  " + e->hierPath();
    if (!stolen.empty())
        fail("stealth read (§6.4): " + a->describe() + " reads undeclared entities:" + stolen);
}

}  // namespace wolvicmod::detail

namespace wolvicmod {

// §6.4 debug switches. auditOn enables read-set accounting (requires a build
// with WOLVICMOD_AUDIT); assertUpdateMutexOn reports multiple Updates of one
// state activating in the same round (off by default — registration-order
// priority then applies silently, §2.4).
inline void Module::auditOn() {
    if (sim_ == nullptr) detail::fail("auditOn(): call elaborate() first ('" + hierPath() + "')");
#ifndef WOLVICMOD_AUDIT
    detail::fail("auditOn(): this build has no audit instrumentation; rebuild with "
                 "WOLVICMOD_AUDIT=ON (§6.4)");
#endif
    sim_->auditReads = true;
}

inline void Module::auditOff() {
    if (sim_ != nullptr) sim_->auditReads = false;
}

inline void Module::assertUpdateMutexOn() {
    if (sim_ == nullptr)
        detail::fail("assertUpdateMutexOn(): call elaborate() first ('" + hierPath() + "')");
    sim_->assertUpdateMutex = true;
}

inline void Module::assertUpdateMutexOff() {
    if (sim_ != nullptr) sim_->assertUpdateMutex = false;
}

}  // namespace wolvicmod
