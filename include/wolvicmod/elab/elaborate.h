#pragma once

#include <algorithm>
#include <cstdlib>
#include <memory>

#include "wolvicmod/core/action.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/elab/flatten.h"
#include "wolvicmod/elab/graph.h"
#include "wolvicmod/sim/errors.h"

namespace wolvicmod {

// Defined here: the destructor needs Action and SimState complete.
inline Module::~Module() {
    auto& s = detail::ctxStack();
    s.erase(std::remove(s.begin(), s.end(), this), s.end());
}

// Elaboration (§4): flatten -> structural checks -> build the simulation
// graphs. Runs once, after the root constructor returns; the structure is
// frozen afterwards.
inline void Module::elaborate(std::string_view topName) {
    if (parent_ != nullptr)
        detail::fail("elaborate() must be called on the root module ('" + hierPath() + "')");
    if (sim_ != nullptr) detail::fail("elaborate() called twice on '" + hierPath() + "'");

    setRootName(topName);

    detail::FlatModel flat;
    detail::flattenInto(this, flat);
    for (uint32_t i = 0; i < flat.entities.size(); ++i) flat.entities[i]->setFlatIndex(i);
    for (uint32_t i = 0; i < flat.actions.size(); ++i) flat.actions[i]->setRegIndex(i);

    detail::checkSignalDrivers(*this, flat);

    // Edge-detection history starts at the signals' initial values: no
    // spurious edges (§5.3).
    for (Action* a : flat.actions) a->initEventPrev();

    auto sim = std::make_unique<detail::SimState>();
    detail::buildExecOrder(flat, *sim);
    detail::buildUpdateChains(flat, *sim);

    // Bind dirty-driven scheduling state (§5.2 fast path): entity slots point
    // into sim->dirtyGens (allocated once, never reallocated); each action's
    // read flat indices concatenate into sim->readIdxPool.
    sim->dirtyGens.assign(flat.entities.size(), 1);
    for (uint32_t i = 0; i < flat.entities.size(); ++i)
        flat.entities[i]->bindDirty(&sim->dirtyGens[i], &sim->clock);
    for (Action* a : flat.actions) {
        const uint32_t off = static_cast<uint32_t>(sim->readIdxPool.size());
        for (const Entity* r : a->reads()) sim->readIdxPool.push_back(r->flatIndex());
        a->bindReadIdx(off, static_cast<uint32_t>(sim->readIdxPool.size()) - off);
    }

    // Scheduling-mode escape hatch: WOLVICMOD_DIRTY_EVAL=0 forces full
    // re-evaluation; =poll selects the legacy dirty-generation scan (A/B).
    if (const char* e = std::getenv("WOLVICMOD_DIRTY_EVAL")) {
        if (e[0] == '0')
            sim->schedMode = detail::SchedMode::Full;
        else if (std::string_view(e) == "poll")
            sim->schedMode = detail::SchedMode::Poll;
    }

    // First eval() runs everything (the push-path equivalent of poll mode's
    // never-run rule, lastRunGen == 0).
    for (uint32_t pos = 0; pos < sim->execOrder.size(); ++pos) sim->readyQ.push(pos);

    sim_ = std::move(sim);

    detail::ctxStack().clear();
}

}  // namespace wolvicmod
