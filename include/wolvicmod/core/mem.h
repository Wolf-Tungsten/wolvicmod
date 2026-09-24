#pragma once

#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>

#include "wolvicmod/core/entity.h"

namespace wolvicmod {

// Row-addressed state array (§2.4): R rows of T. Reads give committed content;
// a write touches one row at the commit point, untouched rows hold, and commit
// cost is independent of capacity.
template <class T, size_t R>
class Mem : public Entity {
public:
    using Value = T;
    static constexpr size_t kRows = R;

    Mem() : Entity(EntityKind::Mem) {}

    // A Mem enters read sets as itself: reads(mem) yields `const Mem&` in the
    // lambda and row access is m[a] (§3.4).
    const Mem& readValue() const {
        detail::auditTouch(this);
        return *this;
    }
    const T& operator[](size_t row) const {
        detail::auditTouch(this);
        return rows_[row];
    }
    const T& get(size_t row) const { return (*this)[row]; }

    // Full-form Update registration:
    //   mem.update().on(e1, ...).addr(a).en(g).reads(s1, ...) = lambda   (§3.4)
    // Defined in core/action.h.
    template <int Dummy = 0>
    auto update();

    // --- internal ---
    void writeRow(size_t row, T&& v) { rows_[row] = std::move(v); }

private:
    std::array<T, R> rows_{};
};

template <class E>
struct IsMem : std::false_type {};
template <class T, size_t R>
struct IsMem<Mem<T, R>> : std::true_type {};

}  // namespace wolvicmod
