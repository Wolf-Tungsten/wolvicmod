#pragma once

#include <utility>

#include "wolvicmod/core/entity.h"
#include "wolvicmod/wave/format.h"

namespace wolvicmod {

// Single-value sequential state (§2.4). Next-state intents from Updates are
// collected in next_ during the combinational phase and committed (NBA) in the
// state-update phase (§5.2).
template <class T>
class Reg : public Entity {
public:
    using Value = T;

    Reg() : Entity(EntityKind::Reg) {}

    const T& readValue() const {
        detail::auditTouch(this);
        return cur_;
    }
    const T& get() const { return readValue(); }

    bool waveSupported() const override { return FstFormattable<T>; }
    uint32_t waveBits() const override {
        if constexpr (FstFormattable<T>) return FstFormat<T>::bits;
        else return 0;
    }
    void waveFormat(std::string& out) const override {
        if constexpr (FstFormattable<T>) FstFormat<T>::format(out, cur_);
    }

    // Full-form Update registration:
    //   reg.update().on(e1, ...).en(g).reads(s1, ...) = lambda   (§3.2)
    // Defined in core/action.h.
    template <int Dummy = 0>
    auto update();

    // --- internal ---
    T& nextSlot() { return next_; }
    void commitNext() { cur_ = std::move(next_); }

private:
    T cur_{};
    T next_{};
};

}  // namespace wolvicmod
