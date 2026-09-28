#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "wolvicmod/core/readyq.h"

namespace wolvicmod {

class Module;
class Action;
class Entity;

namespace detail {

// Read-set accounting frame (§6.4): while an action runs under audit, every
// entity read through the framework's access paths is recorded here and later
// compared against the declared read set. Zero-cost when WOLVICMOD_AUDIT is
// not defined (the call inlines to nothing and no frame is ever installed).
struct AuditFrame {
    const void* action;
    std::vector<const Entity*> accessed;
};

inline AuditFrame*& auditFrame() {
    static thread_local AuditFrame* f = nullptr;
    return f;
}

inline void auditTouch(const Entity* e) {
#ifdef WOLVICMOD_AUDIT
    if (AuditFrame* f = auditFrame()) f->accessed.push_back(e);
#else
    (void)e;
#endif
}

}  // namespace detail

enum class EntityKind : uint8_t { In, Out, Wire, Reg, Mem };

// Value types usable in dirty-change detection (§5.2): equality-comparable
// values let the engine skip downstream recomputation when a write lands the
// same value; non-comparable types conservatively always propagate.
template <class T>
inline constexpr bool kEqualityComparable =
    requires(const T& a, const T& b) {
        { a == b } -> std::convertible_to<bool>;
    };

// Base of all structural entities (§2.1). An entity is created and owned by a
// Module via its createXXX methods; attaching gives it a name (§6.1).
class Entity {
public:
    Entity(const Entity&) = delete;
    Entity& operator=(const Entity&) = delete;
    virtual ~Entity() = default;

    EntityKind kind() const { return kind_; }

    // Attached through a createXXX method (named) or a bare member (unnamed,
    // rejected at action registration / elaboration, §6.2).
    bool named() const { return owner_ != nullptr; }
    const std::string& name() const { return name_; }
    Module* owner() const { return owner_; }

    // Hierarchical path like "top.sr.q" (§6.1). Defined in module.h.
    std::string hierPath() const;

    // Flat index in the elaborated entity space. Assigned at elaboration.
    uint32_t flatIndex() const { return flatIndex_; }

    // Steady-state detection for combinational cycles (§4.3): supported when
    // the value type is equality-comparable and copyable (Signal overrides).
    virtual bool snapshotSupported() const { return false; }
    virtual void snapshotSave() {}
    virtual bool snapshotChanged() const { return true; }

    // Waveform dump (§6.3): value -> bit string. Implemented by Signal/Reg
    // when the value type has an FstFormat specialization; everything else
    // (including Mem) is skipped in wave dumps.
    virtual bool waveSupported() const { return false; }
    virtual uint32_t waveBits() const { return 0; }
    virtual void waveFormat(std::string& out) const { (void)out; }

    // --- internal ---
    void attach(Module* owner, std::string_view name);  // defined in module.h
    void setFlatIndex(uint32_t i) { flatIndex_ = i; }

    // Dirty-driven scheduling (§5.2 fast path): dirtyGen records the dirty
    // clock value of the last value change (legacy poll path); the push path
    // notifies dependent actions through the reverse dependency map.
    // Slots are bound at elaboration into SimState's flat arrays; before
    // elaboration marks are no-ops.
    uint64_t dirtyGen() const { return (dirtySlot_ != nullptr) ? *dirtySlot_ : 1; }
    void markDirty() {
        if (dirtySlot_ != nullptr) {
            *dirtySlot_ = ++(*dirtyClock_);
            // Push notification happens in every scheduling mode: pushes are
            // deduped and a superset drain is harmless (actions are pure
            // functions of their reads), so switching modes mid-run never
            // needs a queue reset.
            if (depN_ != 0) readyQ_->pushDeps(deps_, depN_);
        }
    }
    void bindDirty(uint64_t* slot, uint64_t* clock) {
        dirtySlot_ = slot;
        dirtyClock_ = clock;
    }
    // Reverse dependency map row (CSR into SimState::depPool) + the queue to
    // notify. Bound at elaboration together with bindDirty.
    void bindDeps(const uint32_t* deps, uint32_t n, detail::ReadyQueue* q) {
        deps_ = deps;
        depN_ = n;
        readyQ_ = q;
    }

protected:
    explicit Entity(EntityKind k) : kind_(k) {}

private:
    EntityKind kind_;
    Module* owner_ = nullptr;
    std::string name_;
    uint32_t flatIndex_ = 0;
    uint64_t* dirtySlot_ = nullptr;
    uint64_t* dirtyClock_ = nullptr;
    const uint32_t* deps_ = nullptr;      // CSR row into SimState::depPool
    uint32_t depN_ = 0;
    detail::ReadyQueue* readyQ_ = nullptr;
};

}  // namespace wolvicmod
