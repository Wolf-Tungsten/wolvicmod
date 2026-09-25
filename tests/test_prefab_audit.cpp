// prefab 读集对账（§6.4）：auditOn 下驱动各元件若干拍，任何 lambda 经框架
// 读路径访问未声明实体都会立刻报错；同时打开 assertUpdateMutexOn（预制菜
// 每个状态只挂一条 Update，不应出现同 round 多激活）。

#include <array>

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>
#include <wolvicmod/prefab/prefab.h>

#include "test_prefab_common.h"

using namespace wolvicmod;
using namespace wolvicmod::prefab;
using namespace prefabtest;

namespace {

TEST_CASE("prefab: auditOn + assertUpdateMutexOn 全元件巡查") {
    {
        Queue<uint32_t, 4> top;
        top.elaborate();
        top.auditOn();
        top.assertUpdateMutexOn();
        for (uint32_t c = 0; c < 24; ++c) {
            top.enq.set({(c & 3u) != 0, c});
            top.deq_rdy.set((c & 1u) != 0);
            cycle(top);
        }
    }
    {
        Queue<uint32_t, 2, true, true> top;
        top.elaborate();
        top.auditOn();
        top.assertUpdateMutexOn();
        for (uint32_t c = 0; c < 24; ++c) {
            top.enq.set({(c & 1u) != 0, c});
            top.deq_rdy.set((c & 3u) != 0);
            cycle(top);
        }
    }
    {
        FixedArb<uint32_t, 4> top;
        top.elaborate();
        top.auditOn();
        top.assertUpdateMutexOn();
        std::array<Dec<uint32_t>, 4> ins{};
        for (uint32_t c = 0; c < 24; ++c) {
            for (uint32_t i = 0; i < 4; ++i) ins[i] = {((c >> i) & 1u) != 0, c * 10 + i};
            top.in.set(ins);
            top.out_rdy.set((c & 1u) != 0);
            cycle(top);
        }
    }
    {
        RRArb<uint32_t, 4> top;
        top.elaborate();
        top.auditOn();
        top.assertUpdateMutexOn();
        std::array<Dec<uint32_t>, 4> ins{};
        for (uint32_t c = 0; c < 24; ++c) {
            for (uint32_t i = 0; i < 4; ++i) ins[i] = {((c >> i) & 1u) != 0, c * 10 + i};
            top.in.set(ins);
            top.out_rdy.set((c & 1u) != 0);
            cycle(top);
        }
    }
    {
        ValidPipe<uint32_t, 3> top;
        top.elaborate();
        top.auditOn();
        top.assertUpdateMutexOn();
        for (uint32_t c = 0; c < 24; ++c) {
            top.enq.set({(c & 3u) != 0, c});
            cycle(top);
        }
    }
}

}  // namespace
