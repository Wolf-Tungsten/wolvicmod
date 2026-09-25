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
#include "wolvicmod/prefab/dec.h"

namespace wolvicmod::prefab {

template <class T, uint32_t N = 1>
class ValidPipe : public Module {
public:
    static_assert(N >= 1, "ValidPipe requires N >= 1");
    using DecT = Dec<T>;

    struct Stage {
        bool valid = false;  // RegNext(init false)
        T bits{};            // RegEnable 无复位：按 Verilator 两态语义取零值

        bool operator==(const Stage&) const = default;
    };
    using StageArr = std::array<Stage, N>;

    IN(bool, clk);
    IN(DecT, enq);   // Valid-only 入口：无反压
    OUT(DecT, deq);

    REG(StageArr, stages);

    ValidPipe() {
        deq.assign().reads(stages) = [](auto src) {
            auto [stages] = src;
            DecT o;
            o.valid = stages[N - 1].valid;
            o.bits = stages[N - 1].bits;
            return o;
        };
        stages.update().on(posedge(clk)).reads(stages, enq) = [](auto src) {
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
