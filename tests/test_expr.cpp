// M5: expression fast path (doc §3.1) — operators register equivalent
// Assign/Update actions with auto-collected read sets.

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>

using namespace wolvicmod;

namespace {

TEST_CASE("M5: adder fast path matches the full form (§3.1)") {
    struct Fast : Module {
        IN(uint32_t, a);
        IN(uint32_t, b);
        OUT(uint32_t, sum);
        Fast() { sum = a + b; }
    };
    struct Full : Module {
        IN(uint32_t, a);
        IN(uint32_t, b);
        OUT(uint32_t, sum);
        Full() {
            sum.assign().reads(a, b) = [](auto src) {
                auto [x, y] = src;
                return x + y;
            };
        }
    };
    Fast fast;
    Full full;
    fast.elaborate();
    full.elaborate();
    for (uint32_t x : {0u, 1u, 41u, 1u << 31}) {
        fast.a.set(x);
        fast.b.set(x + 1);
        full.a.set(x);
        full.b.set(x + 1);
        fast.eval();
        full.eval();
        CHECK(fast.sum.get() == full.sum.get());
        CHECK(fast.sum.get() == 2 * x + 1);
    }
}

TEST_CASE("M5: read set is auto-collected and deduplicated") {
    struct M : Module {
        IN(uint32_t, a);
        IN(uint32_t, b);
        OUT(uint32_t, y);
        OUT(uint32_t, z);
        M() {
            y = a + b;
            z = a * a;
        }
    };
    M top;
    CHECK(top.actions()[0]->reads().size() == 2);
    CHECK(top.actions()[1]->reads().size() == 1);  // a appears twice, deduped
}

TEST_CASE("M5: constants, mixing, unary, shifts, comparisons") {
    struct M : Module {
        IN(uint32_t, a);
        IN(uint32_t, b);
        IN(bool, en);
        OUT(uint32_t, y);
        OUT(uint32_t, z);
        OUT(bool, f);
        OUT(bool, gt);
        M() {
            y = 2 * (a & 0xffu) + 1;
            z = a << 2;
            f = !en;
            gt = a > b;
        }
    };
    M top;
    top.elaborate();
    top.a.set(0x123u);
    top.b.set(7u);
    top.en.set(0);
    top.eval();
    CHECK(top.y.get() == 2 * 0x23u + 1);
    CHECK(top.z.get() == 0x123u << 2);
    CHECK(top.f.get() == true);
    CHECK(top.gt.get() == true);
    top.b.set(0x1000u);
    top.eval();
    CHECK(top.gt.get() == false);
}

TEST_CASE("M5: constant assign has an empty read set") {
    struct M : Module {
        OUT(uint32_t, req);
        M() { req = 0; }
    };
    M top;
    top.elaborate();
    CHECK(top.actions()[0]->reads().empty());
    top.eval();
    CHECK(top.req.get() == 0u);
}

TEST_CASE("M5: update fast path counts (§3.2)") {
    struct M : Module {
        IN(bool, clk);
        OUT(uint32_t, dout);
        REG(uint32_t, cnt);
        M() {
            cnt.update().on(posedge(clk)) = cnt + 1;
            dout = cnt;
        }
    };
    M top;
    top.elaborate();
    for (uint32_t i = 1; i <= 5; ++i) {
        top.clk.set(0);
        top.eval();
        top.clk.set(1);
        top.eval();
        CHECK(top.dout.get() == i);
    }
}

TEST_CASE("M5: expression through hierarchy wiring") {
    struct Child : Module {
        IN(uint32_t, x);
        OUT(uint32_t, y);
        Child() { y = x * 3; }
    };
    struct Top : Module {
        IN(uint32_t, a);
        OUT(uint32_t, o);
        MOD(Child, c);
        Top() {
            c.x = a + 1;
            o = c.y + a;
        }
    };
    Top top;
    top.elaborate();
    top.a.set(4);
    top.eval();
    CHECK(top.o.get() == 15 + 4);
}

TEST_CASE("M5: expression result converts to the target type") {
    struct M : Module {
        IN(uint32_t, a);
        IN(uint32_t, b);
        OUT(bool, eq);
        M() { eq = a == b; }
    };
    M top;
    top.elaborate();
    top.a.set(3);
    top.b.set(3);
    top.eval();
    CHECK(top.eq.get() == true);
}

TEST_CASE("M5: subscript expression on array-valued signals") {
    struct M : Module {
        using U32x4 = std::array<uint32_t, 4>;
        IN(U32x4, arr);
        OUT(uint32_t, y);
        OUT(uint32_t, z);
        M() {
            y = arr[1];
            z = arr[0] + arr[2];  // 下标节点可继续组合
        }
    };
    M top;
    CHECK(top.actions()[0]->reads().size() == 1);  // 读集 = 整个 arr 端口
    CHECK(top.actions()[1]->reads().size() == 1);  // arr[0]+arr[2] 去重后仍 1
    top.elaborate();
    top.arr.set({10, 20, 30, 40});
    top.eval();
    CHECK(top.y.get() == 20);
    CHECK(top.z.get() == 40);
    // 活性：改元素即传播（非快照）
    top.arr.set({1, 2, 3, 4});
    top.eval();
    CHECK(top.y.get() == 2);
    CHECK(top.z.get() == 4);
}

TEST_CASE("M5: subscript expression in update and bool arrays") {
    struct M : Module {
        using Boolx2 = std::array<bool, 2>;
        using U8x3 = std::array<uint8_t, 3>;
        IN(bool, clk);
        IN(Boolx2, rdy);
        IN(U8x3, nib);
        OUT(bool, o);
        REG(uint8_t, cnt);
        M() {
            o = rdy[1];
            cnt.update().on(posedge(clk)) = cnt + nib[2];
        }
    };
    M top;
    top.elaborate();
    top.rdy.set({false, true});
    top.nib.set({1, 2, 3});
    top.clk.set(0);
    top.eval();
    CHECK(top.o.get() == true);
    CHECK(top.cnt.get() == 0);
    top.clk.set(1);
    top.eval();
    top.clk.set(0);
    CHECK(top.cnt.get() == 3);
}

}  // namespace
