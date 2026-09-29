#pragma once

// ValidPipe<T, N>：chisel3.util.Pipe(enqValid, enqBits, N) 的拍级对齐实现
// （Pipe.scala：valid 每级 RegNext(init false)，bits 每级 RegEnable(bits,
// 上级 valid)——上级 valid 为低时 bits 保持）。valid 精确延迟 N 拍，bits 随
// 对应 valid 的那一份前进。
//
// dongjiang 的 Shift（directory/DirectoryBase.scala、data/BeatStorage.scala
// 内的位移标记：x := Cat(fire, x >> 1)，d0 在 MSB、每拍无条件移位、N 拍后到
// LSB）语义上就是本元件的 valid 链（BeatStorage 更是直接例化 chisel3.util.Pipe），
// 不需要独立元件；区别仅在 RTL 把 d0 放在 MSB 编号。

#include <array>
#include <cstdint>

#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/valid.h"

namespace wolvicmod::prefab {

template <class T, uint32_t N = 1>
class ValidPipe : public Module {
public:
    static_assert(N >= 1, "ValidPipe requires N >= 1");
    using ValidT = Valid<T>;

    struct Stage {
        bool valid = false;  // RegNext(init false)
        T bits{};            // RegEnable 无复位：按 Verilator 两态语义取零值

        bool operator==(const Stage&) const = default;
    };
    using StageArr = std::array<Stage, N>;

    IN(bool, clk);
    IN(ValidT, enq);   // Valid-only 入口：无反压
    OUT(ValidT, deq);

    REG(StageArr, stages);
    WIRE(bool, w_live);

    ValidPipe() {
        deq.assign().reads(stages) = [](auto src) {
            auto [stages] = src;
            ValidT o;
            o.valid = stages[N - 1].valid;
            o.bits = stages[N - 1].bits;
            return o;
        };
        // 活性门（perf-breakdown §19）：管线全空且入口无 valid 时整条 update
        // 休眠（此时移位是恒等操作，休眠不改变任何可观察行为）。
        w_live.assign().reads(stages, enq) = [](auto src) {
            auto [stages, enq] = src;
            if (enq.valid) return true;
            for (const auto& s : stages)
                if (s.valid) return true;
            return false;
        };
        stages.update().on(posedge(clk)).en(w_live).reads(stages, enq) = [](auto src) {
            auto [stages, enq] = src;
            StageArr next = stages;
            next[0].valid = enq.valid;
            if (enq.valid) next[0].bits = enq.bits;
            for (uint32_t i = 1; i < N; ++i) {
                next[i].valid = stages[i - 1].valid;
                if (stages[i - 1].valid) next[i].bits = stages[i - 1].bits;
            }
            return next;
        };
    }
};

}  // namespace wolvicmod::prefab
