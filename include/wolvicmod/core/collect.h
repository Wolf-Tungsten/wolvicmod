#pragma once

// collectPorts：把 N 个同类型子模块端口汇聚成一路 Wire<std::array<V,N>>。
//
// reads(...) 是可变参 API，子模块以指针数组（createChildModule 循环建）持有
// 时无法逐一枚举，这里用 std::apply 一次性展开。这是 N 同构子模块惯用法的
// 配套胶水：散布（1→N，逐子模块 assign）用普通 for 循环即可，汇聚（N→1，
// 数组端口单驱动）必须程序化展开。
//
// get 为访问子（Cm& -> Out<V>&），调用点以 lambda 给出（端口是引用成员，
// 无法取成员指针）。

#include <array>
#include <cstdint>
#include <tuple>

#include "wolvicmod/core/module.h"

namespace wolvicmod {

// dst[i] = get(*cms[i]) 的当前值。
template <class V, size_t N, class CmArr, class Get>
void collectPorts(Wire<std::array<V, N>>& dst, CmArr& cms, Get get) {
    std::apply(
        [&](auto*... cm) {
            dst.assign().reads(get(*cm)...) = [](auto src) {
                std::array<V, N> a{};
                uint32_t i = 0;
                std::apply([&](const auto&... v) { ((a[i++] = v), ...); }, src);
                return a;
            };
        },
        cms);
}

}  // namespace wolvicmod
