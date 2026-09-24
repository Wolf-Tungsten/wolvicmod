// M3: elaboration — flattening, structural checks, simulation graphs
// (doc §4.2–4.3, §6.1–6.2).

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>

using namespace wolvicmod;

namespace {

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
        dout.assign().reads(cnt) = [](auto src) {
            auto [c] = src;
            return c;
        };
    }
};

TEST_CASE("M3: elaboration builds exec order and priority chains") {
    Counter top;
    top.elaborate();
    CHECK(top.elaborated());
    const auto* sim = top.sim();
    REQUIRE(sim != nullptr);
    // 2 updates + 1 assign, all as singleton exec items
    CHECK(sim->execOrder.size() == 3);
    CHECK(sim->groups.empty());
    // one chain for cnt, reset (registered first) has highest priority
    REQUIRE(sim->chains.size() == 1);
    REQUIRE(sim->chains[0].updates.size() == 2);
    CHECK(sim->chains[0].updates[0]->regIndex() < sim->chains[0].updates[1]->regIndex());
    CHECK(sim->chains[0].updates[0]->target() == static_cast<Entity*>(&top.cnt));
}

TEST_CASE("M3: root name prefixes hierarchical paths after elaboration") {
    Counter top;
    top.elaborate("top");
    CHECK(top.cnt.hierPath() == "top.cnt");
    CHECK(top.clk.hierPath() == "top.clk");
}

TEST_CASE("M3: elaborate() twice is an error") {
    Counter top;
    top.elaborate();
    CHECK_THROWS_AS(top.elaborate(), Error);
}

TEST_CASE("M3: multiple drivers are reported with paths (§4.2)") {
    struct Bad : Module {
        IN(uint32_t, a);
        WIRE(uint32_t, w);
        Bad() {
            w = a;
            w = 0;
        }
    };
    Bad top;
    try {
        top.elaborate();
        FAIL("expected elaboration to throw");
    } catch (const Error& e) {
        const std::string msg = e.what();
        CHECK(msg.find("multiple drivers") != std::string::npos);
        CHECK(msg.find("top.w") != std::string::npos);
    }
}

TEST_CASE("M3: dangling signal is reported (§4.2)") {
    struct Bad : Module {
        IN(uint32_t, a);
        OUT(uint32_t, o);
        WIRE(uint32_t, floating);
        Bad() { o = a; }
    };
    Bad top;
    try {
        top.elaborate();
        FAIL("expected elaboration to throw");
    } catch (const Error& e) {
        const std::string msg = e.what();
        CHECK(msg.find("no driver") != std::string::npos);
        CHECK(msg.find("top.floating") != std::string::npos);
    }
}

TEST_CASE("M3: driving a root input is an error (§4.2)") {
    struct Bad : Module {
        IN(uint32_t, a);
        Bad() { a = 1u; }
    };
    CHECK_THROWS_AS(Bad{}, Error);  // caught earlier, at registration (§2.2)
}

TEST_CASE("M3: legal combinational cycle becomes one SCC group (§4.3)") {
    struct Cycle : Module {
        IN(uint32_t, din);
        OUT(uint32_t, o);
        WIRE(uint32_t, x);
        WIRE(uint32_t, y);
        Cycle() {
            // x depends on y, y depends on x: a zero-delay loop settled by
            // iteration (converges since both lambdas ignore the loop value
            // after one pass... structurally it is still a cycle).
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
    Cycle top;
    top.elaborate();
    const auto* sim = top.sim();
    REQUIRE(sim->groups.size() == 1);
    CHECK(sim->groups[0]->actions.size() == 2);
    CHECK(sim->groups[0]->signals.size() == 2);
    // exec: SCC group + the output assign
    CHECK(sim->execOrder.size() == 2);
}

TEST_CASE("M3: self-loop is a non-trivial SCC") {
    struct Self : Module {
        IN(uint32_t, a);
        WIRE(uint32_t, w);
        Self() {
            w.assign().reads(a, w) = [](auto src) {
                auto [v, ww] = src;
                return v + (ww & 0u);
            };
        }
    };
    Self top;
    top.elaborate();
    CHECK(top.sim()->groups.size() == 1);
}

TEST_CASE("M3: cycle through a non-comparable type fails at elaboration") {
    struct NoEq {
        int x = 0;
    };
    struct Bad : Module {
        IN(uint32_t, a);
        WIRE(NoEq, p);
        WIRE(NoEq, q);
        Bad() {
            p.assign().reads(q) = [](auto src) {
                auto [v] = src;
                return NoEq{v.x};
            };
            q.assign().reads(p) = [](auto src) {
                auto [v] = src;
                return NoEq{v.x};
            };
        }
    };
    Bad top;
    try {
        top.elaborate();
        FAIL("expected elaboration to throw");
    } catch (const Error& e) {
        CHECK(std::string(e.what()).find("equality-comparable") != std::string::npos);
    }
}

TEST_CASE("M3: hierarchy flattens transparently (§4.2)") {
    struct Child : Module {
        IN(uint32_t, din);
        OUT(uint32_t, dout);
        REG(uint32_t, q);
        IN(bool, clk);
        Child() {
            q.update().on(posedge(clk)).reads(din) = [](auto src) {
                auto [d] = src;
                return d;
            };
            dout.assign().reads(q) = [](auto src) {
                auto [v] = src;
                return v;
            };
        }
    };
    struct Top : Module {
        IN(uint32_t, din);
        IN(bool, clk);
        OUT(uint32_t, dout);
        SUB(Child, c0);
        SUB(Child, c1);
        Top() {
            c0.din = din;
            c0.clk = clk;
            c1.din = c0.dout;  // sibling wiring: parent reads c0.Out, drives c1.In
            c1.clk = clk;
            dout = c1.dout;
        }
    };
    Top top;
    CHECK_NOTHROW(top.elaborate());
    CHECK(top.sim()->chains.size() == 2);  // c0.q and c1.q
    CHECK(top.c1.din.hierPath() == "top.c1.din");
}

}  // namespace
