#pragma once

// 波形探针（§6.3）：从复合类型实体引出可观测字段，建成一根普通命名 Wire、
// 由提取 assign 驱动——零新机制，纯语法糖。复合类型默认不进 FST（format.h
// 的静默跳过规则），探针是字段级可观测性的补足（整捆进波形则特化
// FstFormat<T>，见 wave/format.h）。
//
//   probe(src, "field", [](const T& v) { return v.field; });
//
// - 探针名为源实体相对当前模块的路径 + "." + 字段（如 "flit_w.valid"、
//   "sub.dout.valid"），在波形中与源视觉相邻，重名按 §6.1 命名表查错；
// - get 的返回必须可波形格式化（bool/integral，或用户特化了 FstFormat 的
//   类型）——探针的全部意义就是进 FST，返回不可格式化类型是建模错误
//   （编译期拒绝）；
// - 须于模块构造函数中调用（§4.1），可见性审计同 assign 读集（§2.2：
//   本模块实体与直接子模块端口）；
// - 探针是观测终端：逻辑不得读取探针 Wire（剥除构建中它不存在）。
//
// 构建开关：-DWOLVICMOD_PROBE=0 时 probe 为空操作——探针不进 FST、不占求
// 值；get 不被调用，若写成泛型 lambda（auto 参数）其函数体也不再实例化
// （可写仅调试期成立的代码；具体参数 lambda 的函数体仍会被完整检查）。此
// 模式返回 void，跨构建模式共存的调用点不要绑定返回值。

#include <string>
#include <type_traits>
#include <utility>

#include "wolvicmod/core/action.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/wave/format.h"

#ifndef WOLVICMOD_PROBE
#define WOLVICMOD_PROBE 1
#endif

namespace wolvicmod {

template <class Src, class F>
using ProbeValueOf = std::decay_t<std::invoke_result_t<F&, const ReadValueOf<Src>&>>;

#if WOLVICMOD_PROBE

namespace detail {

// 探针名：源实体相对当前模块的路径 + "." + 字段。
inline std::string probeName(const Entity& src, const Module* ctx, const std::string& field) {
    std::string path = src.hierPath();
    const std::string base = ctx->hierPath();
    if (!base.empty() && path.starts_with(base + ".")) path = path.substr(base.size() + 1);
    return path + "." + field;
}

}  // namespace detail

template <Readable Src, class F>
Wire<ProbeValueOf<Src, F>>& probe(Src& src, const std::string& field, F&& get) {
    using R = ProbeValueOf<Src, F>;
    static_assert(FstFormattable<R>,
                  "probe: the extraction must return a wave-formattable type "
                  "(bool/integral or an FstFormat-specialized type) — a probe "
                  "exists to enter the waveform (§6.3)");
    Module* ctx = detail::currentCtx("probe");
    Wire<R>& w = ctx->createWire<R>(detail::probeName(src, ctx, field));
    w.assign().reads(src) = [get = std::forward<F>(get)](auto s) { return get(std::get<0>(s)); };
    return w;
}

#else

template <Readable Src, class F>
void probe(Src&, const std::string&, F&&) {}

#endif

}  // namespace wolvicmod
