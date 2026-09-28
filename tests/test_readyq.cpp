// Push-dispatch scheduler (§5.2 fast path): level-bucketed ready queue fed by
// the reverse dependency map. These tests pin the push engine's equivalence
// with full re-evaluation, including the SCC re-activation regression: an SCC
// group's internal edges once self-pushed the group during its own drain; the
// bucket clear wiped the pending entry with its queued flag stuck set, so the
// group never ran again.

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>

#include <random>
#include <string>
#include <vector>

using namespace wolvicmod;

namespace {

// A combinational ready/valid-style cycle (SCC): a = in1 & b, b = in2 | a.
// Converges by iteration; with in1=1/in2=0 it holds state through the cycle.
// A Reg feeds in1 so the group must re-activate on commits — repeatedly.
struct CombCycle : Module {
    IN(bool, clk);
    IN(bool, en);
    IN(bool, in2);
    WIRE(bool, a);
    WIRE(bool, b);
    REG(bool, in1_r);
    OUT(bool, out_a);
    OUT(bool, out_b);

    CombCycle() {
        in1_r.update().on(posedge(clk)) = en;
        a.assign().reads(in1_r, b) = [](auto src) {
            auto [i1, bb] = src;
            return i1 && bb;
        };
        b.assign().reads(in2, a) = [](auto src) {
            auto [i2, aa] = src;
            return i2 || aa;
        };
        out_a = a;
        out_b = b;
    }
};

void tick(Module& m, In<bool>& clk) {
    clk.set(0);
    m.eval();
    clk.set(1);
    m.eval();
}

TEST_CASE("push: SCC group re-activates on every input change") {
    CombCycle push;
    push.elaborate();
    CombCycle full;
    full.elaborate();
    full.dirtyEvalOff();

    // Phase 1: in2=1 -> b=1; then en=1 latches in1_r=1 -> a=1.
    push.in2.set(true);
    full.in2.set(true);
    tick(push, push.clk);
    tick(full, full.clk);
    CHECK(push.out_b.get() == full.out_b.get());
    push.en.set(true);
    full.en.set(true);
    tick(push, push.clk);
    tick(full, full.clk);
    CHECK(push.out_a.get() == true);
    CHECK(push.out_a.get() == full.out_a.get());

    // Phase 2 (the regression): in2 falls, then en falls; with in1_r=1 and
    // in2=0 the cycle holds its previous a — then en=0 must drop a again.
    // Each transition requires the group to re-activate, not just the first.
    for (int i = 0; i < 3; ++i) {
        push.in2.set(false);
        full.in2.set(false);
        tick(push, push.clk);
        tick(full, full.clk);
        CHECK(push.out_a.get() == full.out_a.get());  // holds 1 (comb latch)
        CHECK(push.out_b.get() == full.out_b.get());
        push.en.set(false);
        full.en.set(false);
        tick(push, push.clk);
        tick(full, full.clk);
        CHECK(push.out_a.get() == false);
        CHECK(push.out_a.get() == full.out_a.get());
        push.en.set(true);
        full.en.set(true);
        tick(push, push.clk);
        tick(full, full.clk);
        CHECK(push.out_a.get() == full.out_a.get());
    }
}

// Mixed model: reg -> comb chain, a comb cycle, a Mem, guarded Updates —
// driven with fixed-seed pseudo-random inputs; push must match full every
// half-cycle on every waveable entity.
struct Mixed : Module {
    IN(bool, clk);
    IN(bool, en);
    IN(uint8_t, din);
    OUT(uint8_t, dout);
    REG(uint8_t, r0);
    REG(uint8_t, r1);
    WIRE(uint8_t, w1);
    WIRE(uint8_t, w2);
    WIRE(uint8_t, w3);
    WIRE(uint8_t, addr_w);
    MEM(uint8_t, 4, mem);
    WIRE(bool, cyc_a);
    WIRE(bool, cyc_b);

    Mixed() {
        r0.update().on(posedge(clk)).en(en).reads(din) = [](auto src) {
            auto [v] = src;
            return static_cast<uint8_t>(v + 1);
        };
        addr_w.assign().reads(din) = [](auto src) {
            auto [v] = src;
            return static_cast<uint8_t>(v & 3);
        };
        w1.assign().reads(r0, din) = [](auto src) {
            auto [r, v] = src;
            return static_cast<uint8_t>(r * 2 + v);
        };
        w2.assign().reads(w1, r1) = [](auto src) {
            auto [x, y] = src;
            return static_cast<uint8_t>(x ^ y);
        };
        mem.update().on(posedge(clk)).addr(addr_w).reads(w2) = [](auto src) {
            auto [v] = src;
            return v;
        };
        w3.assign().reads(mem, w2) = [](auto src) {
            auto [m, v] = src;
            return static_cast<uint8_t>(m[0] + v);
        };
        r1.update().on(posedge(clk)).en(en).reads(w3) = [](auto src) {
            auto [v] = src;
            return v;
        };
        // 组合环：cyc_a = en & cyc_b；cyc_b = (din[0]) | cyc_a
        cyc_a.assign().reads(en, cyc_b) = [](auto src) {
            auto [e, b] = src;
            return e && b;
        };
        cyc_b.assign().reads(din, cyc_a) = [](auto src) {
            auto [v, a] = src;
            return ((v & 1) != 0) || a;
        };
        dout.assign().reads(w2, cyc_b) = [](auto src) {
            auto [v, c] = src;
            return static_cast<uint8_t>(v + (c ? 1 : 0));
        };
    }
};

void collectEntities(Module* m, std::vector<Entity*>& out) {
    for (auto& e : m->entities()) out.push_back(e.get());
    for (auto& c : m->children()) collectEntities(c.get(), out);
}

TEST_CASE("push ≡ full on a mixed model with random stimulus") {
    Mixed a;  // push (default)
    a.elaborate();
    Mixed b;
    b.elaborate();
    b.dirtyEvalOff();

    std::vector<Entity*> ea, eb;
    collectEntities(&a, ea);
    collectEntities(&b, eb);
    REQUIRE(ea.size() == eb.size());

    auto checkAll = [&]() {
        for (size_t i = 0; i < ea.size(); ++i) {
            if (!ea[i]->waveSupported()) continue;
            std::string sa, sb;
            ea[i]->waveFormat(sa);
            eb[i]->waveFormat(sb);
            CAPTURE(ea[i]->hierPath());
            CHECK(sa == sb);
        }
    };

    std::mt19937 rng(42);
    for (int c = 0; c < 300; ++c) {
        const uint8_t v = static_cast<uint8_t>(rng());
        const bool en = (rng() & 1) != 0;
        a.din.set(v);
        b.din.set(v);
        a.en.set(en);
        b.en.set(en);
        a.clk.set(0);
        b.clk.set(0);
        a.eval();
        b.eval();
        checkAll();
        a.clk.set(1);
        b.clk.set(1);
        a.eval();
        b.eval();
        checkAll();
    }
}

}  // namespace
