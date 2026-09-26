#pragma once

// 集合操作原语：combine——把 N 个同值类型可读实体合并为一路 std::array 值信号。
//
// 框架里 std::array 是一等"值"（Wire<std::array<T>> / In<std::array<T,N>> 均
// 合法），combine 是实体世界 → 值世界的转换阀：dst[i] = srcs[i] 的当前值。
// 全框架唯一必需的一处 std::apply（reads 可变参展开）收编于此；过了这道阀，
// 下游 reads(dst) 只见一个标量实体——集合性在信号层被消化，reads 语义不变。
//
// 三重重载：
//   1. combine(dst, 实体指针数组)                ——原语；
//   2. combine(dst, ChildModuleArray, get)       ——子模块数组 + 端口提取；
//   3. combine(dst, 实体指针数组, get)            ——指针数组 + 投影提取。
// get 统一收元素引用（Cm& / E&）、返回 Readable<V>&（如 Out<V>&），且调用点
// 必须带尾置返回类型——端口/信号是引用成员，省略会按值返回并拷贝实体
// （Entity 不可拷贝，编译失败）。
//
// 反方向（1→N 散布）每个目标端口是独立 assign 目标，普通 for 循环即可，
// 无需设施。

#include <array>
#include <cstdint>
#include <tuple>
#include <type_traits>

#include "wolvicmod/core/action.h"
#include "wolvicmod/core/module.h"

namespace wolvicmod {

// combine 原语：dst[i] = srcs[i] 的当前值（一次 assign 注册；reads 逐元素过
// §2.2 可见性审计——只能合并本模块实体与直接子模块端口）。
template <class V, size_t N, Readable E>
void combine(Signal<std::array<V, N>>& dst, const std::array<E*, N>& srcs) {
    static_assert(N > 0, "combine: empty source array");
    static_assert(std::is_same_v<ReadValueOf<E>, V>,
                  "combine: element value type must match the array value type");
    std::apply(
        [&](auto*... e) {
            dst.assign().reads(*e...) = [](auto src) {
                std::array<V, N> a{};
                uint32_t i = 0;
                std::apply([&](const auto&... v) { ((a[i++] = v), ...); }, src);
                return a;
            };
        },
        srcs);
}

// combine 重载：子模块数组 + 端口提取 lambda（get: Cm& -> Readable<V>&）。
template <class V, size_t N, class Cm, size_t M, class Get>
void combine(Signal<std::array<V, N>>& dst, ChildModuleArray<Cm, M>& cms, Get get) {
    static_assert(M == N, "combine: child module count must match the array value type");
    using E = std::remove_reference_t<decltype(get(cms[0]))>;
    std::array<E*, N> srcs{};
    for (size_t i = 0; i < N; ++i) srcs[i] = &get(cms[i]);
    combine(dst, srcs);
}

// combine 重载：实体指针数组（Wire/Reg 等序列）+ 投影 lambda（get: E& ->
// Readable<V>&）。
template <class V, size_t N, class E, size_t M, class Get>
void combine(Signal<std::array<V, N>>& dst, const std::array<E*, M>& srcs, Get get) {
    static_assert(M == N, "combine: source count must match the array value type");
    using R = std::remove_reference_t<decltype(get(*srcs[0]))>;
    std::array<R*, N> ptrs{};
    for (size_t i = 0; i < N; ++i) ptrs[i] = &get(*srcs[i]);
    combine(dst, ptrs);
}

}  // namespace wolvicmod
