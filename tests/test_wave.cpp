// M7: FST waveform dump (doc §6.3) — files are verified by reading them back
// with the same vendored libfst reader API.
// Skipped entirely when built with WOLVICMOD_WITH_FST=OFF.

#ifdef WOLVICMOD_WITH_FST

#include <cstdio>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>

#include <fstapi.h>

using namespace wolvicmod;

// A user-defined value type, made dumpable via an FstFormat specialization.
// The specialization must precede its first use in this TU.
struct Fix16 {
    int16_t v = 0;
};

template <>
struct wolvicmod::FstFormat<Fix16, void> {
    static constexpr uint32_t bits = 16;
    static void format(std::string& out, const Fix16& x) {
        wolvicmod::FstFormat<int16_t>::format(out, x.v);
    }
};

namespace {

TEST_CASE("M7: FstFormat built-ins") {
    std::string s;
    FstFormat<bool>::format(s, true);
    CHECK(s == "1");
    FstFormat<uint8_t>::format(s, 0b10100001);
    CHECK(s == "10100001");
    FstFormat<int8_t>::format(s, -1);
    CHECK(s == "11111111");
    FstFormat<uint32_t>::format(s, 5u);
    CHECK(s == "00000000000000000000000000000101");
    static_assert(!FstFormattable<std::vector<int>>);
    static_assert(FstFormattable<uint64_t>);
}

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

// Read back an FST file: hierarchical name -> handle map, values at times.
struct FstReadback {
    fstReaderContext* ctx = nullptr;
    std::map<std::string, fstHandle> handles;

    explicit FstReadback(const char* file) {
        ctx = fstReaderOpen(file);
        REQUIRE(ctx != nullptr);
        std::vector<std::string> scopes;
        for (struct fstHier* h = fstReaderIterateHier(ctx); h != nullptr;
             h = fstReaderIterateHier(ctx)) {
            if (h->htyp == FST_HT_SCOPE) {
                scopes.push_back(h->u.scope.name);
            } else if (h->htyp == FST_HT_UPSCOPE) {
                scopes.pop_back();
            } else if (h->htyp == FST_HT_VAR) {
                std::string path;
                for (const auto& s : scopes) path += s + ".";
                path += h->u.var.name;
                handles[path] = h->u.var.handle;
            }
        }
    }
    ~FstReadback() {
        if (ctx != nullptr) fstReaderClose(ctx);
    }

    std::string valueAt(const std::string& path, uint64_t time) {
        auto it = handles.find(path);
        REQUIRE(it != handles.end());
        char buf[256];
        fstReaderGetValueFromHandleAtTime(ctx, time, it->second, buf);
        return buf;
    }
};

TEST_CASE("M7: counter dump reads back with correct hierarchy and values") {
    const char* file = "m7_counter.fst";
    {
        Counter top;
        top.elaborate();
        top.waveOn(file);
        // Verilator-style: the user advances time explicitly; steps are
        // non-uniform on purpose (10 then 5).
        uint64_t t = 100;
        top.rst_n.set(1);
        top.eval();
        top.waveDump(t);  // 100: initial values
        top.rst_n.set(0);
        top.eval();
        top.waveDump(t += 10);  // 110: reset
        top.rst_n.set(1);
        top.eval();
        top.waveDump(t += 10);  // 120
        top.en.set(1);
        for (int i = 0; i < 3; ++i) {
            top.clk.set(0);
            top.eval();
            top.waveDump(t += 5);  // 125, 135, 145
            top.clk.set(1);
            top.eval();
            top.waveDump(t += 5);  // 130, 140, 150 — posedge, cnt increments
        }
        top.waveOff();
    }

    FstReadback rb(file);
    CHECK(rb.handles.count("top.clk") == 1);
    CHECK(rb.handles.count("top.rst_n") == 1);
    CHECK(rb.handles.count("top.en") == 1);
    CHECK(rb.handles.count("top.dout") == 1);
    CHECK(rb.handles.count("top.cnt") == 1);
    CHECK(rb.handles.size() == 5);

    // cnt: 0 during reset, 1/2/3 after each posedge (times 130/140/150)
    CHECK(rb.valueAt("top.cnt", 100) == std::string(32, '0'));
    CHECK(rb.valueAt("top.cnt", 120) == std::string(32, '0'));
    CHECK(rb.valueAt("top.cnt", 130) == std::string(31, '0') + "1");
    CHECK(rb.valueAt("top.cnt", 140) == std::string(30, '0') + "10");
    CHECK(rb.valueAt("top.cnt", 150) == std::string(30, '0') + "11");
    CHECK(rb.valueAt("top.dout", 150) == std::string(30, '0') + "11");
    CHECK(rb.valueAt("top.clk", 150) == "1");
    CHECK(rb.valueAt("top.clk", 145) == "0");
    CHECK(fstReaderGetStartTime(rb.ctx) == 100);
    CHECK(fstReaderGetEndTime(rb.ctx) == 150);

    std::remove(file);
}

TEST_CASE("M7: waveDump guards (§6.3)") {
    Counter top;
    top.elaborate();
    CHECK_THROWS_AS(top.waveDump(0), Error);  // waveOn() not called
    top.waveOn("m7_guards.fst");
    CHECK_NOTHROW(top.waveDump(10));
    CHECK_NOTHROW(top.waveDump(10));  // equal time is allowed
    CHECK_THROWS_AS(top.waveDump(5), Error);  // backward time is not
    top.waveOff();
    std::remove("m7_guards.fst");
}

TEST_CASE("M7: non-formattable entities and Mem are skipped") {
    const char* file = "m7_skip.fst";
    struct M : Module {
        IN(bool, clk);
        OUT(uint32_t, o);
        WIRE(std::vector<int>, v);  // no FstFormat specialization
        MEM(uint32_t, 16, mem);     // Mem: no FST memory abstraction
        REG(std::vector<int>, r);   // no FstFormat specialization
        M() {
            v = std::vector<int>{};
            r.update().on(posedge(clk)) = std::vector<int>{};
            mem.update().on(posedge(clk)).addr(o).reads(o) = [](auto src) {
                auto [x] = src;
                return x;
            };
            o = 1;
        }
    };
    {
        M top;
        top.elaborate();
        top.waveOn(file);
        top.clk.set(1);
        top.eval();
        top.waveDump(0);
        top.waveOff();
    }
    FstReadback rb(file);
    CHECK(rb.handles.count("top.clk") == 1);
    CHECK(rb.handles.count("top.o") == 1);
    CHECK(rb.handles.count("top.v") == 0);
    CHECK(rb.handles.count("top.mem") == 0);
    CHECK(rb.handles.count("top.r") == 0);
    std::remove(file);
}

TEST_CASE("M7: waveOn() before elaborate() is an error") {
    Counter top;
    CHECK_THROWS_AS(top.waveOn("m7_never.fst"), Error);
}

// A user-defined value type, made dumpable via an FstFormat specialization.
TEST_CASE("M7: user-defined FstFormat specialization is dumped") {
    struct M : Module {
        IN(bool, clk);
        REG(Fix16, r);
        OUT(int16_t, o);
        M() {
            r.update().on(posedge(clk)).reads(r) = [](auto src) {
                auto [x] = src;
                return Fix16{static_cast<int16_t>(x.v + 2)};
            };
            o.assign().reads(r) = [](auto src) {
                auto [x] = src;
                return x.v;
            };
        }
    };
    const char* file = "m7_custom.fst";
    {
        M top;
        top.elaborate();
        top.waveOn(file);
        top.clk.set(1);
        top.eval();
        top.waveDump(7);
        top.waveOff();
    }
    FstReadback rb(file);
    CHECK(rb.valueAt("top.r", 7) == std::string(14, '0') + "10");  // 2, 16 bits
    std::remove(file);
}

}  // namespace

#endif  // WOLVICMOD_WITH_FST
