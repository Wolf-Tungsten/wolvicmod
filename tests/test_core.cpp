// M2: entities, naming, and the registration kernel (doc §2, §3.1–3.4, §6.1).
// Compile-time negative checks (non-invocable lambdas, wrong builder word
// order, writes through a read set) are compile failures by design and are
// not exercisable here.

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>

using namespace wolvicmod;

namespace {

// §3.1 full-form adder.
struct Adder : Module {
    IN(uint32_t, a);
    IN(uint32_t, b);
    OUT(uint32_t, sum);

    Adder() {
        sum.assign().reads(a, b) = [](auto src) {
            auto [x, y] = src;
            return x + y;
        };
    }
};

// §3.2 two-Update counter (full form).
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

// Hierarchy fixture: child with ports, parent wiring both directions.
struct Child : Module {
    IN(uint32_t, din);
    IN(bool, clk);
    OUT(uint32_t, dout);
    WIRE(uint32_t, internal);
    REG(uint32_t, q);

    Child() {
        internal.assign().reads(din) = [](auto src) {
            auto [d] = src;
            return d;
        };
        q.update().on(posedge(clk)).reads(internal) = [](auto src) {
            auto [w] = src;
            return w;
        };
        dout.assign().reads(q) = [](auto src) {
            auto [v] = src;
            return v;
        };
    }
};

struct Parent : Module {
    IN(uint32_t, din);
    IN(bool, clk);
    OUT(uint32_t, dout);
    SUB(Child, sr);

    Parent() {
        sr.din = din;   // down: parent Assign drives child In
        sr.clk = clk;   // clock distribution, same form
        dout = sr.dout; // up: parent reads child Out
    }
};

TEST_CASE("M2: full-form assign registration collects actions") {
    Adder top;
    CHECK(top.actions().size() == 1);
    CHECK(top.actions()[0]->kind() == Action::Kind::Assign);
    CHECK(top.actions()[0]->target() == static_cast<Entity*>(&top.sum));
    CHECK(top.actions()[0]->reads().size() == 2);
}

TEST_CASE("M2: update chain with on/en/reads registers") {
    Counter top;
    CHECK(top.actions().size() == 3);  // 2 updates + 1 assign
    CHECK(top.actions()[0]->kind() == Action::Kind::Update);
    CHECK(top.actions()[1]->kind() == Action::Kind::Update);
    CHECK(top.actions()[2]->kind() == Action::Kind::Assign);
}

TEST_CASE("M2: naming and hierarchical paths (§6.1)") {
    Parent top;
    CHECK(top.sr.name() == "sr");
    CHECK(top.sr.parent() == &top);
    CHECK(top.sr.din.hierPath() == "sr.din");
    CHECK(top.sr.q.hierPath() == "sr.q");
    CHECK(top.din.hierPath() == "din");  // root unnamed before elaborate()
}

TEST_CASE("M2: duplicate member names are rejected at creation") {
    struct Bad : Module {
        IN(uint32_t, a);
        Bad() { createWire<uint32_t>("a"); }
    };
    CHECK_THROWS_AS(Bad{}, Error);
}

TEST_CASE("M2: hierarchy wiring registers in the parent's context") {
    Parent top;
    // sr.din = din, sr.clk = clk, dout = sr.dout
    CHECK(top.actions().size() == 3);
    // child's own internal actions live on the child
    CHECK(top.sr.actions().size() == 3);
    // the assign driving sr.din targets the child's In
    CHECK(top.actions()[0]->target() == static_cast<Entity*>(&top.sr.din));
}

// ---- wiring-rule violations (§2.2), caught at registration ----

TEST_CASE("M2: parent cannot read a child's internal wire") {
    struct Bad : Module {
        OUT(uint32_t, o);
        SUB(Child, c);
        Bad() {
            o.assign().reads(c.internal) = [](auto src) {
                auto [w] = src;
                return w;
            };
        }
    };
    CHECK_THROWS_AS(Bad{}, Error);
}

TEST_CASE("M2: a module cannot drive its own In") {
    struct Bad : Module {
        IN(uint32_t, i);
        Bad() { i = 0; }
    };
    CHECK_THROWS_AS(Bad{}, Error);
}

TEST_CASE("M2: a module cannot drive a grandchild's In") {
    struct Mid : Module {
        SUB(Child, c);
        Mid() = default;
    };
    struct Bad : Module {
        OUT(uint32_t, o);
        SUB(Mid, m);
        Bad() { m.c.din = 1u; }  // grandchild port: illegal
    };
    CHECK_THROWS_AS(Bad{}, Error);
}

TEST_CASE("M2: parent cannot update a child's Reg") {
    struct Bad : Module {
        IN(bool, clk);
        SUB(Child, c);
        Bad() {
            c.q.update().on(posedge(clk)).reads(c.dout) = [](auto src) {
                auto [d] = src;
                return d;
            };
        }
    };
    CHECK_THROWS_AS(Bad{}, Error);
}

TEST_CASE("M2: unnamed entities are rejected at registration (§6.2)") {
    struct Bad : Module {
        IN(uint32_t, a);
        OUT(uint32_t, o);
        Wire<uint32_t> raw;  // bare member, not created via createWire
        Bad() {
            o.assign().reads(raw) = [](auto src) {
                auto [w] = src;
                return w;
            };
        }
    };
    CHECK_THROWS_AS(Bad{}, Error);
}

TEST_CASE("M2: edge events accept In/Wire/Reg of bool (§3.2)") {
    struct M : Module {
        IN(bool, clk);
        WIRE(bool, gated);
        REG(bool, div2);
        REG(uint32_t, cnt);
        M() {
            gated = clk;
            div2.update().on(posedge(clk)).reads(div2) = [](auto src) {
                auto [d] = src;
                return !d;
            };
            cnt.update().on(posedge(gated), negedge(div2)).reads(cnt) = [](auto src) {
                auto [c] = src;
                return c + 1;
            };
        }
    };
    CHECK_NOTHROW(M{});
}

TEST_CASE("M2: multi-edge update with level read in lambda (§3.2)") {
    struct M : Module {
        IN(bool, clk);
        IN(bool, rst_n);
        IN(bool, en);
        REG(uint32_t, cnt);
        M() {
            cnt.update().on(posedge(clk), negedge(rst_n)).reads(cnt, rst_n, en) = [](auto src) {
                auto [c, rn, e] = src;
                if (rn == 0) return 0u;
                return e ? c + 1 : c;
            };
        }
    };
    CHECK_NOTHROW(M{});
}

TEST_CASE("M2: fast-path constant and identity assign (§3.1)") {
    struct M : Module {
        IN(uint32_t, a);
        OUT(uint32_t, b);
        WIRE(uint32_t, w);
        M() {
            w = a;    // identity
            b = 42;   // constant, zero read set
        }
    };
    M top;
    CHECK(top.actions().size() == 2);
    CHECK(top.actions()[0]->reads().size() == 1);
    CHECK(top.actions()[1]->reads().empty());
}

TEST_CASE("M2: mem update builder with addr/en/reads (§3.4)") {
    struct M : Module {
        IN(bool, clk);
        IN(bool, wen);
        IN(uint32_t, waddr);
        IN(uint32_t, wdata);
        IN(uint32_t, raddr);
        OUT(uint32_t, rdata);
        MEM(uint32_t, 1024, mem);
        M() {
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
    M top;
    CHECK(top.actions().size() == 2);
    // update reads: read set + event signal + addr + guard
    CHECK(top.actions()[0]->reads().size() == 4);
}

}  // namespace
