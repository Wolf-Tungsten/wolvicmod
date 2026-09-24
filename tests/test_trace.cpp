// M8: round-level event trace (doc §6.3) — edge hits, activated Updates, and
// committed state writes per round.

#include <sstream>
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

TEST_CASE("M8: trace records edges, activations, and commits per round") {
    Counter top;
    top.elaborate();
    std::ostringstream os;
    top.traceOn(os);

    top.rst_n.set(1);
    top.eval();
    top.rst_n.set(0);
    top.eval();  // negedge rst_n: reset activates and commits cnt <- 0
    top.rst_n.set(1);
    top.eval();

    const std::string out = os.str();
    CHECK(out.find("[eval 0 round 0]") != std::string::npos);
    CHECK(out.find("edge: negedge top.rst_n") != std::string::npos);
    CHECK(out.find("activate: update -> top.cnt") != std::string::npos);
    CHECK(out.find("commit: top.cnt = 0b") != std::string::npos);
    top.traceOff();
}

TEST_CASE("M8: cascaded clocks show up as multiple rounds in one eval") {
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
    std::ostringstream os;
    top.traceOn(os);

    top.clk.set(0);
    top.eval();
    os.str("");  // discard the quiet eval
    top.clk.set(1);
    top.eval();  // round 0: div2 commits; round 1: cnt commits

    const std::string out = os.str();
    CHECK(out.find("[eval 1 round 0]") != std::string::npos);
    CHECK(out.find("[eval 1 round 1]") != std::string::npos);
    CHECK(out.find("edge: posedge top.clk") != std::string::npos);
    CHECK(out.find("edge: posedge top.div2") != std::string::npos);
    CHECK(out.find("commit: top.div2") != std::string::npos);
    CHECK(out.find("commit: top.cnt = 0b00000000000000000000000000000001") != std::string::npos);
    top.traceOff();
}

TEST_CASE("M8: idempotent eval traces no commits; traceOff silences") {
    Counter top;
    top.elaborate();
    top.rst_n.set(1);
    top.eval();
    top.rst_n.set(0);
    top.eval();
    top.rst_n.set(1);
    top.eval();
    top.en.set(1);
    top.clk.set(0);
    top.eval();
    top.clk.set(1);
    top.eval();  // cnt = 1

    std::ostringstream os;
    top.traceOn(os);
    top.eval();  // nothing changes
    const std::string quiet = os.str();
    CHECK(quiet.find("commit:") == std::string::npos);
    CHECK(quiet.find("activate:") == std::string::npos);

    top.traceOff();
    os.str("");
    top.clk.set(0);
    top.eval();
    top.clk.set(1);
    top.eval();  // cnt = 2, but trace is off
    CHECK(os.str().empty());
    CHECK(top.cnt.get() == 2);
}

TEST_CASE("M8: traceOn() before elaborate() is an error") {
    Counter top;
    std::ostringstream os;
    CHECK_THROWS_AS(top.traceOn(os), Error);
}

}  // namespace
