// Identity-connect aliasing (§5.2): same-type identity assigns (b = a fast
// path) are eliminated at elaboration — the target's storage aliases the
// canonical source's and consumers rewire to it. These tests pin the graph
// reduction and behavioral invariants across hierarchy connects, alias
// chains, Reg sources, edge watches on an aliased clock, an SCC through an
// alias, the degenerate alias cycle fallback, and conversion assigns that
// must NOT be aliased.

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>

#include <random>

using namespace wolvicmod;

namespace {

// Hierarchical model: clk tree (In=In identity connects), sibling wiring,
// Out<-Out chains to the root, a Reg as alias source, edge watches on an
// aliased clock, and a conversion copy that must stay a real action.
struct AlChild : Module {
    IN(bool, clk);
    IN(uint8_t, din);
    IN(bool, en);
    OUT(uint8_t, dout);
    OUT(uint8_t, d2);
    REG(uint8_t, r);
    REG(uint8_t, cnt);

    AlChild() {
        r.update().on(posedge(clk)).en(en).reads(din) = [](auto s) {
            auto [v] = s;
            return static_cast<uint8_t>(v + 1);
        };
        cnt.update().on(posedge(clk)).reads(cnt) = [](auto s) {
            auto [c] = s;
            return static_cast<uint8_t>(c + 1);
        };
        dout = r;    // identity Out <- Reg (Reg as alias source)
        d2 = cnt;    // identity, chained further up at the parent
    }
};

struct AlTop : Module {
    IN(bool, clk);
    IN(uint8_t, din);
    IN(bool, en);
    OUT(uint8_t, out1);
    OUT(uint8_t, out2);
    OUT(uint32_t, out3);
    MOD(AlChild, c0);
    MOD(AlChild, c1);
    WIRE(uint8_t, w);
    WIRE(uint32_t, conv);

    AlTop() {
        c0.clk = clk;
        c1.clk = clk;             // clk tree
        c0.din = din;
        c1.din = c0.dout;         // sibling wiring: parent reads c0.Out
        c0.en = en;
        c1.en = en;
        w = c1.d2;                // Wire <- child Out
        out1 = w;                 // Out <- Wire: chain out1 -> w -> c1.d2 -> c1.cnt
        out2 = c0.dout;           // Out <- child Out
        conv = c1.dout;           // conversion copy (uint32_t <- uint8_t): NOT aliased
        out3 = conv;              // identity past the conversion: aliased to conv
    }
};

// SCC through an alias: a = b is eliminated, so b's action reads its own
// target through the alias — a single-node self-loop SCC.
struct AlCyc : Module {
    IN(bool, en);
    WIRE(bool, a);
    WIRE(bool, b);
    OUT(bool, o);

    AlCyc() {
        a = b;
        b.assign().reads(a, en) = [](auto s) {
            auto [x, e] = s;
            return x || e;
        };
        o = a;
    }
};

// Degenerate alias cycle (a = b; b = a): the closing pair keeps its copy
// action; through the aliased storage it degenerates to a self-copy.
struct AlLoop : Module {
    WIRE(bool, a);
    WIRE(bool, b);
    OUT(bool, o);

    AlLoop() {
        a = b;
        b = a;
        o = b;
    }
};

TEST_CASE("alias: eliminated identity actions leave the graph") {
    AlTop top;
    top.elaborate();
    // AlTop has 14 identity connects (6 port ties + 4 child Out assigns +
    // w/out1/out2/out3); only the 4 Updates and the conversion assign remain.
    CHECK(top.sim()->execOrder.size() == 5);
}

TEST_CASE("alias: hierarchical connects, chains, Reg sources") {
    AlTop top;
    top.elaborate();
    AlCyc cyc;
    cyc.elaborate();

    std::mt19937 rng(31);
    uint32_t posedges = 0;   // cnt has no en guard: +1 per posedge
    bool prevO = false;      // AlCyc latches: o_next = o || en
    bool cycEn = false;
    auto evalBoth = [&]() {
        top.eval();
        cyc.eval();
        CHECK(top.out1.get() == top.c1.cnt.get());   // identity chain out1->w->c1.d2->c1.cnt
        CHECK(top.out2.get() == top.c0.r.get());     // identity chain out2->c0.dout->c0.r
        // Conversion copy is a real action: conv = uint32_t(c1.dout), out3 = conv.
        CHECK(top.out3.get() == static_cast<uint32_t>(top.c1.r.get()));
        CHECK(top.c0.cnt.get() == static_cast<uint8_t>(posedges));
        CHECK(top.c1.cnt.get() == static_cast<uint8_t>(posedges));
        CHECK(cyc.o.get() == (cycEn || prevO));
        prevO = cyc.o.get();
    };
    auto setAll = [&](bool clk, bool en, uint8_t v) {
        top.clk.set(clk);
        top.en.set(en);
        top.din.set(v);
        cycEn = en;
        cyc.en.set(en);
    };

    for (int c = 0; c < 300; ++c) {
        setAll(false, rng() & 1, static_cast<uint8_t>(rng()));
        evalBoth();
        if (rng() & 1) {  // stir inputs while the clock sits low
            setAll(false, rng() & 1, static_cast<uint8_t>(rng()));
            evalBoth();
        }
        setAll(true, rng() & 1, static_cast<uint8_t>(rng()));
        ++posedges;  // this eval applies the posedge
        evalBoth();
    }
}

TEST_CASE("alias: degenerate alias cycle elaborates and converges") {
    AlLoop a;
    a.elaborate();
    a.eval();
    CHECK(a.o.get() == false);
}

TEST_CASE("alias: aliased entity reads through (wave/testbench view)") {
    AlTop top;
    top.elaborate();
    top.din.set(41);
    top.en.set(true);
    top.clk.set(0);
    top.eval();
    top.clk.set(1);
    top.eval();
    // c0.r loaded 41+1; dout aliases r's storage, out2 aliases dout.
    CHECK(top.c0.r.get() == 42);
    CHECK(top.c0.dout.get() == 42);
    CHECK(top.out2.get() == 42);
    // c1.din aliases c0.dout; after the next edge c1.r holds c0's value + 1.
    top.clk.set(0);
    top.eval();
    top.clk.set(1);
    top.eval();
    CHECK(top.c1.r.get() == 43);
    // Conversion copy is a real action: conv = uint32_t(c1.dout) after eval.
    CHECK(top.out3.get() == top.c1.dout.get());
}

}  // namespace
