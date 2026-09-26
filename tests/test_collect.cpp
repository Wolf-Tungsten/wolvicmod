// core/collect.h：combine 原语（N 个同值类型可读实体 → 一路 std::array 值
// 信号）与 collectPorts 复合便捷。涵盖子模块端口数组、Reg 指针数组、Out
// 目标、以及跨层级合并的可见性审计（§2.2）。

#include <array>
#include <cstdint>
#include <string>

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>

using namespace wolvicmod;

namespace {

// 子模块：一个 In 一个 Out（直通），供指针数组持有。
struct Cell : Module {
    IN(uint32_t, din);
    OUT(uint32_t, dout);

    Cell() { dout = din; }
};

using U32x4 = std::array<uint32_t, 4>;  // 宏只收 2 参，含逗号类型先取别名
using U32x3 = std::array<uint32_t, 3>;
using U32x1 = std::array<uint32_t, 1>;

// 父模块：根输入数组散布到 4 个 Cell → combine 回数组 Wire → 求和 Out。
struct CellPool : Module {
    IN(U32x4, din);
    WIRE(U32x4, lanes);
    OUT(uint32_t, sum);
    std::array<Cell*, 4> cells{};

    CellPool() {
        for (uint32_t i = 0; i < 4; ++i) {
            cells[i] = &createChildModule<Cell>("cell_" + std::to_string(i));
            cells[i]->din.assign().reads(din) = [i](auto src) {  // 散布：普通循环
                auto [a] = src;
                return a[i];
            };
        }
        std::array<Out<uint32_t>*, 4> outs{};
        for (uint32_t i = 0; i < 4; ++i) outs[i] = &cells[i]->dout;
        combine(lanes, outs);
        sum.assign().reads(lanes) = [](auto src) {
            auto [a] = src;
            return a[0] + a[1] + a[2] + a[3];
        };
    }
};

// Reg 指针数组 → combine；目标为 Out（dst 泛化到 Signal）。
struct RegPool : Module {
    IN(bool, clk);
    OUT(U32x3, arr_out);
    std::array<Reg<uint32_t>*, 3> regs{};

    RegPool() {
        for (uint32_t i = 0; i < 3; ++i) {
            regs[i] = &createReg<uint32_t>("r_" + std::to_string(i));
            regs[i]->update().on(posedge(clk)).reads(*regs[i]) = [i](auto src) {
                auto [c] = src;
                return c + 1 + i;  // r_i 每拍 +（i+1）
            };
        }
        combine(arr_out, regs);
    }
};

// 跨层级：父模块合并孙模块端口（§2.2 只放行直接子模块端口）。
struct GrandChild : Module {
    OUT(uint32_t, dout);
    GrandChild() { dout = 7; }
};
struct Mid : Module {
    SUB(GrandChild, gc);
};
struct BadTop : Module {
    SUB(Mid, mid);
    WIRE(U32x1, lanes);
    BadTop() {
        std::array<Out<uint32_t>*, 1> outs{&mid.gc.dout};
        combine(lanes, outs);
    }
};

}  // namespace

TEST_CASE("collect: combine 子模块端口数组并逐拍反映输入") {
    CellPool top;
    top.elaborate();
    top.din.set({10, 20, 30, 40});
    top.eval();
    CHECK(top.lanes.get() == std::array<uint32_t, 4>{10, 20, 30, 40});
    CHECK(top.sum.get() == 100);

    top.din.set({10, 20, 99, 40});
    top.eval();
    CHECK(top.lanes.get()[2] == 99);
    CHECK(top.sum.get() == 169);
}

TEST_CASE("collect: combine Reg 指针数组到 Out 目标") {
    RegPool top;
    top.elaborate();
    top.clk.set(0);
    top.eval();
    CHECK(top.arr_out.get() == std::array<uint32_t, 3>{0, 0, 0});
    top.clk.set(1);
    top.eval();  // posedge：r_i <- i+1
    CHECK(top.arr_out.get() == std::array<uint32_t, 3>{1, 2, 3});
    top.clk.set(0);
    top.eval();
    top.clk.set(1);
    top.eval();
    CHECK(top.arr_out.get() == std::array<uint32_t, 3>{2, 4, 6});
}

TEST_CASE("collect: 跨层级合并孙模块端口被可见性审计拒绝") {
    CHECK_THROWS_AS(BadTop{}, Error);
}
