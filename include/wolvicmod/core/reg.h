#pragma once

#include <type_traits>
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

    // Identity-connect aliasing (§5.2): a Reg can be the alias source; readers
    // bind to cur_'s storage. commitNext() writes cur_ in place, so the bound
    // address stays valid across commits.
    void* valueStorage() override { return &cur_; }
    void* nextStorage() override { return &next_; }
    bool bitwiseEqOk() const override { return std::has_unique_object_representations_v<T>; }
    bool isBoolReg() const override { return std::is_same_v<T, bool>; }
    void commitNext() {
        // Change detection (§5.2 fast path): a commit that lands the current
        // value dirties nothing, so downstream logic stays asleep. Bool
        // commits report the transition direction for edge-direction-
        // filtered dispatch.
        if constexpr (kEqualityComparable<T>) {
            if (cur_ == next_) return;
        }
        cur_ = std::move(next_);
        if constexpr (std::is_same_v<T, bool>)
            markDirtyBool(cur_);
        else
            markDirty();
    }

private:
    T cur_{};
    T next_{};
};

}  // namespace wolvicmod
