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
#include "wolvicmod/dbg/stats.h"
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

    // State-update phase (Update only). The intent flag is a plain member —
    // the commit phase polls it once per Update per round, so it must not go
    // through a virtual call. The commit phase clears the flag after applying
    // (§5.2): with dirty-driven scheduling an Update whose inputs did not
    // change is skipped next round, and a stale flag would apply twice.
    // Chains with member-bitmap commit (bindMemberBit) instead keep the
    // intent source of truth in the chain descriptor's memberBits; this flag
    // then only mirrors the last run() for the round trace and is not
    // cleared by phase 2.
    bool intentActive() const { return intentActive_; }
    void clearIntent() { intentActive_ = false; }
    virtual void applyIntent() {}     // write the intent into the target
    virtual void finalizeTarget() {}  // Reg: commit next slot (NBA)

    // Phase-2 fast commit (§5.2): when the intent is a trivially-copyable byte
    // blob (Reg target), phase 2 applies it to the chain's shared next slot
    // with a type-agnostic byte copy — no virtual call per active Update.
    // typedCommitFn() is the chain's fallback commit for value types where
    // bitwise equality is not exact (padding): one indirect call per chain,
    // with a per-type (not per-action) target.
    const char* fastIntentSrc() const { return fastIntentSrc_; }
    uint32_t fastIntentSize() const { return fastIntentSize_; }
    using TypedCommitFn = void (*)(Entity*);
    virtual TypedCommitFn typedCommitFn() const { return nullptr; }

    // Update only: initialize edge-detection history to the signals' initial
    // values so no spurious edge is produced (§5.3).
    virtual void initEventPrev() {}

    // Update only: edge hits seen during the last run() (for round trace).
    virtual bool edgeSeen() const { return false; }
    virtual const std::string& edgeDesc() const {
        static const std::string kEmpty;
        return kEmpty;
    }

    // Edge-direction filtered dispatch (§5.2 push): event signals this Update
    // uses exclusively as a single one-direction edge (not a plain read,
    // guard, address, or second event). Filled at registration; consumed at
    // elaboration, where the dependency on such a signal moves from the
    // always-push reverse map to the signal's direction-filtered watch list.
    struct EdgeWatch {
        const Entity* sig;
        EdgeKind kind;
        bool* prev;  // the EventSlot::prev inside this action
        // Level guard pre-filter (§5.2 push): the Update's guard, read at the
        // transition — false means the edge is consumed without dispatch.
        const Entity* guard = nullptr;
        bool (*guardRead)(const Entity*) = nullptr;
        // EdgeBit 模式：单事件 Update 的边沿事实由写入点直接置位
        // （edgeBits 位图），跳过/反向不再维护 prev。
        bool edgeBit = false;
    };
    const std::vector<EdgeWatch>& edgeWatches() const { return edgeWatches_; }

    // Identity-connect aliasing (§5.2 elaboration): non-null when this Assign
    // is a same-type identity copy (b = a fast path) — a candidate for
    // elimination by storage aliasing. eliminated() marks actions dropped
    // from the graph by alias resolution.
    void setAliasSource(Entity* s) { aliasSrc_ = s; }
    Entity* aliasSource() const { return aliasSrc_; }
    void eliminate() { eliminated_ = true; }
    bool eliminated() const { return eliminated_; }

    // Commit notification binding (§5.2 phase 2, Updates only): the priority
    // chain index whose bit in the flat chain bitmap is set when the intent
    // flag rises. Bound at elaboration (buildUpdateChains).
    void bindCommit(uint32_t chain, uint64_t* chainBits) {
        commitChain_ = chain;
        commitBits_ = chainBits;
    }

    // Member-bitmap commit binding (§5.2 phase 2): non-null when this
    // Update's chain keeps member intent state in the chain descriptor
    // itself — run() then mirrors intent rises/falls into *bits, and phase 2
    // applies/clears intents without dereferencing member Actions.
    void bindMemberBit(uint64_t* bits, uint64_t bit) {
        memberBitsPtr_ = bits;
        memberBit_ = bit;
    }

    // EdgeBit 模式绑定（elaboration）：本动作在 execOrder 中的位置与
    // edgeBits 位图基址——run() 用它测试并清除自己的边沿事实位。
    void bindEdgeBit(uint32_t pos, uint64_t* edgeBits) {
        execPos_ = pos;
        edgeBits_ = edgeBits;
    }

protected:
    Action(Kind k, Entity* target, Module* ctx, std::vector<Entity*> reads)
        : kind_(k), target_(target), ctx_(ctx), reads_(std::move(reads)) {}

    void setIntentActive(bool v) { intentActive_ = v; }
    // Member-bitmap commit state (§5.2), bound at elaboration; nullptr when
    // the chain keeps per-member intent flags (slow path and Mem chains).
    uint64_t* memberBitsPtr() const { return memberBitsPtr_; }
    uint64_t memberBit() const { return memberBit_; }
    void publishFastIntent(const void* src, uint32_t size) {
        fastIntentSrc_ = static_cast<const char*>(src);
        fastIntentSize_ = size;
    }
    // Called when the intent flag rises false -> true: the chain must be
    // visited by phase 2 even though nothing scans for it any more.
    void notifyIntent() {
        commitBits_[commitChain_ >> 6] |= uint64_t{1} << (commitChain_ & 63);
    }
    void addEdgeWatch(const Entity* sig, EdgeKind kind, bool* prev, const Entity* guard,
                      bool (*guardRead)(const Entity*), bool edgeBit) {
        edgeWatches_.push_back({sig, kind, prev, guard, guardRead, edgeBit});
    }

    // EdgeBit 模式状态（bindEdgeBit 绑定，UpdateAction::run 使用）
    uint32_t execPos_ = 0;           // execOrder position (edgeBit mode)
    uint64_t* edgeBits_ = nullptr;   // flat edge-fact bitmap

private:
    Kind kind_;
    Entity* target_;
    Module* ctx_;
    uint32_t regIndex_ = 0;
    std::vector<Entity*> reads_;
    bool intentActive_ = false;
    std::vector<EdgeWatch> edgeWatches_;
    uint32_t commitChain_ = 0;
    uint64_t* commitBits_ = nullptr;  // flat chain bitmap
    Entity* aliasSrc_ = nullptr;      // identity-copy source (aliasing candidate)
    bool eliminated_ = false;         // dropped from the graph by aliasing
    const char* fastIntentSrc_ = nullptr;  // byte-copyable intent blob (§5.2)
    uint32_t fastIntentSize_ = 0;
    uint64_t* memberBitsPtr_ = nullptr;  // chain member intent bitmap (§5.2)
    uint64_t memberBit_ = 0;             // this Update's bit in it
};

// The compute functor is stored concrete (template parameter, no
// std::function): run() is the single type-erased boundary and the whole
// read-pack -> compute -> store chain inlines into it.
template <class T, class F>
class AssignAction : public Action {
public:
    AssignAction(Signal<T>* target, Module* ctx, std::vector<Entity*> reads, F compute)
        : Action(Kind::Assign, target, ctx, std::move(reads)),
          target_(target),
          compute_(std::move(compute)) {}

    void run() override {
        T& tv = target_->mutableValue();
        if constexpr (kEqualityComparable<T>) {
            // Change detection: an unchanged value does not dirty the target,
            // so downstream actions stay asleep (§5.2 fast path). Bool write
            // sites report the transition direction so edge watchers of the
            // opposite edge stay asleep too.
            T v = compute_();
            if (!(v == tv)) {
                tv = std::move(v);
                if constexpr (std::is_same_v<T, bool>)
                    target_->markDirtyBool(tv);
                else
                    target_->markDirty();
            }
        } else {
            tv = compute_();
            target_->markDirty();
        }
    }

private:
    Signal<T>* target_;
    F compute_;
};

struct EventSlot {
    const Entity* sig = nullptr;  // event signal (for trace descriptions)
    EdgeKind kind;
    bool (*read)(const Entity*);  // capture-less reader (see core/edge.h)
    bool prev = false;            // edge-detection history (§5.3)
    // EdgeBit 模式（单事件 watch-listed Update）：边沿事实由写入点直接置
    // edgeBits 位，run() 测试并清除，prev 历史免维护（trace 模式回退 prev）。
    bool watchEdgeBit = false;
};

// Level guard (§3.2), same capture-less representation as EdgeEvent: the guard
// reads exactly one bool-holding entity.
struct GuardSlot {
    const Entity* ent = nullptr;  // nullptr -> no guard
    bool (*read)(const Entity*) = nullptr;
};

template <class E>
GuardSlot makeGuard(E& g) {
    return {&g,
            [](const Entity* e) { return static_cast<bool>(static_cast<const E*>(e)->readValue()); }};
}

// Update action. Target = Reg<T> or Mem<T, R>. When several Updates target the
// same state, the commit phase applies active intents in reverse registration
// order so the earliest-registered (highest priority) lands last and wins
// (§4.3); Mem intents merge per row (§2.4).
template <class T, class Target, class F>
class UpdateAction : public Action {
public:
    UpdateAction(Target* target, Module* ctx, std::vector<Entity*> reads,
                 std::vector<EventSlot> events, GuardSlot guard,
                 std::function<size_t()> addr, F compute)
        : Action(Kind::Update, target, ctx, std::move(reads)),
          target_(target),
          events_(std::move(events)),
          guard_(guard),
          addr_(std::move(addr)),
          compute_(std::move(compute)) {
        // Phase-2 fast commit (§5.2): a trivially-copyable intent on a Reg
        // target is applied by byte copy — publish its address and size.
        if constexpr (!IsMem<Target>::value && std::is_trivially_copyable_v<T>)
            publishFastIntent(&intent_, sizeof(T));
    }

    void run() override {
        bool edge = false;
        edgeSeen_ = false;
        if (detail::traceDescWanted()) edgeDesc_.clear();
        for (auto& e : events_) {
            bool hit;
            if (e.watchEdgeBit && !detail::watchTraceMode()) {
                // EdgeBit 快径：边沿事实由写入点置位（本动作激活即说明
                // 匹配方向跳变发生），测试并清除；信号与 prev 都不读。
                const uint64_t m = uint64_t{1} << (execPos_ & 63);
                hit = (edgeBits_[execPos_ >> 6] & m) != 0;
                if (hit) edgeBits_[execPos_ >> 6] &= ~m;
            } else {
                const bool cur = e.read(e.sig);
                hit = (e.kind == EdgeKind::Posedge) ? (!e.prev && cur) : (e.prev && !cur);
                e.prev = cur;
            }
            if (hit) {
                edge = true;
                edgeSeen_ = true;
                if (detail::traceDescWanted()) {
                    if (!edgeDesc_.empty()) edgeDesc_ += ", ";
                    edgeDesc_ += (e.kind == EdgeKind::Posedge ? "posedge " : "negedge ");
                    edgeDesc_ += e.sig->hierPath();
                }
            }
        }
        const bool guardPass = guard_.ent == nullptr || guard_.read(guard_.ent);
        detail::statUpdateRun(edge, guardPass);
        const bool active = edge && guardPass;
        if (uint64_t* mb = memberBitsPtr(); mb != nullptr) {
            // Member-bitmap commit (§5.2): the chain descriptor's bitmap is
            // the intent source of truth — phase 2 clears it wholesale after
            // committing, so rise/fall detection must read it here rather
            // than the per-action flag (which phase 2 no longer clears;
            // intentActive_ stays as the round-trace mirror only).
            const uint64_t my = memberBit();
            if (active && (*mb & my) == 0) {
                *mb |= my;
                notifyIntent();
            } else if (!active && (*mb & my) != 0) {
                *mb &= ~my;
            }
        } else if (active && !intentActive()) {
            notifyIntent();
        }
        setIntentActive(active);
        if (active) {
            if constexpr (IsMem<Target>::value) intentRow_ = addr_();
            intent_ = compute_();
        }
    }

    void applyIntent() override {
        if constexpr (IsMem<Target>::value)
            target_->writeRow(intentRow_, std::move(intent_));
        else
            target_->nextSlot() = std::move(intent_);
    }

    void finalizeTarget() override {
        if constexpr (!IsMem<Target>::value) target_->commitNext();
    }

    // Chain commit fallback for value types with inexact bitwise equality
    // (§5.2 fast commit): the call target is shared by every Update of this
    // value type, so the predictor sees one target per type, not per action.
    TypedCommitFn typedCommitFn() const override {
        if constexpr (!IsMem<Target>::value)
            return [](Entity* e) { static_cast<Target*>(e)->commitNext(); };
        else
            return nullptr;
    }

    void initEventPrev() override {
        for (auto& e : events_) e.prev = e.read(e.sig);
    }

    bool edgeSeen() const override { return edgeSeen_; }
    const std::string& edgeDesc() const override { return edgeDesc_; }

    // Edge-direction filtered dispatch (§5.2 push): mark the event on sig as
    // eligible for direction filtering — the dependency moves to the signal's
    // watch list at elaboration. The prev pointer kept here lets the write
    // site advance this Update's edge history when a transition in the
    // opposite direction is skipped.
    void registerEdgeWatch(const Entity* sig, EdgeKind kind) {
        for (auto& e : events_)
            if (e.sig == sig && e.kind == kind) {
                // EdgeBit 模式仅限单事件 Update：多位事件共享一个 execPos
                // 位无法区分边沿来自哪个信号，保持 prev 检测。
                const bool eb = events_.size() == 1;
                e.watchEdgeBit = eb;
                addEdgeWatch(sig, kind, &e.prev, guard_.ent, guard_.read, eb);
                return;
            }
    }

private:
    Target* target_;
    std::vector<EventSlot> events_;
    GuardSlot guard_;
    std::function<size_t()> addr_;  // Mem only
    F compute_;
    T intent_{};
    size_t intentRow_ = 0;
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

// Build the compute thunk. The static_assert enforces the compile-time checks
// of §3.1: the lambda must be invocable with std::tuple<const Ts&...> and its
// return must convert to the target type. The returned closure keeps its
// concrete type — the action stores it without a std::function wrapper.
template <class T, class F, class... Es>
auto makeCompute(F&& f, const std::tuple<Es*...>& srcs) {
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
Action* registerAssignLambda(Sig* target, F&& f, const std::tuple<Es*...>& srcs) {
    Module* ctx = currentCtx("assign");
    checkAssignTarget(target, ctx);
    std::vector<Entity*> readVec = checkReadSet(ctx, "assign read set", srcs);
    auto compute = makeCompute<typename Sig::Value>(std::forward<F>(f), srcs);
    auto* raw = ctx->makeAction<AssignAction<typename Sig::Value, decltype(compute)>>(
        target, ctx, std::move(readVec), std::move(compute));
    ctx->addAction(raw);
    return raw;
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
                          std::vector<Entity*> extraReads, GuardSlot guard,
                          std::function<size_t()> addr, F&& f,
                          const std::tuple<Es*...>& srcs) {
    Module* ctx = currentCtx("update");
    checkUpdateTarget(target, ctx);
    if (events.empty()) fail("update target " + entityLabel(target) + ": .on(...) is required (§3.2)");
    std::vector<Entity*> readVec = checkReadSet(ctx, "update read set", srcs);
    for (Entity* e : extraReads) checkReadableFrom(e, ctx, "update event/guard/address");
    readVec.insert(readVec.end(), extraReads.begin(), extraReads.end());

    // Edge-direction filtered dispatch (§5.2 push): an event signal is
    // eligible only when this Update uses it exclusively as a single
    // one-direction edge — not also as a plain read (srcs), guard, address,
    // or a second event. extraReads holds each event signal exactly once by
    // construction, so a second occurrence there means the guard or the
    // address expression reads it too.
    std::vector<std::pair<const Entity*, EdgeKind>> eligible;
    for (const EventSlot& ev : events) {
        size_t evN = 0, erN = 0;
        for (const EventSlot& e2 : events) evN += (e2.sig == ev.sig) ? 1 : 0;
        for (const Entity* r : extraReads) erN += (r == ev.sig) ? 1 : 0;
        const bool inSrcs = std::apply(
            [&](auto*... p) { return ((p == ev.sig) || ...); }, srcs);
        if (evN == 1 && erN == 1 && !inSrcs) eligible.emplace_back(ev.sig, ev.kind);
    }

    auto compute = makeCompute<typename Target::Value>(std::forward<F>(f), srcs);
    auto* action = ctx->makeAction<UpdateAction<typename Target::Value, Target, decltype(compute)>>(
        target, ctx, std::move(readVec), std::move(events), guard,
        std::move(addr), std::move(compute));
    for (const auto& [sig, kind] : eligible) action->registerEdgeWatch(sig, kind);
    ctx->addAction(action);
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
                        std::vector<Entity*> extraReads, GuardSlot guard,
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
        Action* a = registerAssignLambda(
            target, [](auto t) -> const ReadValueOf<S>& { return std::get<0>(t); },
            std::tuple<S*>(&src));
        // Identity-connect aliasing (§5.2): a same-type copy (b = a) is an
        // aliasing candidate — elaboration may eliminate the action and alias
        // the target's storage onto the source's. Conversion copies (different
        // value types) and Mem sources are never aliased.
        if constexpr (std::is_same_v<T, ReadValueOf<S>> && !IsMem<S>::value)
            a->setAliasSource(&src);
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
                                              GuardSlot{}, nullptr);
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
                                          GuardSlot{}, std::move(readRow));
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
                       std::vector<Entity*> extraReads, GuardSlot guard,
                       std::function<size_t()> addr)
        : target_(target),
          events_(std::move(events)),
          extraReads_(std::move(extraReads)),
          guard_(guard),
          addr_(std::move(addr)) {}

    // Level guard (§3.2): when the guard fails the Update does not activate.
    template <BoolReadable G>
    auto en(G& g) && {
        if (guard_.ent != nullptr) fail(".en(...) specified twice for one Update");
        extraReads_.push_back(&g);
        return UpdateReadyBuilder<Target>(target_, std::move(events_), std::move(extraReads_),
                                          makeGuard(g), std::move(addr_));
    }

    template <Readable... Es>
    auto reads(Es&... es) && {
        return UpdateReadsBuilder<Target, Es...>(target_, std::move(events_),
                                                 std::move(extraReads_), guard_,
                                                 std::move(addr_), std::tuple<Es*...>(&es...));
    }

    // Fast path: constant / identity / expression (§3.2).
    template <class Src>
    void operator=(Src&& src) && {
        using S = std::decay_t<Src>;
        using T = typename Target::Value;
        if constexpr (IsExpr<S>::value) {
            registerUpdateExpr(target_, std::move(events_), std::move(extraReads_),
                               guard_, std::move(addr_), std::forward<Src>(src));
        } else if constexpr (Readable<S>) {
            registerUpdateLambda(target_, std::move(events_), std::move(extraReads_),
                                 guard_, std::move(addr_),
                                 [](auto t) -> const ReadValueOf<S>& { return std::get<0>(t); },
                                 std::tuple<S*>(&src));
        } else if constexpr (std::is_constructible_v<T, Src>) {
            static_assert(std::is_copy_constructible_v<T>,
                          "constant fast path requires a copy-constructible value type");
            registerUpdateLambda(target_, std::move(events_), std::move(extraReads_),
                                 guard_, std::move(addr_),
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
    GuardSlot guard_;
    std::function<size_t()> addr_;
};

template <class Target, class... Es>
class UpdateReadsBuilder {
public:
    UpdateReadsBuilder(Target* target, std::vector<EventSlot> events,
                       std::vector<Entity*> extraReads, GuardSlot guard,
                       std::function<size_t()> addr, std::tuple<Es*...> srcs)
        : target_(target),
          events_(std::move(events)),
          extraReads_(std::move(extraReads)),
          guard_(guard),
          addr_(std::move(addr)),
          srcs_(srcs) {}

    template <class F>
    void operator=(F&& f) && {
        registerUpdateLambda(target_, std::move(events_), std::move(extraReads_),
                             guard_, std::move(addr_), std::forward<F>(f), srcs_);
    }

private:
    Target* target_;
    std::vector<EventSlot> events_;
    std::vector<Entity*> extraReads_;
    GuardSlot guard_;
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

inline void Module::addAction(Action* a) { actions_.push_back(a); }

}  // namespace wolvicmod
