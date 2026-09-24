// M4: simulation engine — eval/round two phases, edge detection, NBA commit,
// convergence limits (doc §3.5, §5).

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>

using namespace wolvicmod;

namespace {

// §3.2 counter, two-Update form (full-form lambdas; identity/constant fast
// paths are M2 features already available).
struct Counter : Module {
    IN(bool, clk);
    IN(bool, rst_n);
    IN(bool, en);
    OUT(uint32_t, dout);
    REG(uint32_t, cnt);

    Counter() {
        cnt.update().on(negedge(rst_n)) = 0;
        cnt.update().on(posedge(clk)).en(en).reads(cnt) = [](auto src) {
            auto [c] = src;
            return c + 1;
        };
        dout = cnt;
    }
};

void resetPulse(Counter& top) {
    top.rst_n.set(1);
    top.eval();
    top.rst_n.set(0);
    top.eval();  // reset takes effect, cnt <- 0
    top.rst_n.set(1);
    top.eval();
}

void clkCycle(Counter& top) {
    top.clk.set(0);
    top.eval();
    top.clk.set(1);
    top.eval();  // posedge
}

TEST_CASE("M4: counter follows the §3.5 flow, dout prints 1..10") {
    Counter top;
    top.elaborate();
    resetPulse(top);
    CHECK(top.dout.get() == 0);
    top.en.set(1);
    for (uint32_t cycle = 1; cycle <= 10; ++cycle) {
        clkCycle(top);
        CHECK(top.dout.get() == cycle);
    }
}

TEST_CASE("M4: eval() is idempotent (§5.1)") {
    Counter top;
    top.elaborate();
    resetPulse(top);
    top.en.set(1);
    clkCycle(top);
    CHECK(top.dout.get() == 1);
    top.eval();
    top.eval();
    CHECK(top.dout.get() == 1);
}

TEST_CASE("M4: guard leaves the register untouched (§3.2)") {
    Counter top;
    top.elaborate();
    resetPulse(top);
    top.en.set(0);
    for (int i = 0; i < 3; ++i) clkCycle(top);
    CHECK(top.dout.get() == 0);
}

TEST_CASE("M4: reset has priority over counting (§3.2, §4.3)") {
    Counter top;
    top.elaborate();
    resetPulse(top);
    top.en.set(1);
    clkCycle(top);
    clkCycle(top);
    CHECK(top.dout.get() == 2);
    // rst_n falls and clk rises in the same eval: both Updates activate,
    // the earlier-registered (reset) wins.
    top.clk.set(0);
    top.eval();
    top.rst_n.set(0);
    top.clk.set(1);
    top.eval();
    CHECK(top.dout.get() == 0);
}

TEST_CASE("M4: cascaded clocks advance within one eval() (§5.3)") {
    struct Div : Module {
        IN(bool, clk);
        OUT(uint32_t, dout);
        REG(bool, div2);
        REG(uint32_t, cnt);
        Div() {
            div2.update().on(posedge(clk)).reads(div2) = [](auto src) {
                auto [d] = src;
                return !d;
            };
            cnt.update().on(posedge(div2)).reads(cnt) = [](auto src) {
                auto [c] = src;
                return c + 1;
            };
            dout = cnt;
        }
    };
    Div top;
    top.elaborate();
    // div2 rises on clk posedges 1, 3, 5...; cnt follows in the same eval.
    for (int i = 0; i < 4; ++i) {
        top.clk.set(0);
        top.eval();
        top.clk.set(1);
        top.eval();
    }
    CHECK(top.dout.get() == 2);
}

TEST_CASE("M4: two Updates on the same event: first registration wins") {
    struct Prio : Module {
        IN(bool, clk);
        OUT(uint32_t, dout);
        REG(uint32_t, cnt);
        Prio() {
            cnt.update().on(posedge(clk)) = 7;
            cnt.update().on(posedge(clk)).reads(cnt) = [](auto src) {
                auto [c] = src;
                return c + 1;
            };
            dout = cnt;
        }
    };
    Prio top;
    top.elaborate();
    for (int i = 0; i < 3; ++i) {
        top.clk.set(0);
        top.eval();
        top.clk.set(1);
        top.eval();
        CHECK(top.dout.get() == 7);
    }
}

TEST_CASE("M4: zero-delay oscillation hits the round cap (§5.3)") {
    struct Osc : Module {
        WIRE(bool, w);
        REG(bool, x);
        OUT(bool, o);
        Osc() {
            w.assign().reads(x) = [](auto src) {
                auto [v] = src;
                return !v;
            };
            x.update().on(posedge(w), negedge(w)).reads(x) = [](auto src) {
                auto [v] = src;
                return !v;
            };
            o = w;
        }
    };
    Osc top;
    top.elaborate();
    try {
        top.eval();
        FAIL("expected the round cap to throw");
    } catch (const Error& e) {
        CHECK(std::string(e.what()).find("oscillation") != std::string::npos);
    }
}

TEST_CASE("M4: combinational cycle settles by iteration (§4.3)") {
    struct Cyc : Module {
        IN(uint32_t, din);
        OUT(uint32_t, o);
        WIRE(uint32_t, x);
        WIRE(uint32_t, y);
        Cyc() {
            x.assign().reads(din, y) = [](auto src) {
                auto [d, yy] = src;
                return d + (yy & 0u);
            };
            y.assign().reads(x) = [](auto src) {
                auto [xx] = src;
                return xx;
            };
            o = y;
        }
    };
    Cyc top;
    top.elaborate();
    top.din.set(5);
    top.eval();
    CHECK(top.o.get() == 5);
}

TEST_CASE("M4: non-converging combinational cycle hits the iteration cap") {
    struct Bad : Module {
        IN(uint32_t, a);
        WIRE(uint32_t, x);
        WIRE(uint32_t, y);
        Bad() {
            x.assign().reads(y) = [](auto src) {
                auto [v] = src;
                return v + 1;
            };
            y.assign().reads(x) = [](auto src) {
                auto [v] = src;
                return v + 1;
            };
        }
    };
    Bad top;
    top.elaborate();
    top.a.set(0);
    CHECK_THROWS_AS(top.eval(), Error);
}

TEST_CASE("M4: memory write/read with row-level commit (§3.4)") {
    struct DataMem : Module {
        IN(bool, clk);
        IN(bool, wen);
        IN(uint32_t, waddr);
        IN(uint32_t, wdata);
        IN(uint32_t, raddr);
        OUT(uint32_t, rdata);
        MEM(uint32_t, 1024, mem);
        DataMem() {
            mem.update().on(posedge(clk)).addr(waddr).en(wen).reads(wdata) = [](auto src) {
                auto [d] = src;
                return d;
            };
            rdata.assign().reads(mem, raddr) = [](auto src) {
                auto [m, a] = src;
                return m[a];
            };
        }
    };
    DataMem top;
    top.elaborate();
    // write 42 @ row 10
    top.wen.set(1);
    top.waddr.set(10);
    top.wdata.set(42);
    top.clk.set(0);
    top.eval();
    top.clk.set(1);
    top.eval();
    top.wen.set(0);
    top.eval();
    top.raddr.set(10);
    top.eval();
    CHECK(top.rdata.get() == 42);
    top.raddr.set(5);
    top.eval();
    CHECK(top.rdata.get() == 0);  // untouched row holds its initial value
    // out-of-range write address is a runtime error
    top.wen.set(1);
    top.waddr.set(1024);
    top.clk.set(0);
    top.eval();
    top.clk.set(1);
    CHECK_THROWS_AS(top.eval(), Error);
}

TEST_CASE("M4: set() guards (§3.5)") {
    Counter top;
    CHECK_THROWS_AS(top.clk.set(1), Error);  // before elaborate()
    top.elaborate();
    CHECK_NOTHROW(top.clk.set(1));
}

TEST_CASE("M4: eval() before elaborate() is an error") {
    Counter top;
    CHECK_THROWS_AS(top.eval(), Error);
}

}  // namespace
