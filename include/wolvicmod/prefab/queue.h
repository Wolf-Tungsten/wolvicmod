#pragma once

// Queue<T, N, Flow, Pipe>：chisel3.util.Queue 的拍级对齐实现（拍平版）。
//
// 状态与 chisel 一致：ram[N] + 模 N 的 enq_ptr/deq_ptr + maybe_full。
//   empty = ptr_match && !maybe_full；full = ptr_match && maybe_full
//   do_enq 时 ram[enq_ptr]←enq.bits、enq_ptr++；do_deq 时 deq_ptr++；
//   do_enq != do_deq 时 maybe_full ← do_enq。
// Flow=true（chisel flow）：空时 deq 同拍直通 enq——deq.valid = !empty || enq.valid，
//   deq.bits = empty ? enq.bits : ram[deq_ptr]。注意 chisel 源码（Queue.scala
//   when(empty) 分支）在空直通被消费（deq_rdy 拉高）的那拍强制 do_enq/do_deq 均
//   为 false：直通数据不写入 ram、指针不动，ram 里不留"幻影"拷贝。
// Pipe=true（chisel pipe）：enq_rdy = !full || deq_rdy。满且 deq_rdy 的同拍，
//   读写落在同一 ram 地址（enq_ptr==deq_ptr），读侧见旧值——REG 当前值 NBA
//   提交，组合读拿 committed 内容，与原 Mem 实现天然一致。
// 附带 count 输出（占位数），便于使用方做水位逻辑。
//
// 拍平注记（perf-breakdown §19）：ram 与三个指针合并为单个 REG(QState)，
// do_enq/do_deq/empty/full 等中间量不再铺 wire（在消费点内联），4 条 NBA
// update 融合为 1 条并由 w_chg = do_enq || do_deq 门控——静止拍整条 update
// 休眠。每实例动作数 12 → 5（deq / enq_rdy / count / w_chg / update）。

#include <array>
#include <cstdint>

#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/valid.h"

namespace wolvicmod::prefab {

template <class T, uint32_t N, bool Flow = false, bool Pipe = false>
class Queue : public Module {
public:
    static_assert(N >= 1, "Queue requires N >= 1");
    using ValidT = Valid<T>;

    struct QState {
        std::array<T, N> ram{};
        uint32_t enq_ptr = 0;
        uint32_t deq_ptr = 0;
        bool maybe_full = false;

        bool operator==(const QState&) const = default;
    };

    IN(bool, clk);
    IN(ValidT, enq);
    OUT(bool, enq_rdy);
    OUT(ValidT, deq);
    IN(bool, deq_rdy);
    OUT(uint32_t, count);

    REG(QState, st);
    WIRE(bool, w_chg);

    Queue() {
        if constexpr (Flow) {
            deq.assign().reads(st, enq) = [](auto src) {
                auto [st, enq] = src;
                const bool empty = st.enq_ptr == st.deq_ptr && !st.maybe_full;
                ValidT o;
                o.valid = !empty || enq.valid;
                o.bits = empty ? enq.bits : st.ram[st.deq_ptr];
                return o;
            };
        } else {
            deq.assign().reads(st) = [](auto src) {
                auto [st] = src;
                ValidT o;
                o.valid = !(st.enq_ptr == st.deq_ptr && !st.maybe_full);
                o.bits = st.ram[st.deq_ptr];
                return o;
            };
        }

        if constexpr (Pipe) {
            enq_rdy.assign().reads(st, deq_rdy) = [](auto src) {
                auto [st, deq_rdy] = src;
                const bool full = st.enq_ptr == st.deq_ptr && st.maybe_full;
                return !full || deq_rdy;
            };
        } else {
            enq_rdy.assign().reads(st) = [](auto src) {
                auto [st] = src;
                return !(st.enq_ptr == st.deq_ptr && st.maybe_full);
            };
        }

        // chisel count：ptr_match 时 0 或 N；否则 (enq_ptr - deq_ptr) mod N。
        count.assign().reads(st) = [](auto src) -> uint32_t {
            auto [st] = src;
            if (st.enq_ptr == st.deq_ptr) return st.maybe_full ? N : 0u;
            return st.enq_ptr > st.deq_ptr ? st.enq_ptr - st.deq_ptr
                                           : st.enq_ptr + N - st.deq_ptr;
        };

        // 状态变更门：do_enq || do_deq。静止拍 update 不激活，compute 与
        // 提交（整状态比较/拷贝）全部跳过。
        w_chg.assign().reads(st, enq, deq_rdy) = [](auto src) {
            auto [st, enq, deq_rdy] = src;
            const auto f = fire(st, enq, deq_rdy);
            return f.do_enq || f.do_deq;
        };

        st.update().on(posedge(clk)).en(w_chg).reads(st, enq, deq_rdy) = [](auto src) {
            auto [st, enq, deq_rdy] = src;
            QState next = st;
            const auto f = fire(st, enq, deq_rdy);
            if (f.do_enq) {
                next.ram[st.enq_ptr] = enq.bits;
                next.enq_ptr = st.enq_ptr + 1 == N ? 0u : st.enq_ptr + 1;
            }
            if (f.do_deq) next.deq_ptr = st.deq_ptr + 1 == N ? 0u : st.deq_ptr + 1;
            if (f.do_enq != f.do_deq) next.maybe_full = f.do_enq;
            return next;
        };
    }

private:
    struct Fire {
        bool do_enq;
        bool do_deq;
    };

    // chisel flow 在 empty 分支强制 do_deq := false.B，且 deq.ready 时
    // do_enq := false.B（空直通被消费的那拍队列内部不动）。
    static Fire fire(const QState& st, const ValidT& enq, bool deq_rdy) {
        const bool ptr_match = st.enq_ptr == st.deq_ptr;
        const bool empty = ptr_match && !st.maybe_full;
        const bool full = ptr_match && st.maybe_full;
        const bool rdy = Pipe ? (!full || deq_rdy) : !full;
        bool do_enq = enq.valid && rdy;
        bool do_deq = deq_rdy && (Flow ? (!empty || enq.valid) : !empty);
        if constexpr (Flow) {
            if (empty) {
                do_deq = false;
                if (deq_rdy) do_enq = false;
            }
        }
        return {do_enq, do_deq};
    }
};

}  // namespace wolvicmod::prefab
