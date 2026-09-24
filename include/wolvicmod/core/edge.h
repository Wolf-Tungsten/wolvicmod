#pragma once

#include <concepts>
#include <functional>

#include "wolvicmod/core/entity.h"
#include "wolvicmod/core/signal.h"

namespace wolvicmod {

// Readable entity holding a bool value: In/Out/Wire of bool, or Reg<bool>
// (a divider output, §3.2) — clocks have no special status.
template <class E>
concept BoolReadable =
    std::derived_from<E, Entity> && requires(const E& e) {
        { e.readValue() } -> std::same_as<const bool&>;
    };

enum class EdgeKind : uint8_t { Posedge, Negedge };

struct EdgeEvent {
    Entity* sig;
    EdgeKind kind;
    std::function<bool()> read;  // current value of the event signal
};

template <BoolReadable E>
EdgeEvent posedge(E& s) {
    return {&s, EdgeKind::Posedge, [&s] { return static_cast<bool>(s.readValue()); }};
}

template <BoolReadable E>
EdgeEvent negedge(E& s) {
    return {&s, EdgeKind::Negedge, [&s] { return static_cast<bool>(s.readValue()); }};
}

}  // namespace wolvicmod
