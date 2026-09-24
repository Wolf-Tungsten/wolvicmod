#pragma once

#include <functional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/mem.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/core/reg.h"
#include "wolvicmod/core/signal.h"
#include "wolvicmod/sim/errors.h"

namespace wolvicmod {

// Anything readable into a read set: In/Out/Wire/Reg (yields const T&) and Mem
// (yields const Mem&, §3.4).
template <class E>
concept Readable =
    std::derived_from<E, Entity> && requires(const E& e) {
        { e.readValue() };
    };

template <class E>
using ReadValueOf = std::decay_t<decltype(std::declval<const E&>().readValue())>;

namespace detail {

// Round trace (§6.3) descriptions are built only while a trace sink is
// attached; set by traceOn()/traceOff() (dbg/trace.h).
inline bool& traceDescFlag() {
    static bool f = false;
    return f;
}
inline bool traceDescWanted() { return traceDescFlag(); }

}  // namespace detail

// ---------------------------------------------------------------
// Action records (§2.1)
// ---------------------------------------------------------------

class Action {
public:
    enum class Kind : uint8_t { Assign, Update };
    virtual ~Action() = default;

    Kind kind() const { return kind_; }
    Entity* target() const { return target_; }
    Module* ctx() const { return ctx_; }
    uint32_t regIndex() const { return regIndex_; }
    void setRegIndex(uint32_t i) { regIndex_ = i; }

    // All entities this action reads — read set plus guard/address/event
    // signals for Updates: the entity -> action edges of the combinational
    // evaluation graph (§4.3).
    const std::vector<Entity*>& reads() const { return reads_; }

    std::string describe() const {
        return (kind_ == Kind::Assign ? "assign -> " : "update -> ") + target_->hierPath();
    }

    // Combinational phase (§5.2): Assign writes its signal; Update only detects
    // edges and records its intent — no state is touched.
    virtual void run() = 0;

    // State-update phase (Update only).
    virtual bool intentActive() const { return false; }
    virtual void applyIntent() {}     // write the intent into the target
    virtual void finalizeTarget() {}  // Reg: commit next slot (NBA)

    // Update only: initialize edge-detection history to the signals' initial
    // values so no spurious edge is produced (§5.3).
    virtual void initEventPrev() {}

    // Update only: edge hits seen during the last run() (for round trace).
    virtual bool edgeSeen() const { return false; }
    virtual const std::string& edgeDesc() const {
        static const std::string kEmpty;
        return kEmpty;
    }

protected:
    Action(Kind k, Entity* target, Module* ctx, std::vector<Entity*> reads)
        : kind_(k), target_(target), ctx_(ctx), reads_(std::move(reads)) {}

private:
    Kind kind_;
    Entity* target_;
    Module* ctx_;
    uint32_t regIndex_ = 0;
    std::vector<Entity*> reads_;
};

template <class T>
class AssignAction : public Action {
public:
    AssignAction(Signal<T>* target, Module* ctx, std::vector<Entity*> reads,
                 std::function<T()> compute)
        : Action(Kind::Assign, target, ctx, std::move(reads)),
          target_(target),
          compute_(std::move(compute)) {}

    void run() override { target_->mutableValue() = compute_(); }

private:
    Signal<T>* target_;
    std::function<T()> compute_;
};

struct EventSlot {
    const Entity* sig = nullptr;  // event signal (for trace descriptions)
    EdgeKind kind;
    std::function<bool()> read;
    bool prev = false;  // edge-detection history (§5.3)
};

// Update action. Target = Reg<T> or Mem<T, R>. When several Updates target the
// same state, the commit phase applies active intents in reverse registration
// order so the earliest-registered (highest priority) lands last and wins
// (§4.3); Mem intents merge per row (§2.4).
template <class T, class Target>
class UpdateAction : public Action {
public:
    UpdateAction(Target* target, Module* ctx, std::vector<Entity*> reads,
                 std::vector<EventSlot> events, std::function<bool()> guard,
                 std::function<size_t()> addr, std::function<T()> compute)
        : Action(Kind::Update, target, ctx, std::move(reads)),
          target_(target),
          events_(std::move(events)),
          guard_(std::move(guard)),
          addr_(std::move(addr)),
          compute_(std::move(compute)) {}

    void run() override {
        bool edge = false;
        edgeSeen_ = false;
        if (detail::traceDescWanted()) edgeDesc_.clear();
        for (auto& e : events_) {
            const bool cur = e.read();
            const bool hit =
                (e.kind == EdgeKind::Posedge) ? (!e.prev && cur) : (e.prev && !cur);
            if (hit) {
                edge = true;
                edgeSeen_ = true;
                if (detail::traceDescWanted()) {
                    if (!edgeDesc_.empty()) edgeDesc_ += ", ";
                    edgeDesc_ += (e.kind == EdgeKind::Posedge ? "posedge " : "negedge ");
                    edgeDesc_ += e.sig->hierPath();
                }
            }
            e.prev = cur;
        }
        active_ = edge && (!guard_ || guard_());
        if (active_) {
            if constexpr (IsMem<Target>::value) intentRow_ = addr_();
            intent_ = compute_();
        }
    }

    bool intentActive() const override { return active_; }

    void applyIntent() override {
        if constexpr (IsMem<Target>::value)
            target_->writeRow(intentRow_, std::move(intent_));
        else
            target_->nextSlot() = std::move(intent_);
    }

    void finalizeTarget() override {
        if constexpr (!IsMem<Target>::value) target_->commitNext();
    }

    void initEventPrev() override {
        for (auto& e : events_) e.prev = e.read();
    }

    bool edgeSeen() const override { return edgeSeen_; }
    const std::string& edgeDesc() const override { return edgeDesc_; }

private:
    Target* target_;
    std::vector<EventSlot> events_;
    std::function<bool()> guard_;   // null → no guard
    std::function<size_t()> addr_;  // Mem only
    std::function<T()> compute_;
    T intent_{};
    size_t intentRow_ = 0;
    bool active_ = false;
    bool edgeSeen_ = false;
    std::string edgeDesc_;
};

// ---------------------------------------------------------------
// Registration-time checks (§2.2, §6.2)
// ---------------------------------------------------------------

namespace detail {

template <class>
struct DependentFalse : std::false_type {};

// Expression node trait — the primary template matches nothing; core/expr.h
// specializes it for expression nodes (milestone M5).
template <class S>
struct IsExpr : std::false_type {};

inline Module* currentCtx(const char* what) {
    if (ctxStack().empty())
        fail(std::string(what) +
             ": action registration must happen inside a module constructor (§4.1)");
    return ctxStack().back();
}

inline std::string entityLabel(const Entity* e) {
    return (e != nullptr && e->named()) ? ("'" + e->hierPath() + "'")
                                        : std::string("'<unnamed entity>'");
}

inline std::string ctxLabel(const Module* ctx) {
    return (ctx->name().empty() && ctx->parent() == nullptr) ? std::string("'<root>'")
                                                             : ("'" + ctx->hierPath() + "'");
}

inline void checkNamed(const Entity* e, const char* what) {
    if (e == nullptr || !e->named())
        fail(std::string(what) + ": " + entityLabel(e) +
             " was not created via a createXXX method (§6.1)");
}

// §2.2: a module may drive its own Wire/Out and direct children's In.
inline void checkAssignTarget(const Entity* target, Module* ctx) {
    checkNamed(target, "assign target");
    switch (target->kind()) {
        case EntityKind::Wire:
        case EntityKind::Out:
            if (target->owner() != ctx)
                fail("assign target " + entityLabel(target) + " is not owned by module " +
                     ctxLabel(ctx) + " (a module drives only its own Wire/Out, §2.2)");
            break;
        case EntityKind::In:
            if (target->owner()->parent() != ctx)
                fail("assign target " + entityLabel(target) +
                     ": an In can only be driven by its parent module (§2.2)");
            break;
        default:
            fail("assign target " + entityLabel(target) +
                 ": Reg/Mem are driven by .update(), not .assign()");
    }
}

// §2.2: a module may read its own entities and direct children's ports.
inline void checkReadableFrom(const Entity* e, Module* ctx, const char* what) {
    checkNamed(e, what);
    Module* o = e->owner();
    if (o == ctx) return;
    if (o != nullptr && o->parent() == ctx &&
        (e->kind() == EntityKind::In || e->kind() == EntityKind::Out))
        return;
    fail(std::string(what) + ": " + entityLabel(e) + " is not visible from module " +
         ctxLabel(ctx) + " (only own entities and direct child ports are, §2.2)");
}

template <class... Es>
std::vector<Entity*> checkReadSet(Module* ctx, const char* what, const std::tuple<Es*...>& srcs) {
    std::vector<Entity*> v;
    v.reserve(sizeof...(Es));
    std::apply(
        [&](auto*... p) {
            (checkReadableFrom(p, ctx, what), ...);
            (v.push_back(p), ...);
        },
        srcs);
    return v;
}

// Build the type-erased compute thunk. The static_assert enforces the
// compile-time checks of §3.1: the lambda must be invocable with
// std::tuple<const Ts&...> and its return must convert to the target type.
template <class T, class F, class... Es>
std::function<T()> makeCompute(F&& f, const std::tuple<Es*...>& srcs) {
    using Fd = std::decay_t<F>;
    static_assert(
        std::is_invocable_r_v<T, Fd&, std::tuple<const ReadValueOf<Es>&...>>,
        "the lambda must be callable as T(std::tuple<const Ts&...>) where Ts are the "
        "read-set value types in .reads(...) order (§3.1)");
    return [fn = Fd(std::forward<F>(f)), srcs]() mutable -> T {
        return std::apply(
            [&](auto*... p) -> T {
                return std::invoke(fn, std::forward_as_tuple(p->readValue()...));
            },
            srcs);
    };
}

template <class Sig, class F, class... Es>
void registerAssignLambda(Sig* target, F&& f, const std::tuple<Es*...>& srcs) {
    Module* ctx = currentCtx("assign");
    checkAssignTarget(target, ctx);
    std::vector<Entity*> readVec = checkReadSet(ctx, "assign read set", srcs);
    auto compute = makeCompute<typename Sig::Value>(std::forward<F>(f), srcs);
    ctx->addAction(std::make_unique<AssignAction<typename Sig::Value>>(
        target, ctx, std::move(readVec), std::move(compute)));
}

// §2.2: Reg/Mem are updated by their own module.
inline void checkUpdateTarget(const Entity* target, Module* ctx) {
    checkNamed(target, "update target");
    if (target->owner() != ctx)
        fail("update target " + entityLabel(target) +
             ": Reg/Mem are updated by their own module (§2.2)");
}

template <class Target, class F, class... Es>
void registerUpdateLambda(Target* target, std::vector<EventSlot> events,
                          std::vector<Entity*> extraReads, std::function<bool()> guard,
                          std::function<size_t()> addr, F&& f,
                          const std::tuple<Es*...>& srcs) {
    Module* ctx = currentCtx("update");
    checkUpdateTarget(target, ctx);
    if (events.empty()) fail("update target " + entityLabel(target) + ": .on(...) is required (§3.2)");
    std::vector<Entity*> readVec = checkReadSet(ctx, "update read set", srcs);
    for (Entity* e : extraReads) checkReadableFrom(e, ctx, "update event/guard/address");
    readVec.insert(readVec.end(), extraReads.begin(), extraReads.end());
    auto compute = makeCompute<typename Target::Value>(std::forward<F>(f), srcs);
    ctx->addAction(std::make_unique<UpdateAction<typename Target::Value, Target>>(
        target, ctx, std::move(readVec), std::move(events), std::move(guard),
        std::move(addr), std::move(compute)));
}

// Builder templates (defined below) and expression hooks (core/expr.h).
template <class Sig>
class AssignBuilder;
template <class Sig, class... Es>
class AssignReadsBuilder;
template <class Target>
class UpdateOnBuilder;
template <class Target>
class UpdateAddrBuilder;
template <class Target>
class UpdateReadyBuilder;
template <class Target, class... Es>
class UpdateReadsBuilder;
template <class Sig, class E>
void registerAssignExpr(Sig* target, E&& expr);
template <class Target, class E>
void registerUpdateExpr(Target* target, std::vector<EventSlot> events,
                        std::vector<Entity*> extraReads, std::function<bool()> guard,
                        std::function<size_t()> addr, E&& expr);

}  // namespace detail

// ---------------------------------------------------------------
// Assign registration chain (§3.1)
// ---------------------------------------------------------------

namespace detail {

template <class Sig>
class AssignBuilder {
public:
    explicit AssignBuilder(Sig* target) : target_(target) {}

    template <Readable... Es>
    auto reads(Es&... es) && {
        return AssignReadsBuilder<Sig, Es...>(target_, std::tuple<Es*...>(&es...));
    }

private:
    Sig* target_;
};

template <class Sig, class... Es>
class AssignReadsBuilder {
public:
    AssignReadsBuilder(Sig* target, std::tuple<Es*...> srcs) : target_(target), srcs_(srcs) {}

    template <class F>
    void operator=(F&& f) && {
        registerAssignLambda(target_, std::forward<F>(f), srcs_);
    }

private:
    Sig* target_;
    std::tuple<Es*...> srcs_;
};

// Fast-path dispatch: constant / identity / expression (§3.1).
template <class Sig, class Src>
void assignFrom(Sig* target, Src&& src) {
    using S = std::decay_t<Src>;
    using T = typename Sig::Value;
    if constexpr (IsExpr<S>::value) {
        registerAssignExpr(target, std::forward<Src>(src));
    } else if constexpr (Readable<S>) {
        registerAssignLambda(target, [](auto t) -> const ReadValueOf<S>& { return std::get<0>(t); },
                             std::tuple<S*>(&src));
    } else if constexpr (std::is_constructible_v<T, Src>) {
        static_assert(std::is_copy_constructible_v<T>,
                      "constant fast path requires a copy-constructible value type");
        registerAssignLambda(target, [val = T(std::forward<Src>(src))](auto) -> const T& { return val; },
                             std::tuple<>());
    } else {
        static_assert(DependentFalse<S>::value,
                      "unsupported assign source: use .reads(...) = lambda, a signal/state, "
                      "a constant, or an operator expression (§3.1)");
    }
}

}  // namespace detail

// ---------------------------------------------------------------
// Update registration chain (§3.2, §3.4): type-state builders enforce the
// word order on -> [addr] -> [en] -> reads -> lambda.
// ---------------------------------------------------------------

namespace detail {

template <class Target>
class UpdateOnBuilder {
public:
    explicit UpdateOnBuilder(Target* target) : target_(target) {}

    template <class... Evs>
    auto on(Evs... evs) && {
        static_assert(sizeof...(Evs) > 0, ".on(...) requires at least one edge event (§3.2)");
        static_assert((std::is_same_v<std::decay_t<Evs>, EdgeEvent> && ...),
                      ".on(...) takes posedge(...)/negedge(...) events");
        std::vector<EventSlot> slots;
        std::vector<Entity*> sigs;
        slots.reserve(sizeof...(Evs));
        sigs.reserve(sizeof...(Evs));
        ((slots.push_back(EventSlot{evs.sig, evs.kind, evs.read}), sigs.push_back(evs.sig)), ...);
        if constexpr (IsMem<Target>::value) {
            return UpdateAddrBuilder<Target>(target_, std::move(slots), std::move(sigs));
        } else {
            return UpdateReadyBuilder<Target>(target_, std::move(slots), std::move(sigs),
                                              nullptr, nullptr, false);
        }
    }

private:
    Target* target_;
};

// Mem only: .addr(...) is required before .en/.reads (§3.4).
template <class Target>
class UpdateAddrBuilder {
public:
    UpdateAddrBuilder(Target* target, std::vector<EventSlot> events, std::vector<Entity*> sigs)
        : target_(target), events_(std::move(events)), extraReads_(std::move(sigs)) {}

    template <Readable A>
    auto addr(A& a) && {
        static_assert(std::is_integral_v<ReadValueOf<A>>,
                      ".addr(...) signal must hold an integral address (§3.4)");
        extraReads_.push_back(&a);
        auto readRow = [&a, tgt = target_]() -> size_t {
            size_t v = static_cast<size_t>(a.readValue());
            if (v >= Target::kRows)
                fail("mem write address out of range: " + std::to_string(v) +
                     " >= " + std::to_string(Target::kRows) + " ('" + tgt->hierPath() + "')");
            return v;
        };
        return UpdateReadyBuilder<Target>(target_, std::move(events_), std::move(extraReads_),
                                          nullptr, std::move(readRow), false);
    }

private:
    Target* target_;
    std::vector<EventSlot> events_;
    std::vector<Entity*> extraReads_;
};

// After .on(...) (+ .addr(...) for Mem); .en(...) is optional.
template <class Target>
class UpdateReadyBuilder {
public:
    UpdateReadyBuilder(Target* target, std::vector<EventSlot> events,
                       std::vector<Entity*> extraReads, std::function<bool()> guard,
                       std::function<size_t()> addr, bool hasGuard)
        : target_(target),
          events_(std::move(events)),
          extraReads_(std::move(extraReads)),
          guard_(std::move(guard)),
          addr_(std::move(addr)),
          hasGuard_(hasGuard) {}

    // Level guard (§3.2): when the guard fails the Update does not activate.
    template <BoolReadable G>
    auto en(G& g) && {
        if (hasGuard_) fail(".en(...) specified twice for one Update");
        extraReads_.push_back(&g);
        auto guard = [&g] { return static_cast<bool>(g.readValue()); };
        return UpdateReadyBuilder<Target>(target_, std::move(events_), std::move(extraReads_),
                                          std::move(guard), std::move(addr_), true);
    }

    template <Readable... Es>
    auto reads(Es&... es) && {
        return UpdateReadsBuilder<Target, Es...>(target_, std::move(events_),
                                                 std::move(extraReads_), std::move(guard_),
                                                 std::move(addr_), std::tuple<Es*...>(&es...));
    }

    // Fast path: constant / identity / expression (§3.2).
    template <class Src>
    void operator=(Src&& src) && {
        using S = std::decay_t<Src>;
        using T = typename Target::Value;
        if constexpr (IsExpr<S>::value) {
            registerUpdateExpr(target_, std::move(events_), std::move(extraReads_),
                               std::move(guard_), std::move(addr_), std::forward<Src>(src));
        } else if constexpr (Readable<S>) {
            registerUpdateLambda(target_, std::move(events_), std::move(extraReads_),
                                 std::move(guard_), std::move(addr_),
                                 [](auto t) -> const ReadValueOf<S>& { return std::get<0>(t); },
                                 std::tuple<S*>(&src));
        } else if constexpr (std::is_constructible_v<T, Src>) {
            static_assert(std::is_copy_constructible_v<T>,
                          "constant fast path requires a copy-constructible value type");
            registerUpdateLambda(target_, std::move(events_), std::move(extraReads_),
                                 std::move(guard_), std::move(addr_),
                                 [val = T(std::forward<Src>(src))](auto) -> const T& { return val; },
                                 std::tuple<>());
        } else {
            static_assert(DependentFalse<S>::value,
                          "unsupported update source: use .reads(...) = lambda, a signal/state, "
                          "a constant, or an operator expression (§3.2)");
        }
    }

private:
    Target* target_;
    std::vector<EventSlot> events_;
    std::vector<Entity*> extraReads_;
    std::function<bool()> guard_;
    std::function<size_t()> addr_;
    bool hasGuard_;
};

template <class Target, class... Es>
class UpdateReadsBuilder {
public:
    UpdateReadsBuilder(Target* target, std::vector<EventSlot> events,
                       std::vector<Entity*> extraReads, std::function<bool()> guard,
                       std::function<size_t()> addr, std::tuple<Es*...> srcs)
        : target_(target),
          events_(std::move(events)),
          extraReads_(std::move(extraReads)),
          guard_(std::move(guard)),
          addr_(std::move(addr)),
          srcs_(srcs) {}

    template <class F>
    void operator=(F&& f) && {
        registerUpdateLambda(target_, std::move(events_), std::move(extraReads_),
                             std::move(guard_), std::move(addr_), std::forward<F>(f), srcs_);
    }

private:
    Target* target_;
    std::vector<EventSlot> events_;
    std::vector<Entity*> extraReads_;
    std::function<bool()> guard_;
    std::function<size_t()> addr_;
    std::tuple<Es*...> srcs_;
};

}  // namespace detail

// ---------------------------------------------------------------
// Entity member definitions (declared in signal.h / reg.h / mem.h)
// ---------------------------------------------------------------

template <class T>
template <class Src>
void Signal<T>::operator=(Src&& src) {
    detail::assignFrom(this, std::forward<Src>(src));
}

template <class T>
template <int Dummy>
auto Signal<T>::assign() {
    return detail::AssignBuilder<Signal<T>>(this);
}

template <class T>
void In<T>::operator=(const In& src) {
    detail::assignFrom(this, src);
}

template <class T>
void Out<T>::operator=(const Out& src) {
    detail::assignFrom(this, src);
}

template <class T>
void Wire<T>::operator=(const Wire& src) {
    detail::assignFrom(this, src);
}

template <class T>
template <int Dummy>
auto Reg<T>::update() {
    return detail::UpdateOnBuilder<Reg<T>>(this);
}

template <class T, size_t R>
template <int Dummy>
auto Mem<T, R>::update() {
    return detail::UpdateOnBuilder<Mem<T, R>>(this);
}

// ---------------------------------------------------------------
// Module member definitions that need Action complete
// ---------------------------------------------------------------

inline void Module::addAction(std::unique_ptr<Action> a) { actions_.push_back(std::move(a)); }

}  // namespace wolvicmod
