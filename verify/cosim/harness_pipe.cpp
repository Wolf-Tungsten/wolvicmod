// ValidPipe（chisel3.util.Pipe）对拍 harness：latency=1/3/5。
// 激励：定向背靠背 burst + stall 气泡段 + 随机密度分段。
// 每拍比对 deq_valid / deq.bits（valid 时）。

#include <cstdint>
#include <iostream>
#include <random>
#include <string>

#include "VPipeRef_l1.h"
#include "VPipeRef_l3.h"
#include "VPipeRef_l5.h"

#include "common.h"

#include <wolvicmod/wolvicmod.h>
#include <wolvicmod/prefab/prefab.h>

using namespace wolvicmod;
using namespace wolvicmod::prefab;

namespace {

template <class VRef, uint32_t L>
uint64_t cosimPipe(const char* cfg, uint32_t seed, uint64_t cycles) {
    VRef ref;
    ValidPipe<uint32_t, L> dut;
    dut.elaborate();
    std::mt19937 rng(seed);
    cosim::Stats st;
    cosim::Replay rp;

    cosim::resetRef(ref, [&] {
        ref.enq_valid = 0;
        ref.enq_bits = 0;
    });
    dut.enq.set({false, 0});
    dut.clk.set(0);
    dut.eval();

    const uint64_t directedEnd = cycles / 5;
    for (uint64_t c = 0; c < cycles; ++c) {
        bool ev;
        if (c < directedEnd) {
            // 定向：burst（4 拍连续）与 stall（2 拍空）交替，恰好穿过各延迟级
            const uint64_t ph = c % 6;
            ev = ph < 4;
        } else {
            ev = cosim::roll(rng, cosim::densityAt(c - directedEnd, cycles - directedEnd));
        }
        const uint32_t bits = rng();

        ref.enq_valid = ev;
        ref.enq_bits = bits;
        dut.enq.set({ev, bits});
        rp.push("ev=" + std::to_string(ev) + " bits=0x" + [&] {
            char b[16];
            std::snprintf(b, sizeof b, "%08x", bits);
            return std::string(b);
        }());

        cosim::phaseLow(ref, dut);
        cosim::check(st, "pipe", cfg, seed, c, "deq_valid", ref.deq_valid, dut.deq.get().valid, rp);
        if (ref.deq_valid && dut.deq.get().valid)
            cosim::check(st, "pipe", cfg, seed, c, "deq_bits", ref.deq_bits, dut.deq.get().bits, rp);
        cosim::phaseHigh(ref, dut);
    }
    if (st.mismatches > 0) {
        std::cout << "FAIL pipe " << cfg << " seed=" << seed
                  << " mismatches=" << st.mismatches << "/" << st.checks << "\n";
        return st.mismatches;
    }
    std::cout << "PASS pipe " << cfg << " seed=" << seed << " cycles=" << cycles
              << " checks=" << st.checks << "\n";
    return 0;
}

}  // namespace

int main() {
    uint64_t bad = 0;
    for (uint32_t seed : {7u, 77u, 777u}) {
        bad += cosimPipe<VPipeRef_l1, 1>("l1", seed, 100000);
        bad += cosimPipe<VPipeRef_l3, 3>("l3", seed, 100000);
        bad += cosimPipe<VPipeRef_l5, 5>("l5", seed, 100000);
    }
    std::cout << (bad ? "FAIL pipe" : "ALL-PASS pipe") << "\n";
    return bad ? 1 : 0;
}
