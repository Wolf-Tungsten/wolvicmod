// M6: Mem row-level commit semantics (§2.4) and hierarchy end-to-end
// simulation (§3.3).

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>

using namespace wolvicmod;

namespace {

TEST_CASE("M6: two Updates on one Mem merge per row (§2.4)") {
    struct M : Module {
        IN(bool, clk);
        IN(uint32_t, a_row);
        IN(uint32_t, a_val);
        IN(uint32_t, b_row);
        IN(uint32_t, b_val);
        IN(uint32_t, raddr);
        OUT(uint32_t, rdata);
        MEM(uint32_t, 16, mem);
        M() {
            // both ports fire on the same clock edge
            mem.update().on(posedge(clk)).addr(a_row).reads(a_val) = [](auto src) {
                auto [v] = src;
                return v;
            };
            mem.update().on(posedge(clk)).addr(b_row).reads(b_val) = [](auto src) {
                auto [v] = src;
                return v;
            };
            rdata.assign().reads(mem, raddr) = [](auto src) {
                auto [m, a] = src;
                return m[a];
            };
        }
    };
    M top;
    top.elaborate();
    // different rows: both writes land
    top.a_row.set(1);
    top.a_val.set(111);
    top.b_row.set(2);
    top.b_val.set(222);
    top.clk.set(0);
    top.eval();
    top.clk.set(1);
    top.eval();
    top.raddr.set(1);
    top.eval();
    CHECK(top.rdata.get() == 111);
    top.raddr.set(2);
    top.eval();
    CHECK(top.rdata.get() == 222);
    // same row: the earlier-registered Update (a_port) wins
    top.a_row.set(3);
    top.a_val.set(1000);
    top.b_row.set(3);
    top.b_val.set(2000);
    top.clk.set(0);
    top.eval();
    top.clk.set(1);
    top.eval();
    top.raddr.set(3);
    top.eval();
    CHECK(top.rdata.get() == 1000);
}

TEST_CASE("M6: Mem commit cost does not touch unwritten rows") {
    struct M : Module {
        IN(bool, clk);
        IN(uint32_t, waddr);
        IN(uint32_t, wdata);
        IN(uint32_t, raddr);
        OUT(uint32_t, rdata);
        MEM(uint32_t, 4096, mem);
        M() {
            mem.update().on(posedge(clk)).addr(waddr).reads(wdata) = [](auto src) {
                auto [v] = src;
                return v;
            };
            rdata.assign().reads(mem, raddr) = [](auto src) {
                auto [m, a] = src;
                return m[a];
            };
        }
    };
    M top;
    top.elaborate();
    top.waddr.set(4000);
    top.wdata.set(77);
    top.clk.set(0);
    top.eval();
    top.clk.set(1);
    top.eval();
    top.raddr.set(4000);
    top.eval();
    CHECK(top.rdata.get() == 77);
    top.raddr.set(3999);
    top.eval();
    CHECK(top.rdata.get() == 0);
}

// §3.3: parameterized shift register + sort pipeline, driven end to end.
template <class T, int N>
struct ShiftReg : Module {
    using Stages = std::array<T, N>;

    IN(T, din);
    IN(bool, clk);
    OUT(T, dout);
    REG(Stages, q);

    ShiftReg() {
        q.update().on(posedge(clk)).reads(din, q) = [](auto src) {
            auto [d, qq] = src;
            Stages next;
            next[0] = d;
            for (int i = 1; i < N; ++i) next[i] = qq[i - 1];
            return next;
        };
        dout.assign().reads(q) = [](auto src) {
            auto [qq] = src;
            return qq[N - 1];
        };
    }
};

struct SortPipeline : Module {
    using Vec = std::vector<int>;
    using SR = ShiftReg<Vec, 3>;

    IN(Vec, din);
    IN(bool, clk);
    OUT(Vec, dout);
    REG(Vec, sorted);
    SUB(SR, sr);

    SortPipeline() {
        sorted.update().on(posedge(clk)).reads(din) = [](auto src) {
            auto [v] = src;
            auto w = v;
            std::sort(w.begin(), w.end());
            return w;
        };
        sr.din = sorted;
        sr.clk = clk;
        dout = sr.dout;
    }
};

TEST_CASE("M6: sort pipeline produces sorted output after 4 beats (§3.3)") {
    SortPipeline top;
    top.elaborate();
    top.din.set({5, 1, 4, 1, 9});
    for (int beat = 1; beat <= 4; ++beat) {
        top.clk.set(0);
        top.eval();
        top.clk.set(1);
        top.eval();
        if (beat < 4) CHECK(top.dout.get().empty());  // still shifting
    }
    CHECK(top.dout.get() == std::vector<int>({1, 1, 4, 5, 9}));
}

TEST_CASE("M6: sibling wiring forms a datapath (§3.3)") {
    struct Stage : Module {
        IN(uint32_t, din);
        IN(bool, clk);
        OUT(uint32_t, dout);
        REG(uint32_t, q);
        Stage() {
            q.update().on(posedge(clk)).reads(din) = [](auto src) {
                auto [d] = src;
                return d + 1;
            };
            dout = q;
        }
    };
    struct Top : Module {
        IN(uint32_t, din);
        IN(bool, clk);
        OUT(uint32_t, dout);
        SUB(Stage, s0);
        SUB(Stage, s1);
        Top() {
            s0.din = din;
            s0.clk = clk;
            s1.din = s0.dout;  // sibling to sibling
            s1.clk = clk;
            dout = s1.dout;
        }
    };
    Top top;
    top.elaborate();
    top.din.set(10);
    // beat 1: s0.q <- 11, s1.q <- old s0.dout(0)+1 = 1
    top.clk.set(0);
    top.eval();
    top.clk.set(1);
    top.eval();
    CHECK(top.dout.get() == 1);
    // beat 2: s0.q <- 11, s1.q <- 12
    top.clk.set(0);
    top.eval();
    top.clk.set(1);
    top.eval();
    CHECK(top.dout.get() == 12);
}

}  // namespace
