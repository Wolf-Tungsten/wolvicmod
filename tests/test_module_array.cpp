// core/module.h：createChildModuleArray——批量创建 N 个默认构造子模块（自动
// 起名 "name[i]" 并注册），返回指针数组作为构造函数体内的连线句柄；句柄无需
// 逃逸出构造函数（§4.1），推荐写成构造函数局部变量。

#include <array>
#include <cstdint>

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>

using namespace wolvicmod;

namespace {

struct Cell : Module {
    IN(uint32_t, din);
    OUT(uint32_t, dout);

    Cell() { dout = din; }
};

using U32x4 = std::array<uint32_t, 4>;

// 子模块数组为构造函数局部：根输入数组散布 → Cell 直通 → combine 回 Out。
struct Pool : Module {
    IN(U32x4, din);
    OUT(U32x4, dout);

    Pool() {
        auto cells = createChildModuleArray<Cell, 4>("cm");
        for (uint32_t i = 0; i < 4; ++i)
            cells[i]->din.assign().reads(din) = [i](auto src) {
                auto [a] = src;
                return a[i];
            };
        std::array<Out<uint32_t>*, 4> outs{};
        for (uint32_t i = 0; i < 4; ++i) outs[i] = &cells[i]->dout;
        combine(dout, outs);
    }
};

// 两个同名数组：第二个数组的首元素 "cm[0]" 撞名，创建即抛。
struct DupTop : Module {
    DupTop() {
        auto a = createChildModuleArray<Cell, 2>("cm");
        (void)a;
        auto b = createChildModuleArray<Cell, 2>("cm");
        (void)b;
    }
};

}  // namespace

TEST_CASE("createChildModuleArray: 数组创建、连线与元素独立") {
    Pool top;
    top.elaborate();
    top.din.set({1, 2, 3, 4});
    top.eval();
    CHECK(top.dout.get() == std::array<uint32_t, 4>{1, 2, 3, 4});
    top.din.set({9, 8, 7, 6});
    top.eval();
    CHECK(top.dout.get() == std::array<uint32_t, 4>{9, 8, 7, 6});
}

TEST_CASE("createChildModuleArray: 自动起名、注册与层次路径") {
    Pool top;
    REQUIRE(top.children().size() == 4);
    for (uint32_t i = 0; i < 4; ++i)
        CHECK(top.children()[i]->name() == "cm[" + std::to_string(i) + "]");
    top.elaborate("top");
    CHECK(top.children()[1]->hierPath() == "top.cm[1]");
    // 子模块实体经 children() 对 flatten 可见（实体创建序：din, dout）
    REQUIRE(top.children()[1]->entities().size() == 2);
    CHECK(top.children()[1]->entities()[0]->hierPath() == "top.cm[1].din");
    CHECK(top.children()[1]->entities()[1]->hierPath() == "top.cm[1].dout");
}

TEST_CASE("createChildModuleArray: 重名数组撞元素名即抛") {
    CHECK_THROWS_AS(DupTop{}, Error);
}
