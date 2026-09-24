#pragma once

#include <algorithm>
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
    sim_ = std::move(sim);

    detail::ctxStack().clear();
}

}  // namespace wolvicmod
