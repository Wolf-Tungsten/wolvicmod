// M9: debug switches (doc §6.4) — read-set accounting (stealth reads) and the
// Update mutual-exclusion assertion. Built with WOLVICMOD_AUDIT=1 (see
// tests/CMakeLists.txt); the runtime switches default to off.

#include <string>

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
        dout = cnt;
    }
};

TEST_CASE("M9: clean models pass with audit enabled") {
    Counter top;
    top.elaborate();
    top.auditOn();
    top.rst_n.set(1);
    top.eval();
    top.rst_n.set(0);
    top.eval();
    top.rst_n.set(1);
    top.eval();
    top.en.set(1);
    top.clk.set(1);
    CHECK_NOTHROW(top.eval());
    CHECK(top.dout.get() == 1);
}

TEST_CASE("M9: stealth read via get() is caught at first eval (§6.4)") {
    struct Sneaky : Module {
        IN(uint32_t, a);
        IN(uint32_t, b);
        OUT(uint32_t, sum);
        Sneaky() {
            sum.assign().reads(a) = [this](auto src) {
                auto [x] = src;
                return x + b.get();  // b is read but not declared
            };
        }
    };
    Sneaky top;
    top.elaborate();
    top.auditOn();
    top.a.set(1);
    top.b.set(2);
    try {
        top.eval();
        FAIL("expected the stealth read to throw");
    } catch (const Error& e) {
        const std::string msg = e.what();
        CHECK(msg.find("stealth read") != std::string::npos);
        CHECK(msg.find("top.b") != std::string::npos);
    }
}

TEST_CASE("M9: without auditOn the same model simulates silently") {
    struct Sneaky : Module {
        IN(uint32_t, a);
        IN(uint32_t, b);
        OUT(uint32_t, sum);
        Sneaky() {
            sum.assign().reads(a) = [this](auto src) {
                auto [x] = src;
                return x + b.get();
            };
        }
    };
    Sneaky top;
    top.elaborate();
    top.a.set(1);
    top.b.set(2);
    top.eval();
    CHECK(top.sum.get() == 3);  // wrong-but-silent without the audit
}

TEST_CASE("M9: expression fast path declares its leaves, audit stays quiet") {
    struct M : Module {
        IN(uint32_t, a);
        IN(uint32_t, b);
        OUT(uint32_t, y);
        M() { y = a * b + 1; }
    };
    M top;
    top.elaborate();
    top.auditOn();
    top.a.set(3);
    top.b.set(4);
    CHECK_NOTHROW(top.eval());
    CHECK(top.y.get() == 13);
}

TEST_CASE("M9: guard/address/event reads are declared, audit stays quiet") {
    struct DataMem : Module {
        IN(bool, clk);
        IN(bool, wen);
        IN(uint32_t, waddr);
        IN(uint32_t, wdata);
        IN(uint32_t, raddr);
        OUT(uint32_t, rdata);
        MEM(uint32_t, 16, mem);
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
    top.auditOn();
    top.wen.set(1);
    top.waddr.set(3);
    top.wdata.set(9);
    top.clk.set(1);
    CHECK_NOTHROW(top.eval());
    top.wen.set(0);
    top.raddr.set(3);
    CHECK_NOTHROW(top.eval());
    CHECK(top.rdata.get() == 9);
}

TEST_CASE("M9: undeclared Mem row read is a stealth read too") {
    struct Sneaky : Module {
        IN(uint32_t, a);
        OUT(uint32_t, o);
        MEM(uint32_t, 8, mem);
        IN(bool, clk);
        Sneaky() {
            mem.update().on(posedge(clk)).addr(a).reads(a) = [](auto src) {
                auto [v] = src;
                return v;
            };
            o.assign().reads(a) = [this](auto src) {
                auto [v] = src;
                return v + mem[0];  // mem not in this assign's read set
            };
        }
    };
    Sneaky top;
    top.elaborate();
    top.auditOn();
    top.a.set(0);
    try {
        top.eval();
        FAIL("expected the stealth read to throw");
    } catch (const Error& e) {
        CHECK(std::string(e.what()).find("top.mem") != std::string::npos);
    }
}

TEST_CASE("M9: Update mutex assertion reports simultaneous activation (§6.4)") {
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
    // default: registration-order priority applies silently
    top.clk.set(1);
    CHECK_NOTHROW(top.eval());
    CHECK(top.dout.get() == 7);
    // with the assertion enabled the same situation is reported
    top.assertUpdateMutexOn();
    top.clk.set(0);
    top.eval();
    top.clk.set(1);
    try {
        top.eval();
        FAIL("expected the mutex assertion to throw");
    } catch (const Error& e) {
        CHECK(std::string(e.what()).find("top.cnt") != std::string::npos);
    }
    top.assertUpdateMutexOff();
    top.clk.set(0);
    top.eval();
    top.clk.set(1);
    CHECK_NOTHROW(top.eval());
}

TEST_CASE("M9: audit switches require elaborate() first") {
    Counter top;
    CHECK_THROWS_AS(top.auditOn(), Error);
    CHECK_THROWS_AS(top.assertUpdateMutexOn(), Error);
}

}  // namespace
