// prefab 仲裁器单测：FixedArb（chisel3 Arbiter）/ RRArb（chisel3 RRArbiter）。
// 覆盖：固定优先级；RR 轮转序（连续 fire 时 chosen 依次推进、跳过无效、
// 不 fire 不推进）。阵列端口：整路 in.set(arr)，断言 in_rdy.get()[i]。

#include <array>

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>
#include <wolvicmod/prefab/arb.h>

#include "test_prefab_common.h"

using namespace wolvicmod;
using namespace wolvicmod::prefab;
using namespace prefabtest;

namespace {

using InArr4 = std::array<Dec<uint32_t>, 4>;

TEST_CASE("prefab FixedArb: in[0] 优先级最高，全组合") {
    FixedArb<uint32_t, 4> top;
    top.elaborate();
    InArr4 ins{};  // 全无效
    top.in.set(ins);
    top.out_rdy.set(false);

    // 全 valid：chosen=0
    for (uint32_t i = 0; i < 4; ++i) ins[i] = {true, 100 + i};
    top.in.set(ins);
    top.out_rdy.set(true);
    comb(top);
    CHECK(top.out.get().valid == true);
    CHECK(top.chosen.get() == 0);
    CHECK(top.out.get().bits == 100);
    CHECK(top.in_rdy.get()[0] == true);
    CHECK(top.in_rdy.get()[1] == false);
    CHECK(top.in_rdy.get()[2] == false);
    CHECK(top.in_rdy.get()[3] == false);
    edge(top);

    // in[0] 无效 → chosen=1
    ins[0].valid = false;
    top.in.set(ins);
    comb(top);
    CHECK(top.chosen.get() == 1);
    CHECK(top.out.get().bits == 101);
    CHECK(top.in_rdy.get()[1] == true);
    // chisel Arbiter 的 ready 不门控自身 valid：本路 ready = out_rdy &&
    // 前面没有 valid——in[0] 无效但前面无 valid → ready=1；in[2]/in[3] 前面
    // 有 in[1] valid → 0（对拍实证：FixedArbRef_n4 生成 SV）
    CHECK(top.in_rdy.get()[0] == true);
    CHECK(top.in_rdy.get()[2] == false);
    CHECK(top.in_rdy.get()[3] == false);
    edge(top);

    // 只剩 in[3] → chosen=3
    ins[1].valid = false;
    ins[2].valid = false;
    top.in.set(ins);
    comb(top);
    CHECK(top.chosen.get() == 3);
    CHECK(top.out.get().bits == 103);
    CHECK(top.in_rdy.get()[3] == true);
    edge(top);

    // out_rdy=0 → 无任何 in_rdy
    top.out_rdy.set(false);
    comb(top);
    CHECK(top.out.get().valid == true);
    CHECK(top.chosen.get() == 3);
    for (uint32_t i = 0; i < 4; ++i) CHECK(top.in_rdy.get()[i] == false);
    edge(top);

    // 全无效 → out.valid=0
    ins[3].valid = false;
    top.in.set(ins);
    comb(top);
    CHECK(top.out.get().valid == false);
    edge(top);
}

TEST_CASE("prefab RRArb: 连续 fire 时 chosen 依次推进、跳过无效") {
    RRArb<uint32_t, 4> top;
    top.elaborate();
    InArr4 ins{};
    for (uint32_t i = 0; i < 4; ++i) ins[i] = {true, 200 + i};
    top.in.set(ins);
    top.out_rdy.set(true);

    // last_grant 初值 0 → 首轮从 1 开始：1,2,3,0,1,2,3,0
    for (uint32_t expect : {1u, 2u, 3u, 0u, 1u, 2u, 3u, 0u}) {
        comb(top);
        CHECK(top.chosen.get() == expect);
        CHECK(top.out.get().valid == true);
        CHECK(top.out.get().bits == 200 + expect);
        CHECK(top.in_rdy.get()[expect] == true);
        edge(top);
    }
}

TEST_CASE("prefab RRArb: 跳过无效输入") {
    RRArb<uint32_t, 4> top;
    top.elaborate();
    InArr4 ins{};
    ins[0] = {true, 10};
    ins[2] = {true, 12};
    top.in.set(ins);
    top.out_rdy.set(true);

    // last_grant=0 → 扫 1..3：中 2；再扫从 3：无 → 绕回 0..2：中 0；再 2…
    // RR ready 语义（不门控自身 valid，两遍结构）逐拍钉住：
    //   valids{0,2}、lg=0：vm={2}，ready={0,1,1,0}（in1 无效但 ready——
    //   它在第一遍中不被任何候选抢占；in3 被 vm2 抢占）；
    //   fire 后 lg=2：vm 空 → 退回原始优先级，ready={1,0,0,0}（in0 中）
    for (uint32_t expect : {2u, 0u, 2u, 0u, 2u}) {
        comb(top);
        CHECK(top.chosen.get() == expect);
        CHECK(top.out.get().bits == 10 + expect);
        edge(top);
    }

    // 显式钉住 ready 两遍结构（valids={0,2}）
    RRArb<uint32_t, 4> top2;
    top2.elaborate();
    InArr4 ins2{};
    ins2[0] = {true, 10};
    ins2[2] = {true, 12};
    top2.in.set(ins2);
    top2.out_rdy.set(true);
    comb(top2);
    CHECK(top2.chosen.get() == 2);
    CHECK(top2.in_rdy.get()[0] == false);  // 不在 validMask（lg=0，索引 0 不大于 0）
    CHECK(top2.in_rdy.get()[1] == true);   // 无效但第一遍内无候选抢占它
    CHECK(top2.in_rdy.get()[2] == true);   // 中
    CHECK(top2.in_rdy.get()[3] == false);  // 被 vm2 抢占
    edge(top2);
    comb(top2);
    CHECK(top2.chosen.get() == 0);         // lg=2，vm 空 → 第二遍
    // ready={1,0,0,1}：in0 走第二遍（第一遍空）；in3 走第一遍（在 validMask 内
    // 且无更小索引候选抢占——它无效不会 fire）；in1/in2 被 in0 抢占第二遍
    CHECK(top2.in_rdy.get()[0] == true);
    CHECK(top2.in_rdy.get()[1] == false);
    CHECK(top2.in_rdy.get()[2] == false);
    CHECK(top2.in_rdy.get()[3] == true);
    edge(top2);
}

TEST_CASE("prefab RRArb: out_rdy 拉低时不 fire、last_grant 保持") {
    RRArb<uint32_t, 4> top;
    top.elaborate();
    InArr4 ins{};
    for (uint32_t i = 0; i < 4; ++i) ins[i] = {true, 300 + i};
    top.in.set(ins);
    top.out_rdy.set(false);

    // 反压 3 拍：chosen 恒为 1（last_grant 不动），无 in_rdy
    for (int c = 0; c < 3; ++c) {
        comb(top);
        CHECK(top.chosen.get() == 1);
        CHECK(top.out.get().valid == true);
        for (uint32_t i = 0; i < 4; ++i) CHECK(top.in_rdy.get()[i] == false);
        edge(top);
    }
    // 放行：fire chosen=1，之后 last_grant=1 → chosen=2
    top.out_rdy.set(true);
    comb(top);
    CHECK(top.chosen.get() == 1);
    CHECK(top.in_rdy.get()[1] == true);
    edge(top);
    comb(top);
    CHECK(top.chosen.get() == 2);
    edge(top);

    // 只剩 in[3] 单独 valid：每拍都中 3（扫描绕回）
    for (uint32_t i = 0; i < 3; ++i) ins[i].valid = false;
    top.in.set(ins);
    for (int c = 0; c < 3; ++c) {
        comb(top);
        CHECK(top.chosen.get() == 3);
        edge(top);
    }
}

}  // namespace
