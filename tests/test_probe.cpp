// dbg/probe.h：波形探针——复合类型字段引出为可观测命名 Wire。覆盖命名（相对
// 路径组合）、值跟踪（Wire/Reg 源）、可格式化判定、子模块端口探针、重名
// 报错、构造外调用拒绝。

#include <cstdint>
#include <string>

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>

using namespace wolvicmod;

namespace {

// 非可格式化复合类型：探针前在波形中不可见。
struct Flit {
    bool valid = false;
    uint32_t id = 0;
};

struct ProbeTop : Module {
    IN(uint32_t, din);
    WIRE(Flit, flit_w);
    Wire<bool>* p_valid = nullptr;    // testbench 白盒
    Wire<uint32_t>* p_id = nullptr;

    ProbeTop() {
        flit_w.assign().reads(din) = [](auto src) {
            auto [d] = src;
            return Flit{(d & 1) != 0, d >> 1};
        };
        p_valid = &probe(flit_w, "valid", [](const Flit& f) { return f.valid; });
        p_id = &probe(flit_w, "id", [](const Flit& f) { return f.id; });
    }
};

struct RegProbeTop : Module {
    IN(bool, clk);
    IN(uint32_t, din);
    REG(Flit, flit_r);
    Wire<uint32_t>* p_id = nullptr;

    RegProbeTop() {
        flit_r.update().on(posedge(clk)).reads(din) = [](auto src) {
            auto [d] = src;
            return Flit{(d & 1) != 0, d >> 1};
        };
        p_id = &probe(flit_r, "id", [](const Flit& f) { return f.id; });
    }
};

struct Child : Module {
    OUT(Flit, dout);
    Child() { dout = Flit{true, 7}; }
};

struct ChildProbeTop : Module {
    MOD(Child, sub);
    Wire<bool>* p_valid = nullptr;

    ChildProbeTop() {
        p_valid = &probe(sub.dout, "valid", [](const Flit& f) { return f.valid; });
    }
};

struct DupProbeTop : Module {
    WIRE(Flit, flit_w);
    DupProbeTop() {
        flit_w = Flit{};
        probe(flit_w, "valid", [](const Flit& f) { return f.valid; });
        probe(flit_w, "valid", [](const Flit& f) { return !f.valid; });  // 重名
    }
};

}  // namespace

TEST_CASE("probe: 复合 Wire 字段引出——命名、值跟踪、可格式化") {
    ProbeTop top;
    top.elaborate();
    REQUIRE(top.p_valid != nullptr);
    REQUIRE(top.p_id != nullptr);

    // 源不可见，探针可见——探针的全部意义
    CHECK(top.flit_w.waveSupported() == false);
    CHECK(top.p_valid->waveSupported() == true);
    CHECK(top.p_id->waveSupported() == true);

    // 探针是普通命名 Wire：层次路径与源视觉相邻
    CHECK(top.p_valid->hierPath() == "top.flit_w.valid");
    CHECK(top.p_id->hierPath() == "top.flit_w.id");

    top.din.set(0b1011);  // valid=1, id=0b101
    top.eval();
    CHECK(top.p_valid->get() == true);
    CHECK(top.p_id->get() == 0b101);

    top.din.set(0b0100);  // valid=0, id=0b010
    top.eval();
    CHECK(top.p_valid->get() == false);
    CHECK(top.p_id->get() == 0b010);
}

TEST_CASE("probe: Reg 源逐拍跟踪") {
    RegProbeTop top;
    top.elaborate();
    top.clk.set(0);
    top.din.set(0b1001);
    top.eval();
    CHECK(top.p_id->get() == 0);  // 未提交
    top.clk.set(1);
    top.eval();  // posedge：flit_r <- {1, 0b100}
    CHECK(top.p_id->get() == 0b100);
}

TEST_CASE("probe: 子模块端口探针，名含相对路径") {
    ChildProbeTop top;
    top.elaborate();
    REQUIRE(top.p_valid != nullptr);
    CHECK(top.p_valid->hierPath() == "top.sub.dout.valid");
    top.eval();
    CHECK(top.p_valid->get() == true);
}

TEST_CASE("probe: 探针重名被命名表拒绝") {
    CHECK_THROWS_AS(DupProbeTop{}, Error);
}

TEST_CASE("probe: 模块构造外调用被拒绝") {
    ProbeTop top;
    top.elaborate();
    CHECK_THROWS_AS(probe(top.flit_w, "late", [](const Flit& f) { return f.valid; }), Error);
}
