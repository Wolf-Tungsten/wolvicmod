// core/module.h：createChildModuleArray / ChildModuleArray / MOD_ARRAY——批量
// 创建 N 个默认构造子模块（自动起名 "name[i]" 并注册）；ChildModuleArray 包装
// 指针数组、operator[] 返回引用，调用点与单个子模块的引用成员一致。句柄无需
// 逃逸出构造函数（§4.1）：默认写构造函数局部，确有构造后访问需求才以
// MOD_ARRAY 绑为成员。

#include <array>
#include <cstdint>
#include <string>

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>

using namespace wolvicmod;

namespace {

struct Cell : Module {
    IN(uint32_t, din);
    OUT(uint32_t, dout);

    Cell() { dout = din; }
};

using U32x4 = std::array<uint32_t, 4>;  // 宏只收定长参数，含逗号类型先取别名
using U32x2 = std::array<uint32_t, 2>;

// 子模块数组为构造函数局部：根输入数组散布 → Cell 直通 → combine 回 Out。
struct Pool : Module {
    IN(U32x4, din);
    OUT(U32x4, dout);

    Pool() {
        auto cells = createChildModuleArray<Cell, 4>("cm");
        for (uint32_t i = 0; i < 4; ++i)
            cells[i].din.assign().reads(din) = [i](auto src) {
                auto [a] = src;
                return a[i];
            };
        std::array<Out<uint32_t>*, 4> outs{};
        for (uint32_t i = 0; i < 4; ++i) outs[i] = &cells[i].dout;
        combine(dout, outs);
    }
};

// MOD_ARRAY 成员：声明式一行创建，子模块名随成员名（cells[0]、cells[1]）。
struct MemberPool : Module {
    IN(U32x2, din);
    OUT(U32x2, dout);
    MOD_ARRAY(Cell, 2, cells);

    MemberPool() {
        for (uint32_t i = 0; i < 2; ++i)
            cells[i].din.assign().reads(din) = [i](auto src) {
                auto [a] = src;
                return a[i];
            };
        std::array<Out<uint32_t>*, 2> outs{&cells[0].dout, &cells[1].dout};
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

TEST_CASE("MOD_ARRAY: 成员一行声明，operator[] 引用即注册本体") {
    MemberPool top;
    CHECK(top.cells.size() == 2);
    CHECK(&top.cells[0] == top.children()[0].get());
    CHECK(&top.cells[1] == top.children()[1].get());
    top.elaborate("top");
    CHECK(top.children()[1]->hierPath() == "top.cells[1]");
    top.din.set({5, 6});
    top.eval();
    CHECK(top.dout.get() == std::array<uint32_t, 2>{5, 6});
}

TEST_CASE("createChildModuleArray: 重名数组撞元素名即抛") {
    CHECK_THROWS_AS(DupTop{}, Error);
}
