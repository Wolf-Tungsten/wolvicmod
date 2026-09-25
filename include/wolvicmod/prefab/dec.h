#pragma once

// 预制菜（prefab）可复用时序元件库：通道约定。
//
// Decoupled 通道 = 两个端口：载荷用合体的 Dec<T>（valid + bits），反压用一根
// 独立的 bool 端口，命名为 <通道名>_rdy；fire = valid && rdy。
//   生产者侧：Out<Dec<T>> xxx + In<bool>  xxx_rdy
//   消费者侧：In<Dec<T>>  xxx + Out<bool> xxx_rdy
// Valid-only 通道（无反压，如 SRAM 响应、ValidPipe 出口）：单个 Out<Dec<T>>，
// 没有配套 rdy 端口，valid 拉高即传输。

namespace wolvicmod::prefab {

// Decoupled 载荷。operator== 供组合环稳态迭代判定（§4.3）与快照使用；
// T 无 == 时该比较为删除态，与框架对无 == 类型的处理一致。
template <class T>
struct Dec {
    bool valid = false;
    T bits{};

    bool operator==(const Dec&) const = default;
};

}  // namespace wolvicmod::prefab
