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
    for (auto& a : m->actions()) out.actions.push_back(a);
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

// Identity-connect aliasing (§5.2): same-type identity assigns (b = a fast
// path, tagged via Action::aliasSource) are eliminated — the target's value
// storage aliases the source's, and consumers rewire to the canonical entity
// when the evaluation graph is built (buildExecOrder takes the canon map).
// Union-find over flat entity indices with the source side as the canonical
// root. An alias cycle (a = b; b = a) has no ultimate source: the closing
// pair keeps its copy action (semantics unchanged, it degenerates to a
// self-copy through the aliased storage).
inline void resolveAliases(const FlatModel& flat, std::vector<uint32_t>& canon) {
    const uint32_t E = static_cast<uint32_t>(flat.entities.size());
    canon.resize(E);
    for (uint32_t i = 0; i < E; ++i) canon[i] = i;
    auto find = [&](uint32_t x) {
        uint32_t r = x;
        while (canon[r] != r) r = canon[r];
        while (canon[x] != r) {
            const uint32_t p = canon[x];
            canon[x] = r;
            x = p;
        }
        return r;
    };
    for (Action* a : flat.actions) {
        const Entity* src = a->aliasSource();
        if (src == nullptr) continue;
        const uint32_t rt = find(a->target()->flatIndex());
        const uint32_t rs = find(src->flatIndex());
        if (rt == rs) continue;  // alias cycle: keep the copy action
        if (flat.entities[rs]->valueStorage() == nullptr) continue;  // no aliasable storage
        canon[rt] = rs;
        a->eliminate();
    }
    // Bind aliased storage after the map is final: canonical roots are never
    // aliased, so their valueStorage() is their own inline cell.
    for (Action* a : flat.actions) {
        if (!a->eliminated()) continue;
        Entity* t = a->target();
        t->aliasStorageTo(flat.entities[find(t->flatIndex())]->valueStorage());
    }
}

}  // namespace wolvicmod::detail
