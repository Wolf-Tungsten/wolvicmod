// prefab ValidPipe<T, N> 单测：chisel3.util.Pipe 拍级对齐。
// 覆盖：valid 延迟拍数精确（N=1 与 N=3）；stall（valid 拉低）时气泡逐拍
// 传播且 bits 保持（RegEnable：上级 valid 为低时不前进）。

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>
#include <wolvicmod/prefab/pipe.h>

#include "test_prefab_common.h"

using namespace wolvicmod;
using namespace wolvicmod::prefab;
using namespace prefabtest;

namespace {

TEST_CASE("prefab ValidPipe<1>: valid 精确延迟 1 拍") {
    ValidPipe<uint32_t, 1> top;
    top.elaborate();
    top.enq.set({false, 0});

    comb(top);
    CHECK(top.deq.get().valid == false);
    edge(top);

    top.enq.set({true, 5});
    comb(top);
    CHECK(top.deq.get().valid == false);   // 本拍还不到
    edge(top);
    top.enq.set({false, 0});
    comb(top);
    CHECK(top.deq.get().valid == true);    // 恰好 1 拍后
    CHECK(top.deq.get().bits == 5);
    edge(top);
    comb(top);
    CHECK(top.deq.get().valid == false);   // 只停留 1 拍
    edge(top);
}

TEST_CASE("prefab ValidPipe<3>: valid 精确延迟 3 拍，背靠背两拍") {
    ValidPipe<uint32_t, 3> top;
    top.elaborate();
    top.enq.set({false, 0});
    comb(top);
    edge(top);

    // 拍 0 灌 A=10，拍 1 灌 B=11，之后停灌
    top.enq.set({true, 10});
    comb(top);
    CHECK(top.deq.get().valid == false);
    edge(top);
    top.enq.set({true, 11});
    comb(top);
    CHECK(top.deq.get().valid == false);   // 拍 1：未到
    edge(top);
    top.enq.set({false, 0});
    comb(top);
    CHECK(top.deq.get().valid == false);   // 拍 2：未到
    edge(top);
    comb(top);
    CHECK(top.deq.get().valid == true);    // 拍 3：A 到达（恰好 3 拍）
    CHECK(top.deq.get().bits == 10);
    edge(top);
    comb(top);
    CHECK(top.deq.get().valid == true);    // 拍 4：B 到达
    CHECK(top.deq.get().bits == 11);
    edge(top);
    comb(top);
    CHECK(top.deq.get().valid == false);   // 拍 5：空
    edge(top);
}

TEST_CASE("prefab ValidPipe<3>: stall 时气泡传播、bits 不被覆盖") {
    ValidPipe<uint32_t, 3> top;
    top.elaborate();
    top.enq.set({false, 0});
    comb(top);
    edge(top);

    // 拍 0 灌 A=77；拍 1、2 stall；拍 3 灌 B=88
    top.enq.set({true, 77});
    comb(top);
    edge(top);
    top.enq.set({false, 0});
    comb(top);
    CHECK(top.deq.get().valid == false);   // 拍 1
    edge(top);
    comb(top);
    CHECK(top.deq.get().valid == false);   // 拍 2
    edge(top);
    // 拍 3：A 恰好到达；同拍灌 B —— A 的 bits 不能被 B 覆盖
    top.enq.set({true, 88});
    comb(top);
    CHECK(top.deq.get().valid == true);
    CHECK(top.deq.get().bits == 77);
    edge(top);
    // 拍 4、5：A 后面的两个气泡（stall 的两拍）传到了出口
    top.enq.set({false, 0});
    comb(top);
    CHECK(top.deq.get().valid == false);   // 气泡
    edge(top);
    comb(top);
    CHECK(top.deq.get().valid == false);   // 气泡
    edge(top);
    // 拍 6：B 到达
    comb(top);
    CHECK(top.deq.get().valid == true);
    CHECK(top.deq.get().bits == 88);
    edge(top);
    comb(top);
    CHECK(top.deq.get().valid == false);
    edge(top);
}

}  // namespace
