#pragma once

// 集合操作原语：combine——把 N 个同值类型可读实体合并为一路 std::array 值信号。
//
// 框架里 std::array 是一等"值"（Wire<std::array<T>> / In<std::array<T,N>> 均
// 合法），combine 是实体世界 → 值世界的转换阀：dst[i] = srcs[i] 的当前值。
// 全框架唯一必需的一处 std::apply（reads 可变参展开）收编于此；过了这道阀，
// 下游 reads(dst) 只见一个标量实体——集合性在信号层被消化，reads 语义不变。
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

// combine：dst[i] = srcs[i] 的当前值（一次 assign 注册；reads 逐元素过
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

// collectPorts：子模块数组（ChildModuleArray）的同名端口合并（combine 的复合
// 便捷）。get 为访问子（Cm& -> Out<V>&），调用点以 lambda 给出且必须带尾置返
// 回类型——端口是引用成员，省略 -> Out<V>& 会按值返回并拷贝实体（Entity 不
// 可拷贝，编译失败）。
template <class V, class Cm, size_t N, class Get>
void collectPorts(Signal<std::array<V, N>>& dst, ChildModuleArray<Cm, N>& cms, Get get) {
    using E = std::remove_reference_t<decltype(get(cms[0]))>;
    std::array<E*, N> srcs{};
    for (size_t i = 0; i < N; ++i) srcs[i] = &get(cms[i]);
    combine(dst, srcs);
}

}  // namespace wolvicmod
