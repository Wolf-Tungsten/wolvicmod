// Queue（chisel3.util.Queue）对拍 harness：5 个配置 × 3 seed。
// 激励：定向灌满→排空振荡（含满时同拍 enq+deq、空直通拍序）+ 随机密度分段
// （100%/50%/10%）。每拍比对 enq_ready / deq_valid / deq.bits(valid 时) / count。

#include <cstdint>
#include <iostream>
#include <random>
#include <string>

#include "VQueueRef_e1_p1_f0.h"
#include "VQueueRef_e2_p0_f0.h"
#include "VQueueRef_e4_p0_f0.h"
#include "VQueueRef_e2_p0_f1.h"
#include "VQueueRef_e4_p1_f1.h"

#include "common.h"

#include <wolvicmod/wolvicmod.h>
#include <wolvicmod/prefab/prefab.h>

using namespace wolvicmod;
using namespace wolvicmod::prefab;

namespace {

template <class VRef, uint32_t N, bool Flow, bool Pipe>
uint64_t cosimQueue(const char* cfg, uint32_t seed, uint64_t cycles) {
    VRef ref;
    Queue<uint32_t, N, Flow, Pipe> dut;
    dut.elaborate();
    std::mt19937 rng(seed);
    cosim::Stats st;
    cosim::Replay rp;

    cosim::resetRef(ref, [&] {
        ref.enq_valid = 0;
        ref.enq_bits = 0;
        ref.deq_ready = 0;
    });
    dut.enq.set({false, 0});
    dut.deq_rdy.set(false);
    dut.clk.set(0);
    dut.eval();

    // 定向振荡相位占前 20%：灌满→（满时同拍推拉）→排空循环；
    // flow 配置在排空后插空直通拍序。
    const uint64_t directedEnd = cycles / 5;
    bool filling = true;
    for (uint64_t c = 0; c < cycles; ++c) {
        bool ev, dr;
        if (c < directedEnd) {
            const uint32_t cnt = dut.count.get();
            if (filling) {
                if (cnt == N) {
                    // 满：pipe 配置同拍推拉数拍，然后转入排空
                    if (Pipe && (c % 4) != 3) {
                        ev = true;
                        dr = true;
                    } else {
                        ev = false;
                        dr = true;
                        filling = false;
                    }
                } else {
                    ev = true;
                    dr = false;
                }
            } else {
                if (cnt == 0) {
                    filling = true;
                    if (Flow) {
                        // 空直通：enq+deq 同拍（被消费），再一拍 enq 滞留
                        ev = true;
                        dr = (c % 2) == 0;
                    } else {
                        ev = true;
                        dr = false;
                    }
                } else {
                    ev = false;
                    dr = true;
                }
            }
        } else {
            const uint32_t pct = cosim::densityAt(c - directedEnd, cycles - directedEnd);
            ev = cosim::roll(rng, pct);
            dr = cosim::roll(rng, pct);
        }
        const uint32_t bits = rng();

        ref.enq_valid = ev;
        ref.enq_bits = bits;
        ref.deq_ready = dr;
        dut.enq.set({ev, bits});
        dut.deq_rdy.set(dr);
        rp.push("ev=" + std::to_string(ev) + " bits=0x" + [&] {
            char b[16];
            std::snprintf(b, sizeof b, "%08x", bits);
            return std::string(b);
        }() + " dr=" + std::to_string(dr));

        cosim::phaseLow(ref, dut);
        cosim::check(st, "queue", cfg, seed, c, "enq_ready", ref.enq_ready, dut.enq_rdy.get(), rp);
        cosim::check(st, "queue", cfg, seed, c, "deq_valid", ref.deq_valid, dut.deq.get().valid, rp);
        if (ref.deq_valid && dut.deq.get().valid)
            cosim::check(st, "queue", cfg, seed, c, "deq_bits", ref.deq_bits, dut.deq.get().bits, rp);
        cosim::check(st, "queue", cfg, seed, c, "count", ref.count, dut.count.get(), rp);
        cosim::phaseHigh(ref, dut);
    }
    if (st.mismatches > 0) {
        std::cout << "FAIL queue " << cfg << " seed=" << seed
                  << " mismatches=" << st.mismatches << "/" << st.checks << "\n";
        return st.mismatches;
    }
    std::cout << "PASS queue " << cfg << " seed=" << seed << " cycles=" << cycles
              << " checks=" << st.checks << "\n";
    return 0;
}

}  // namespace

int main() {
    uint64_t bad = 0;
    for (uint32_t seed : {11u, 22u, 33u}) {
        bad += cosimQueue<VQueueRef_e1_p1_f0, 1, false, true>("e1_p1_f0", seed, 120000);
        bad += cosimQueue<VQueueRef_e2_p0_f0, 2, false, false>("e2_p0_f0", seed, 120000);
        bad += cosimQueue<VQueueRef_e4_p0_f0, 4, false, false>("e4_p0_f0", seed, 120000);
        bad += cosimQueue<VQueueRef_e2_p0_f1, 2, true, false>("e2_p0_f1", seed, 120000);
        bad += cosimQueue<VQueueRef_e4_p1_f1, 4, true, true>("e4_p1_f1", seed, 120000);
    }
    std::cout << (bad ? "FAIL queue" : "ALL-PASS queue") << "\n";
    return bad ? 1 : 0;
}
