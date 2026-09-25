#pragma once

// Queue<T, N, Flow, Pipe>：chisel3.util.Queue 的拍级对齐实现。
//
// 状态与 chisel 一致：Mem<T,N> ram + 模 N 的 enq_ptr/deq_ptr + maybe_full。
//   empty = ptr_match && !maybe_full；full = ptr_match && maybe_full
//   do_enq 时 ram[enq_ptr]←enq.bits、enq_ptr++；do_deq 时 deq_ptr++；
//   do_enq != do_deq 时 maybe_full ← do_enq。
// Flow=true（chisel flow）：空时 deq 同拍直通 enq——deq.valid = !empty || enq.valid，
//   deq.bits = empty ? enq.bits : ram[deq_ptr]。注意 chisel 源码（Queue.scala
//   when(empty) 分支）在空直通被消费（deq_rdy 拉高）的那拍强制 do_enq/do_deq 均
//   为 false：直通数据不写入 ram、指针不动，ram 里不留"幻影"拷贝。
// Pipe=true（chisel pipe）：enq_rdy = !full || deq_rdy。满且 deq_rdy 的同拍，
//   读写落在同一 ram 地址（enq_ptr==deq_ptr），读侧见旧值——wolvicmod Mem 组合
//   读 committed 内容 + 写 NBA 提交，天然一致。
// 附带 count 输出（占位数），便于使用方做水位逻辑。

#include <cstdint>

#include "wolvicmod/core/edge.h"
#include "wolvicmod/core/module.h"
#include "wolvicmod/prefab/dec.h"

namespace wolvicmod::prefab {

template <class T, uint32_t N, bool Flow = false, bool Pipe = false>
class Queue : public Module {
public:
    static_assert(N >= 1, "Queue requires N >= 1");
    using DecT = Dec<T>;

    IN(bool, clk);
    IN(DecT, enq);
    OUT(bool, enq_rdy);
    OUT(DecT, deq);
    IN(bool, deq_rdy);
    OUT(uint32_t, count);

    MEM(T, N, ram);
    REG(uint32_t, enq_ptr);
    REG(uint32_t, deq_ptr);
    REG(bool, maybe_full);

    WIRE(bool, w_empty);
    WIRE(bool, w_full);
    WIRE(bool, w_do_enq);
    WIRE(bool, w_do_deq);
    WIRE(bool, w_ptr_chg);

    Queue() {
        w_empty.assign().reads(enq_ptr, deq_ptr, maybe_full) = [](auto src) {
            auto [enq_ptr, deq_ptr, maybe_full] = src;
            return enq_ptr == deq_ptr && !maybe_full;
        };
        w_full.assign().reads(enq_ptr, deq_ptr, maybe_full) = [](auto src) {
            auto [enq_ptr, deq_ptr, maybe_full] = src;
            return enq_ptr == deq_ptr && maybe_full;
        };

        if constexpr (Flow) {
            deq.assign().reads(w_empty, enq, ram, deq_ptr) = [](auto src) {
                auto [w_empty, enq, ram, deq_ptr] = src;
                DecT o;
                o.valid = !w_empty || enq.valid;
                o.bits = w_empty ? enq.bits : ram[deq_ptr];
                return o;
            };
        } else {
            deq.assign().reads(w_empty, ram, deq_ptr) = [](auto src) {
                auto [w_empty, ram, deq_ptr] = src;
                DecT o;
                o.valid = !w_empty;
                o.bits = ram[deq_ptr];
                return o;
            };
        }

        if constexpr (Pipe) {
            enq_rdy.assign().reads(w_full, deq_rdy) = [](auto src) {
                auto [w_full, deq_rdy] = src;
                return !w_full || deq_rdy;
            };
        } else {
            enq_rdy.assign().reads(w_full) = [](auto src) {
                auto [w_full] = src;
                return !w_full;
            };
        }

        // do_enq/do_deq 默认 = 两侧 fire；chisel flow 在 empty 分支强制
        // do_deq := false.B，且 deq.ready 时 do_enq := false.B（空直通被消费
        // 的那拍队列内部不动）。
        if constexpr (Flow) {
            w_do_enq.assign().reads(enq, enq_rdy, w_empty, deq_rdy) = [](auto src) {
                auto [enq, enq_rdy, w_empty, deq_rdy] = src;
                return enq.valid && enq_rdy && !(w_empty && deq_rdy);
            };
            w_do_deq.assign().reads(deq, deq_rdy, w_empty) = [](auto src) {
                auto [deq, deq_rdy, w_empty] = src;
                return deq.valid && deq_rdy && !w_empty;
            };
        } else {
            w_do_enq.assign().reads(enq, enq_rdy) = [](auto src) {
                auto [enq, enq_rdy] = src;
                return enq.valid && enq_rdy;
            };
            w_do_deq.assign().reads(deq, deq_rdy) = [](auto src) {
                auto [deq, deq_rdy] = src;
                return deq.valid && deq_rdy;
            };
        }
        w_ptr_chg.assign().reads(w_do_enq, w_do_deq) = [](auto src) {
            auto [w_do_enq, w_do_deq] = src;
            return w_do_enq != w_do_deq;
        };

        ram.update().on(posedge(clk)).addr(enq_ptr).en(w_do_enq).reads(enq) = [](auto src) {
            auto [enq] = src;
            return enq.bits;
        };
        enq_ptr.update().on(posedge(clk)).en(w_do_enq).reads(enq_ptr) = [](auto src) {
            auto [enq_ptr] = src;
            return enq_ptr + 1 == N ? 0u : enq_ptr + 1;
        };
        deq_ptr.update().on(posedge(clk)).en(w_do_deq).reads(deq_ptr) = [](auto src) {
            auto [deq_ptr] = src;
            return deq_ptr + 1 == N ? 0u : deq_ptr + 1;
        };
        maybe_full.update().on(posedge(clk)).en(w_ptr_chg).reads(w_do_enq) = [](auto src) {
            auto [w_do_enq] = src;
            return w_do_enq;
        };

        // chisel count：ptr_match 时 0 或 N；否则 (enq_ptr - deq_ptr) mod N。
        count.assign().reads(w_full, enq_ptr, deq_ptr) = [](auto src) -> uint32_t {
            auto [w_full, enq_ptr, deq_ptr] = src;
            if (w_full) return N;
            return enq_ptr >= deq_ptr ? enq_ptr - deq_ptr : enq_ptr + N - deq_ptr;
        };
    }
};

}  // namespace wolvicmod::prefab
