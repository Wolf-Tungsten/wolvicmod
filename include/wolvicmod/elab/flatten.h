#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "wolvicmod/core/action.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/sim/errors.h"

namespace wolvicmod::detail {

// Flattened model (§4.2): the module tree dissolved into flat entity and
// action sets. References were pointers from the start, so flattening is pure
// collection — hierarchical names are kept for diagnostics and waveforms.
struct FlatModel {
    std::vector<Entity*> entities;  // DFS pre-order over the module tree
    std::vector<Action*> actions;   // same order; defines registration order
};

inline void flattenInto(Module* m, FlatModel& out) {
    for (auto& e : m->entities()) out.entities.push_back(e.get());
    for (auto& a : m->actions()) out.actions.push_back(a.get());
    for (auto& c : m->children()) flattenInto(c.get(), out);
}

// Structural legality on the flat set (§4.2):
// - every signal has exactly one driving Assign (Wire SSA; ports alike);
// - root-module inputs are the only driverless signals allowed (external);
// - Reg/Mem are not checked (Updates are naturally multiple, §2.4).
inline void checkSignalDrivers(const Module& root, const FlatModel& flat) {
    std::unordered_map<const Entity*, std::vector<const Action*>> drivers;
    for (const Action* a : flat.actions)
        if (a->kind() == Action::Kind::Assign) drivers[a->target()].push_back(a);

    for (const Entity* e : flat.entities) {
        const auto k = e->kind();
        if (k != EntityKind::In && k != EntityKind::Out && k != EntityKind::Wire) continue;
        const auto it = drivers.find(e);
        const size_t n = (it == drivers.end()) ? 0 : it->second.size();
        const bool isRootIn = (k == EntityKind::In) && (e->owner() == &root);
        if (isRootIn) {
            if (n > 0)
                fail("root input '" + e->hierPath() +
                     "' is driven by an assign; root inputs are driven externally "
                     "via set() (§4.2)");
        } else if (n == 0) {
            fail("signal '" + e->hierPath() + "' has no driver (§4.2)");
        } else if (n > 1) {
            std::string msg = "multiple drivers of '" + e->hierPath() + "' (§4.2):";
            for (const Action* a : it->second)
                msg += "\n  assign registered in module '" + a->ctx()->hierPath() + "'";
            fail(msg);
        }
    }
}

}  // namespace wolvicmod::detail
