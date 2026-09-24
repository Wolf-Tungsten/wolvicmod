#pragma once

#include <algorithm>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

#include "wolvicmod/core/action.h"

// Expression fast path (§3.1): operators on signals/states do not evaluate —
// they build a node tree that registration expands into an equivalent Assign /
// Update, with the read set auto-collected from the expression's leaves:
//   sum = a + b;   dout = cnt;   req = 0;

namespace wolvicmod::detail {

// ---- nodes ----
template <class E>
struct Leaf {
    E* e;
};
template <class V>
struct Const {
    V v;
};
template <class Op, class L, class R>
struct Bin {
    L l;
    R r;
};
template <class Op, class X>
struct Un {
    X x;
};

template <class Op, class L, class R>
struct IsExpr<Bin<Op, L, R>> : std::true_type {};
template <class Op, class X>
struct IsExpr<Un<Op, X>> : std::true_type {};

template <class E>
concept ValueReadable = Readable<E> && !IsMem<E>::value;

template <class T>
concept ExprNode = IsExpr<std::decay_t<T>>::value;

template <class T>
concept ExprOperand = ValueReadable<T> || ExprNode<T> || std::is_arithmetic_v<T>;

template <class L, class R>
concept ExprPair = ExprOperand<L> && ExprOperand<R> &&
                   (ValueReadable<L> || ExprNode<L> || ValueReadable<R> || ExprNode<R>);

// Shift functors (the standard library has none).
struct OpShl {
    template <class A, class B>
    constexpr auto operator()(const A& a, const B& b) const { return a << b; }
};
struct OpShr {
    template <class A, class B>
    constexpr auto operator()(const A& a, const B& b) const { return a >> b; }
};

// ---- evaluation ----
template <class E>
const typename E::Value& evalNode(const Leaf<E>& n) { return n.e->readValue(); }
template <class V>
const V& evalNode(const Const<V>& n) { return n.v; }
template <class Op, class L, class R>
decltype(auto) evalNode(const Bin<Op, L, R>& n) { return Op{}(evalNode(n.l), evalNode(n.r)); }
template <class Op, class X>
decltype(auto) evalNode(const Un<Op, X>& n) { return Op{}(evalNode(n.x)); }

// ---- read-set collection ----
template <class E>
void collectLeaves(const Leaf<E>& n, std::vector<Entity*>& out) { out.push_back(n.e); }
template <class V>
void collectLeaves(const Const<V>&, std::vector<Entity*>&) {}
template <class Op, class L, class R>
void collectLeaves(const Bin<Op, L, R>& n, std::vector<Entity*>& out) {
    collectLeaves(n.l, out);
    collectLeaves(n.r, out);
}
template <class Op, class X>
void collectLeaves(const Un<Op, X>& n, std::vector<Entity*>& out) { collectLeaves(n.x, out); }

inline void dedupeReads(std::vector<Entity*>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

// ---- operand wrapping ----
template <ValueReadable E>
Leaf<E> wrapOperand(E& e) { return {&e}; }
template <ExprNode N>
std::decay_t<N> wrapOperand(N&& n) { return std::forward<N>(n); }
template <class V>
    requires(std::is_arithmetic_v<V>)
Const<V> wrapOperand(V v) { return {v}; }

template <class Op, class L, class R>
auto makeBin(L&& l, R&& r) {
    return Bin<Op, decltype(wrapOperand(std::forward<L>(l))),
               decltype(wrapOperand(std::forward<R>(r)))>{wrapOperand(std::forward<L>(l)),
                                                          wrapOperand(std::forward<R>(r))};
}
template <class Op, class X>
auto makeUn(X&& x) {
    return Un<Op, decltype(wrapOperand(std::forward<X>(x)))>{wrapOperand(std::forward<X>(x))};
}

}  // namespace wolvicmod::detail

// ---- operators (namespace wolvicmod so ADL finds them) ----
namespace wolvicmod {

#define WOLVICMOD_DEFINE_BINOP(sym, optype)                                \
    template <class L, class R>                                            \
        requires detail::ExprPair<std::remove_cvref_t<L>, std::remove_cvref_t<R>> \
    auto operator sym(L&& l, R&& r) {                                      \
        return detail::makeBin<optype>(std::forward<L>(l), std::forward<R>(r)); \
    }

WOLVICMOD_DEFINE_BINOP(+, std::plus<>)
WOLVICMOD_DEFINE_BINOP(-, std::minus<>)
WOLVICMOD_DEFINE_BINOP(*, std::multiplies<>)
WOLVICMOD_DEFINE_BINOP(/, std::divides<>)
WOLVICMOD_DEFINE_BINOP(%, std::modulus<>)
WOLVICMOD_DEFINE_BINOP(&, std::bit_and<>)
WOLVICMOD_DEFINE_BINOP(|, std::bit_or<>)
WOLVICMOD_DEFINE_BINOP(^, std::bit_xor<>)
WOLVICMOD_DEFINE_BINOP(<<, detail::OpShl)
WOLVICMOD_DEFINE_BINOP(>>, detail::OpShr)
WOLVICMOD_DEFINE_BINOP(&&, std::logical_and<>)
WOLVICMOD_DEFINE_BINOP(||, std::logical_or<>)
WOLVICMOD_DEFINE_BINOP(==, std::equal_to<>)
WOLVICMOD_DEFINE_BINOP(!=, std::not_equal_to<>)
WOLVICMOD_DEFINE_BINOP(<, std::less<>)
WOLVICMOD_DEFINE_BINOP(<=, std::less_equal<>)
WOLVICMOD_DEFINE_BINOP(>, std::greater<>)
WOLVICMOD_DEFINE_BINOP(>=, std::greater_equal<>)

#undef WOLVICMOD_DEFINE_BINOP

#define WOLVICMOD_DEFINE_UNOP(sym, optype)                                   \
    template <class X>                                                       \
        requires(detail::ValueReadable<std::remove_cvref_t<X>> ||            \
                 detail::ExprNode<std::remove_cvref_t<X>>)                   \
    auto operator sym(X&& x) { return detail::makeUn<optype>(std::forward<X>(x)); }

WOLVICMOD_DEFINE_UNOP(-, std::negate<>)
WOLVICMOD_DEFINE_UNOP(~, std::bit_not<>)
WOLVICMOD_DEFINE_UNOP(!, std::logical_not<>)

#undef WOLVICMOD_DEFINE_UNOP

}  // namespace wolvicmod

// ---- registration (definitions of the hooks declared in action.h) ----
namespace wolvicmod::detail {

template <class Sig, class E>
void registerAssignExpr(Sig* target, E&& expr) {
    using Ex = std::decay_t<E>;
    using T = typename Sig::Value;
    Module* ctx = currentCtx("assign");
    checkAssignTarget(target, ctx);
    Ex ex(std::forward<E>(expr));
    std::vector<Entity*> readVec;
    collectLeaves(ex, readVec);
    for (const Entity* r : readVec) checkReadableFrom(r, ctx, "assign expression");
    dedupeReads(readVec);
    static_assert(std::is_constructible_v<T, decltype(evalNode(ex))>,
                  "expression result must convert to the assign target type (§3.1)");
    auto compute = [ex = std::move(ex)]() mutable -> T { return evalNode(ex); };
    ctx->addAction(std::make_unique<AssignAction<T>>(target, ctx, std::move(readVec),
                                                     std::function<T()>(std::move(compute))));
}

template <class Target, class E>
void registerUpdateExpr(Target* target, std::vector<EventSlot> events,
                        std::vector<Entity*> extraReads, std::function<bool()> guard,
                        std::function<size_t()> addr, E&& expr) {
    using Ex = std::decay_t<E>;
    using T = typename Target::Value;
    Module* ctx = currentCtx("update");
    checkUpdateTarget(target, ctx);
    if (events.empty())
        fail("update target " + entityLabel(target) + ": .on(...) is required (§3.2)");
    Ex ex(std::forward<E>(expr));
    std::vector<Entity*> readVec;
    collectLeaves(ex, readVec);
    for (const Entity* r : readVec) checkReadableFrom(r, ctx, "update expression");
    for (const Entity* r : extraReads) checkReadableFrom(r, ctx, "update event/guard/address");
    readVec.insert(readVec.end(), extraReads.begin(), extraReads.end());
    dedupeReads(readVec);
    static_assert(std::is_constructible_v<T, decltype(evalNode(ex))>,
                  "expression result must convert to the update target type (§3.2)");
    auto compute = [ex = std::move(ex)]() mutable -> T { return evalNode(ex); };
    ctx->addAction(std::make_unique<UpdateAction<T, Target>>(
        target, ctx, std::move(readVec), std::move(events), std::move(guard),
        std::move(addr), std::function<T()>(std::move(compute))));
}

}  // namespace wolvicmod::detail
