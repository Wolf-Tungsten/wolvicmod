#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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

    // Identity-connect aliasing (§5.2): a same-type identity assign
    // (b = a fast path) is eliminated at elaboration — the target's value
    // storage aliases the canonical source's, and consumers rewire to the
    // canonical entity in the evaluation graph. valueStorage() exposes the
    // effective value address (nullptr for Mem / non-value entities);
    // aliasStorageTo() redirects a Signal's storage.
    virtual void* valueStorage() { return nullptr; }
    virtual void aliasStorageTo(void*) {}

    // Phase-2 fast commit (§5.2): Reg overrides expose the next slot and the
    // value-type properties so the commit path can apply intents and detect
    // changes with type-agnostic byte ops instead of virtual calls.
    virtual void* nextStorage() { return nullptr; }
    // True when bitwise equality implies value equality (no padding bits), so
    // memcmp is an exact change test; false means the typed commit path keeps
    // operator== semantics.
    virtual bool bitwiseEqOk() const { return false; }
    virtual bool isBoolReg() const { return false; }

    // Dirty-driven dispatch (§5.2): a value change notifies dependent actions
    // by setting one bit per dependent in the flat activity bitmap (the bit
    // OR is idempotent — the bit IS the dedup). Slots are bound at
    // elaboration into SimState's flat arrays; before elaboration marks are
    // no-ops.
    void markDirty() {
        if (actBits_ == nullptr) return;
        pushDeps();
        // The transition direction is unknown here, so edge watchers of both
        // directions are notified conservatively (they maintain their own
        // edge history when they run).
        pushWatch(posWatch_, posWatchN_);
        pushWatch(negWatch_, negWatchN_);
    }

    // Edge-direction-filtered dispatch (§5.2): a dependent Update whose only
    // use of this entity is a single one-direction edge event sits in the
    // matching watch list instead of the always-notify dependency list. Bool
    // write sites know the transition direction: watchers of the matching
    // edge are notified; opposite watchers only have their edge history
    // advanced (prev = the value they would have observed), which is what
    // keeps a skipped Update's next edge detection correct. Correctness
    // relies on one transition per entity per round, so entities driven from
    // inside an SCC group (steady-state iteration may rewrite them) are never
    // put into watch lists (see buildExecOrder).
    //
    // Guard pre-filter (§5.2 push): a watcher carrying a level guard whose
    // current value is false cannot activate on this edge — active = edge &&
    // guard, and the edge is known true at the write site. Skipping consumes
    // the edge by advancing prev (exactly what its run() would do), so no
    // dispatch happens at all. Correctness additionally requires the guard to
    // hold its settled value across the clock pulse — the settle-then-pulse
    // clocking convention (poke inputs + eval, then poke clock + eval) that
    // all harnesses follow. markDirty() (direction unknown) cannot filter:
    // it does not know the new value, so it cannot advance prev on a skip.
    void markDirtyBool(bool v) {
        if (actBits_ == nullptr) return;
        pushDeps();
        if (v) {
            pushWatch(posWatch_, posWatchN_, true);
            for (uint32_t i = 0; i < negWatchN_; ++i) *negWatch_[i].prev = true;
        } else {
            pushWatch(negWatch_, negWatchN_, false);
            for (uint32_t i = 0; i < posWatchN_; ++i) *posWatch_[i].prev = false;
        }
    }

    struct EdgeWatchDep {
        uint32_t execPos;
        bool* prev;  // the watcher's EventSlot::prev, kept current on skips
        // Level guard pre-filter (§5.2 push): non-null when the watcher has a
        // guard; read at the transition — false means skip + consume edge.
        const Entity* guard = nullptr;
        bool (*guardRead)(const Entity*) = nullptr;
    };
    void bindEdgeWatches(const EdgeWatchDep* pos, uint32_t posN, const EdgeWatchDep* neg,
                         uint32_t negN) {
        posWatch_ = pos;
        posWatchN_ = posN;
        negWatch_ = neg;
        negWatchN_ = negN;
    }

    // Reverse dependency map row (CSR into SimState::depPool) plus the flat
    // activity bitmap the row's bits are set into. Bound at elaboration.
    void bindDeps(const uint32_t* deps, uint32_t n, uint64_t* actBits) {
        deps_ = deps;
        depN_ = n;
        actBits_ = actBits;
    }

protected:
    explicit Entity(EntityKind k) : kind_(k) {}

private:
    static void setBit(uint64_t* bits, uint32_t pos) {
        bits[pos >> 6] |= uint64_t{1} << (pos & 63);
    }
    void pushDeps() {
        for (uint32_t i = 0; i < depN_; ++i) setBit(actBits_, deps_[i]);
    }
    // Direction unknown (markDirty): unconditional notify, no prev touch —
    // watchers maintain their own edge history when they run.
    void pushWatch(const EdgeWatchDep* w, uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) setBit(actBits_, w[i].execPos);
    }
    // Direction known (markDirtyBool): guard pre-filter — a watcher whose
    // guard reads false cannot activate; consume the edge in place of its
    // run() by advancing prev to the value it would have observed.
    void pushWatch(const EdgeWatchDep* w, uint32_t n, bool v) {
        for (uint32_t i = 0; i < n; ++i) {
            if (w[i].guard != nullptr && !w[i].guardRead(w[i].guard))
                *w[i].prev = v;
            else
                setBit(actBits_, w[i].execPos);
        }
    }

    EntityKind kind_;
    Module* owner_ = nullptr;
    std::string name_;
    uint32_t flatIndex_ = 0;
    const uint32_t* deps_ = nullptr;      // CSR row into SimState::depPool
    uint32_t depN_ = 0;
    uint64_t* actBits_ = nullptr;         // flat activity bitmap
    const EdgeWatchDep* posWatch_ = nullptr;  // CSR rows into SimState watch pools
    uint32_t posWatchN_ = 0;
    const EdgeWatchDep* negWatch_ = nullptr;
    uint32_t negWatchN_ = 0;
};

}  // namespace wolvicmod
