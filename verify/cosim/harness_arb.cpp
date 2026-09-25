// FixedArb（chisel3.util.Arbiter）/ RRArb（chisel3.util.RRArbiter）对拍
// harness：N=4。激励：定向全 valid 连发（轮转/粘性拍序）+ 随机掩码密度分段。
// 每拍比对 in_ready[i]（注意 chisel 的 ready 不门控自身 valid——逐路全比对
// 恰好钉住这一点）/ out_valid / out.bits(valid 时) / chosen。

#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>

#include "VFixedArbRef_n4.h"
#include "VRRArbRef_n4.h"

#include "common.h"

#include <wolvicmod/wolvicmod.h>
#include <wolvicmod/prefab/prefab.h>

using namespace wolvicmod;
using namespace wolvicmod::prefab;

namespace {

template <class VRef, template <class, uint32_t> class ArbT>
uint64_t cosimArb(const char* comp, const char* cfg, uint32_t seed, uint64_t cycles) {
    VRef ref;
    ArbT<uint32_t, 4> dut;
    dut.elaborate();
    std::mt19937 rng(seed);
    cosim::Stats st;
    cosim::Replay rp;
    constexpr uint32_t N = 4;
    using InArr = std::array<Dec<uint32_t>, N>;

    cosim::resetRef(ref, [&] {
        ref.in_valid = 0;
        ref.out_ready = 0;
    });
    {
        InArr ins{};
        dut.in.set(ins);
    }
    dut.out_rdy.set(false);
    dut.clk.set(0);
    dut.eval();

    const uint64_t directedEnd = cycles / 5;
    for (uint64_t c = 0; c < cycles; ++c) {
        uint64_t vm;
        bool odr;
        if (c < directedEnd) {
            // 定向：全 valid 连发 / 全 valid 反压 / 单路轮换 / 间隔掩码
            const uint64_t ph = (c / 8) % 4;
            if (ph == 0)      vm = 0b1111;
            else if (ph == 1) vm = 0b1111;
            else if (ph == 2) vm = 1u << ((c / 8) % N);
            else              vm = 0b1010;
            odr = (ph != 1);
        } else {
            const uint32_t pct = cosim::densityAt(c - directedEnd, cycles - directedEnd);
            vm = 0;
            for (uint32_t i = 0; i < N; ++i) vm |= cosim::roll(rng, pct) ? (1ull << i) : 0;
            odr = cosim::roll(rng, pct);
        }

        InArr ins{};
        for (uint32_t i = 0; i < N; ++i) ins[i] = {((vm >> i) & 1u) != 0, static_cast<uint32_t>(rng())};

        ref.in_valid = vm;
        ref.in_bits_0 = ins[0].bits;
        ref.in_bits_1 = ins[1].bits;
        ref.in_bits_2 = ins[2].bits;
        ref.in_bits_3 = ins[3].bits;
        ref.out_ready = odr;
        dut.in.set(ins);
        dut.out_rdy.set(odr);
        rp.push("vm=0x" + std::to_string(vm) + " odr=" + std::to_string(odr));

        cosim::phaseLow(ref, dut);
        const uint64_t refRdy[N] = {ref.in_ready_0, ref.in_ready_1, ref.in_ready_2, ref.in_ready_3};
        for (uint32_t i = 0; i < N; ++i)
            cosim::check(st, comp, cfg, seed, c, cosim::lanePort("in_ready", i).c_str(),
                         refRdy[i], dut.in_rdy.get()[i], rp);
        cosim::check(st, comp, cfg, seed, c, "out_valid", ref.out_valid, dut.out.get().valid, rp);
        if (ref.out_valid && dut.out.get().valid)
            cosim::check(st, comp, cfg, seed, c, "out_bits", ref.out_bits, dut.out.get().bits, rp);
        cosim::check(st, comp, cfg, seed, c, "chosen", ref.chosen, dut.chosen.get(), rp);
        cosim::phaseHigh(ref, dut);
    }
    if (st.mismatches > 0) {
        std::cout << "FAIL " << comp << " " << cfg << " seed=" << seed
                  << " mismatches=" << st.mismatches << "/" << st.checks << "\n";
        return st.mismatches;
    }
    std::cout << "PASS " << comp << " " << cfg << " seed=" << seed << " cycles=" << cycles
              << " checks=" << st.checks << "\n";
    return 0;
}

}  // namespace

int main() {
    uint64_t bad = 0;
    for (uint32_t seed : {8u, 88u, 888u}) {
        bad += cosimArb<VFixedArbRef_n4, FixedArb>("fixedarb", "n4", seed, 100000);
        bad += cosimArb<VRRArbRef_n4, RRArb>("rrarb", "n4", seed, 100000);
    }
    std::cout << (bad ? "FAIL arb" : "ALL-PASS arb") << "\n";
    return bad ? 1 : 0;
}
