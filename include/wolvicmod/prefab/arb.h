#pragma once

// 仲裁器：FixedArb（chisel3 Arbiter，固定优先级）与 RRArb（chisel3
// RRArbiter，轮转优先级）——chisel3 标准库语义，框架通用。
// （XiangShan 生态的 VipArbiter / QoS 仲裁 / Alloc 属项目侧元件，见
//  proj-xiangshan-l3/prefab/xsarb.h。）
//
// 统一端口形态（阵列端口）：输入侧一路 In<std::array<Dec<T>,N>> in + 一路
// Out<std::array<bool,N>> in_rdy；输出侧 Out<Dec<T>> out + In<bool> out_rdy；
// 另有 Out<uint32_t> chosen（当前授权索引）。clk 端口仅为接口统一；纯组合的
// FixedArb 不采样它。

#include <array>
#include <cstdint>

#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/dec.h"

namespace wolvicmod::prefab {

// ---------------- FixedArb：chisel3 Arbiter（固定优先级，in[0] 最高） ----------------

template <class T, uint32_t N>
class FixedArb : public Module {
public:
    static_assert(N >= 1);
    using DecT = Dec<T>;
    using InArr = std::array<DecT, N>;  // 宏参数含逗号，先取别名
    using RdyArr = std::array<bool, N>;

    IN(bool, clk);  // 纯组合元件，clk 仅为接口统一保留
    IN(InArr, in);
    OUT(RdyArr, in_rdy);
    OUT(DecT, out);
    IN(bool, out_rdy);
    OUT(uint32_t, chosen);

    FixedArb() {
        chosen.assign().reads(in) = [](auto src) -> uint32_t {
            auto [in] = src;
            for (uint32_t i = 0; i < N; ++i)
                if (in[i].valid) return i;
            return N - 1;  // chisel 默认 (n-1).U
        };
        out.assign().reads(in, chosen) = [](auto src) {
            auto [in, chosen] = src;
            DecT o;
            for (uint32_t i = 0; i < N; ++i) o.valid = o.valid || in[i].valid;
            o.bits = in[chosen].bits;
            return o;
        };
        in_rdy.assign().reads(in, out_rdy) = [](auto src) {
            auto [in, out_rdy] = src;
            // chisel Arbiter 的 ready 不门控自身 valid：本路 ready = out_rdy &&
            // 没有优先级更高的 valid 在前面（低于首个 valid 的路即使无效也 ready，
            // 不会多 fire——它们无效）。对拍实证见 FixedArbRef_n4 的生成 SV。
            RdyArr rdy{};
            bool higherValid = false;  // OR of in[j].valid for j < i
            for (uint32_t i = 0; i < N; ++i) {
                rdy[i] = out_rdy && !higherValid;
                higherValid = higherValid || in[i].valid;
            }
            return rdy;
        };
    }
};

// ---------------- RRArb：chisel3 RRArbiter ----------------
// last_grant 初值 0（chisel 为 RegEnable 无复位，RTL 初值不定；按 Verilator
// 两态语义取 0）。out.fire 时 last_grant ← chosen。

template <class T, uint32_t N>
class RRArb : public Module {
public:
    static_assert(N >= 1);
    using DecT = Dec<T>;
    using InArr = std::array<DecT, N>;  // 宏参数含逗号，先取别名
    using RdyArr = std::array<bool, N>;

    IN(bool, clk);
    IN(InArr, in);
    OUT(RdyArr, in_rdy);
    OUT(DecT, out);
    IN(bool, out_rdy);
    OUT(uint32_t, chosen);

    REG(uint32_t, last_grant);
    WIRE(bool, w_out_fire);

    RRArb() {
        // 两遍优先级：先扫 last_grant+1..N-1，无 valid 再扫 0..last_grant。
        chosen.assign().reads(in, last_grant) = [](auto src) -> uint32_t {
            auto [in, last_grant] = src;
            for (uint32_t k = 1; k <= N; ++k) {
                uint32_t idx = (last_grant + k) % N;
                if (in[idx].valid) return idx;
            }
            return N - 1;
        };
        out.assign().reads(in, chosen) = [](auto src) {
            auto [in, chosen] = src;
            DecT o;
            for (uint32_t i = 0; i < N; ++i) o.valid = o.valid || in[i].valid;
            o.bits = in[chosen].bits;
            return o;
        };
        in_rdy.assign().reads(in, last_grant, out_rdy) = [](auto src) {
            auto [in, last_grant, out_rdy] = src;
            // chisel RRArbiter（LockingRRArbiterLike）的 ready：不门控自身 valid。
            // 两遍结构：第一遍 validMask = valid && (索引 > last_grant)，本路在
            // 第一遍内不被更高优先候选（validMask 中索引更小的）抢占即可 ready；
            // 第一遍全空时退回第二遍（原始 valids 固定优先级）。对拍实证见
            // RRArbRef_n4 的生成 SV（RRArbiter4_UInt32.sv）。
            bool anyVM = false;
            for (uint32_t i = 0; i < N; ++i)
                anyVM = anyVM || (in[i].valid && i > last_grant);
            RdyArr rdy{};
            bool vmBelow = false;  // OR of vm[j] = in[j].valid && j>last_grant, j < i
            bool vBelow = false;   // OR of in[j].valid for j < i
            for (uint32_t i = 0; i < N; ++i) {
                const bool pass1 = (i > last_grant) && !vmBelow;
                const bool pass2 = !anyVM && !vBelow;
                rdy[i] = out_rdy && (pass1 || pass2);
                if (in[i].valid) {
                    vBelow = true;
                    if (i > last_grant) vmBelow = true;
                }
            }
            return rdy;
        };
        w_out_fire.assign().reads(out, out_rdy) = [](auto src) {
            auto [out, out_rdy] = src;
            return out.valid && out_rdy;
        };
        last_grant.update().on(posedge(clk)).en(w_out_fire).reads(chosen) = [](auto src) {
            auto [chosen] = src;
            return chosen;
        };
    }
};

}  // namespace wolvicmod::prefab
