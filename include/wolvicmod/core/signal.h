#pragma once

#include <concepts>
#include <optional>
#include <type_traits>

#include "wolvicmod/core/entity.h"
#include "wolvicmod/wave/format.h"

namespace wolvicmod {

// Combinational signal base (§2.3). In/Out/Wire share value storage; the kind
// distinguishes their structural roles.
template <class T>
class Signal : public Entity {
public:
    using Value = T;

    // Readable protocol: readValue() feeds read sets (const ref, §3.1).
    const T& readValue() const {
        detail::auditTouch(this);
        return effective();
    }

    // External observation (§3.5).
    const T& get() const { return readValue(); }

    // Fast-path Assign registration (§3.1): constant / identity / expression.
    // Defined in core/action.h (expression support in core/expr.h).
    template <class Src>
    void operator=(Src&& src);

    // Subscript expression for array-valued signals (§3.1): returns an
    // expression node (not a value) — only usable in assign/update registration.
    // Defined in core/expr.h.
    auto operator[](size_t i)
        requires requires(const T& t, size_t n) { t[n]; };

    // Full-form Assign registration: target.assign().reads(...) = lambda.
    // Defined in core/action.h.
    template <int Dummy = 0>
    auto assign();

    // --- internal ---
    T& mutableValue() { return alias_ != nullptr ? *alias_ : value_; }

    // Identity-connect aliasing (§5.2): valueStorage() is the effective value
    // address (the canonical source's own storage); aliasStorageTo() redirects
    // this signal's reads/writes to the canonical source's storage.
    void* valueStorage() override { return alias_ != nullptr ? static_cast<void*>(alias_) : static_cast<void*>(&value_); }
    void aliasStorageTo(void* p) override { alias_ = static_cast<T*>(p); }

    bool snapshotSupported() const override { return kSnapshotable; }
    void snapshotSave() override {
        if constexpr (kSnapshotable) snap_.emplace(effective());
    }
    bool snapshotChanged() const override {
        if constexpr (kSnapshotable) return !snap_.has_value() || !(*snap_ == effective());
        else return true;
    }

    bool waveSupported() const override { return FstFormattable<T>; }
    uint32_t waveBits() const override {
        if constexpr (FstFormattable<T>) return FstFormat<T>::bits;
        else return 0;
    }
    void waveFormat(std::string& out) const override {
        if constexpr (FstFormattable<T>) FstFormat<T>::format(out, effective());
    }

protected:
    explicit Signal(EntityKind k) : Entity(k) {}

private:
    static constexpr bool kSnapshotable =
        requires(const T& x, const T& y) {
            { x == y } -> std::convertible_to<bool>;
        } && std::copy_constructible<T>;

    // Effective value: the alias target when storage-aliased (elaboration),
    // otherwise the inline cell. The alias_ test adds no dependent load on
    // the non-aliased path (value_ sits at a fixed offset).
    const T& effective() const { return alias_ != nullptr ? *alias_ : value_; }

    T value_{};
    T* alias_ = nullptr;
    std::optional<T> snap_;
};

template <class T>
class In : public Signal<T> {
public:
    In() : Signal<T>(EntityKind::In) {}

    using Signal<T>::operator=;
    // Same-type connection (e.g. sr.clk = clk): registers an identity Assign.
    // Defined in core/action.h.
    void operator=(const In& src);

    // External drive, root-module inputs only (§3.5, §4.2). Defined in module.h.
    void set(const T& v);
};

template <class T>
class Out : public Signal<T> {
public:
    Out() : Signal<T>(EntityKind::Out) {}
    using Signal<T>::operator=;
    void operator=(const Out& src);
};

template <class T>
class Wire : public Signal<T> {
public:
    Wire() : Signal<T>(EntityKind::Wire) {}
    using Signal<T>::operator=;
    void operator=(const Wire& src);
};

}  // namespace wolvicmod
