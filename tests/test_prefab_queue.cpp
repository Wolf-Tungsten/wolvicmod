// prefab Queue<T, N, Flow, Pipe> 单测：chisel3.util.Queue 拍级对齐。
// 覆盖：灌满→排空全占位数扫描；同拍 enq+deq；Flow 空直通；Pipe 满时 deq_rdy
// 放行；长程随机反压下的数据保序性（push 序列 == pop 序列）。

#include <deque>
#include <random>

#include <doctest/doctest.h>
#include <wolvicmod/wolvicmod.h>
#include <wolvicmod/prefab/queue.h>

#include "test_prefab_common.h"

using namespace wolvicmod;
using namespace wolvicmod::prefab;
using namespace prefabtest;

namespace {

using Q4 = Queue<uint32_t, 4>;

TEST_CASE("prefab Queue: 灌满→排空，占位数逐拍扫描") {
    Q4 top;
    top.elaborate();
    top.enq.set({false, 0});
    top.deq_rdy.set(false);

    // cycle 0：空
    comb(top);
    CHECK(top.count.get() == 0);
    CHECK(top.enq_rdy.get() == true);
    CHECK(top.deq.get().valid == false);
    edge(top);

    // 灌入 10,11,12,13（4 拍灌满）
    for (uint32_t i = 0; i < 4; ++i) {
        top.enq.set({true, 10 + i});
        comb(top);
        CHECK(top.count.get() == i);            // 本拍占位数 = 已灌入 i 项
        CHECK(top.enq_rdy.get() == true);       // 未满（第 4 项在灌入拍仍可写）
        if (i > 0) {
            CHECK(top.deq.get().valid == true); // 最旧项 10 在队首
            CHECK(top.deq.get().bits == 10);
        }
        edge(top);
    }
    // cycle 4：满
    comb(top);
    CHECK(top.count.get() == 4);
    CHECK(top.enq_rdy.get() == false);          // 满 → enq_rdy 拉低
    CHECK(top.deq.get().valid == true);
    CHECK(top.deq.get().bits == 10);
    edge(top);

    // 满时 enq 被阻塞（valid 顶着也不入队），同拍开始排空
    top.enq.set({true, 99});
    top.deq_rdy.set(true);
    comb(top);
    CHECK(top.enq_rdy.get() == false);
    CHECK(top.deq.get().bits == 10);
    edge(top);
    // cycle 5：10 被弹走，99 没进来，count 4→3；撤销 enq 继续排空
    top.enq.set({false, 0});
    comb(top);
    CHECK(top.count.get() == 3);
    CHECK(top.deq.get().bits == 11);
    edge(top);

    // 排空扫描：12, 13
    for (uint32_t expect : {12u, 13u}) {
        comb(top);
        CHECK(top.deq.get().valid == true);
        CHECK(top.deq.get().bits == expect);
        edge(top);
    }
    // 空
    comb(top);
    CHECK(top.count.get() == 0);
    CHECK(top.deq.get().valid == false);
    CHECK(top.enq_rdy.get() == true);
    edge(top);
}

TEST_CASE("prefab Queue: 每个占位数下同拍 enq+deq，count 按规则变化且保序") {
    Q4 top;
    top.elaborate();
    top.deq_rdy.set(false);
    top.enq.set({false, 0});
    comb(top);
    edge(top);

    uint32_t nextPush = 100;
    std::deque<uint32_t> model;
    auto pump = [&](uint32_t target) {
        while (model.size() < target) {
            top.enq.set({true, nextPush});
            comb(top);
            edge(top);
            model.push_back(nextPush++);
        }
        top.enq.set({false, 0});
    };
    // 逐档占位数 k：同拍 enq+deq 两拍；非 pipe 队列满时 enq 被阻塞
    for (uint32_t k = 1; k <= 4; ++k) {
        pump(k);
        for (int rep = 0; rep < 2; ++rep) {
            const bool enqWouldFire = model.size() < 4;  // 非 pipe：不满才可入
            top.enq.set({true, nextPush});
            top.deq_rdy.set(true);
            comb(top);
            CHECK(top.count.get() == model.size());
            CHECK(top.enq_rdy.get() == enqWouldFire);
            CHECK(top.deq.get().valid == true);
            CHECK(top.deq.get().bits == model.front());
            edge(top);
            model.pop_front();
            if (enqWouldFire) model.push_back(nextPush++);
        }
        top.deq_rdy.set(false);
        top.enq.set({false, 0});
        comb(top);
        CHECK(top.count.get() == model.size());
        edge(top);
    }
    // 排空校验保序
    top.deq_rdy.set(true);
    while (!model.empty()) {
        comb(top);
        CHECK(top.deq.get().valid == true);
        CHECK(top.deq.get().bits == model.front());
        edge(top);
        model.pop_front();
    }
    comb(top);
    CHECK(top.deq.get().valid == false);
    CHECK(top.count.get() == 0);
    edge(top);
}

TEST_CASE("prefab Queue: 随机反压长程保序（模型对拍 300 拍）") {
    Q4 top;
    top.elaborate();
    std::mt19937 rng(20260924);
    std::deque<uint32_t> model;
    uint32_t pushed = 0, popped = 0;
    top.enq.set({false, 0});
    top.deq_rdy.set(false);
    comb(top);
    edge(top);
    for (int c = 0; c < 300; ++c) {
        const bool ev = (rng() & 3u) != 0;   // 75% valid
        const bool dr = (rng() & 1u) != 0;   // 50% ready
        top.enq.set({ev, pushed});
        top.deq_rdy.set(dr);
        comb(top);
        // 组合输出对拍
        CHECK(top.count.get() == model.size());
        CHECK(top.enq_rdy.get() == (model.size() < 4));
        CHECK(top.deq.get().valid == !model.empty());
        if (!model.empty()) CHECK(top.deq.get().bits == model.front());
        const bool doEnq = ev && model.size() < 4;
        const bool doDeq = dr && !model.empty();
        edge(top);
        if (doDeq) {
            CHECK(model.front() == popped);
            model.pop_front();
            ++popped;
        }
        if (doEnq) model.push_back(pushed++);
    }
    CHECK(popped > 50);   // 确实发生了大量传输
    CHECK(pushed > popped);
}

using Q4Flow = Queue<uint32_t, 4, true, false>;

TEST_CASE("prefab Queue(flow): 空直通被消费的那拍不入队") {
    Q4Flow top;
    top.elaborate();
    top.enq.set({false, 0});
    top.deq_rdy.set(false);
    comb(top);
    edge(top);

    // cycle 0：空 + enq.valid + deq_rdy → deq 同拍直通，队列内部不动
    top.enq.set({true, 77});
    top.deq_rdy.set(true);
    comb(top);
    CHECK(top.deq.get().valid == true);   // 直通
    CHECK(top.deq.get().bits == 77);      // bits 来自 enq 而非 ram
    CHECK(top.count.get() == 0);
    edge(top);
    // cycle 1：直通被消费后队列仍为空（无幻影拷贝：下一拍 deq 不再有效）
    top.enq.set({false, 0});
    comb(top);
    CHECK(top.deq.get().valid == false);
    CHECK(top.count.get() == 0);
    edge(top);

    // cycle 2：空 + enq.valid 但 deq_rdy=0 → 直通展示但不消费，数据写入 ram
    top.enq.set({true, 88});
    top.deq_rdy.set(false);
    comb(top);
    CHECK(top.deq.get().valid == true);   // flow 仍拉高 valid
    CHECK(top.deq.get().bits == 88);
    edge(top);
    // cycle 3：count=1，deq 来自 ram
    top.enq.set({false, 0});
    comb(top);
    CHECK(top.count.get() == 1);
    CHECK(top.deq.get().valid == true);
    CHECK(top.deq.get().bits == 88);
    edge(top);

    // cycle 3 再灌一项（count 1→2），确认直通后队列状态正常
    top.enq.set({true, 89});
    comb(top);
    CHECK(top.count.get() == 1);
    edge(top);
    top.deq_rdy.set(true);
    top.enq.set({false, 0});
    comb(top);
    CHECK(top.count.get() == 2);
    CHECK(top.deq.get().bits == 88);
    edge(top);
    comb(top);
    CHECK(top.deq.get().bits == 89);
    edge(top);
    comb(top);
    CHECK(top.count.get() == 0);
    CHECK(top.deq.get().valid == false);
    edge(top);
}

TEST_CASE("prefab Queue(flow): 连续两拍直通 + 随机对拍") {
    Q4Flow top;
    top.elaborate();
    std::mt19937 rng(7);
    std::deque<uint32_t> model;
    uint32_t pushed = 0, popped = 0;
    top.enq.set({false, 0});
    top.deq_rdy.set(false);
    comb(top);
    edge(top);
    for (int c = 0; c < 300; ++c) {
        const bool ev = (rng() & 3u) != 0;
        const bool dr = (rng() & 1u) != 0;
        top.enq.set({ev, pushed});
        top.deq_rdy.set(dr);
        comb(top);
        const bool empty = model.empty();
        // deq：flow 直通
        CHECK(top.deq.get().valid == (!empty || ev));
        if (!empty) CHECK(top.deq.get().bits == model.front());
        else if (ev) CHECK(top.deq.get().bits == pushed);
        CHECK(top.enq_rdy.get() == (model.size() < 4));
        CHECK(top.count.get() == model.size());
        // 状态推进（chisel flow：空直通被消费的那拍队列不动）
        const bool doEnq = ev && model.size() < 4 && !(empty && dr);
        const bool doDeq = (!empty || ev) && dr && !empty;
        const bool bypassFire = empty && ev && dr;
        edge(top);
        if (doDeq) {
            CHECK(model.front() == popped);
            model.pop_front();
            ++popped;
        }
        if (bypassFire) { ++popped; ++pushed; }  // 直通交付：数据不入队
        if (doEnq) model.push_back(pushed++);
    }
    CHECK(popped > 50);
}

using Q2Pipe = Queue<uint32_t, 2, false, true>;

TEST_CASE("prefab Queue<1>: 深度 1 的灌满/排空") {
    Queue<uint32_t, 1> top;
    top.elaborate();
    top.enq.set({false, 0});
    top.deq_rdy.set(false);
    comb(top);
    CHECK(top.count.get() == 0);
    CHECK(top.enq_rdy.get() == true);
    CHECK(top.deq.get().valid == false);
    edge(top);

    top.enq.set({true, 42});
    comb(top);
    edge(top);
    top.enq.set({true, 43});
    comb(top);
    CHECK(top.count.get() == 1);
    CHECK(top.enq_rdy.get() == false);   // 满
    CHECK(top.deq.get().valid == true);
    CHECK(top.deq.get().bits == 42);
    edge(top);                           // 43 被阻塞
    top.enq.set({false, 0});
    top.deq_rdy.set(true);
    comb(top);
    CHECK(top.enq_rdy.get() == false);   // 非 pipe：满时 deq_rdy 不放行
    edge(top);                           // 弹出 42
    comb(top);
    CHECK(top.count.get() == 0);
    CHECK(top.deq.get().valid == false);
    CHECK(top.enq_rdy.get() == true);
    edge(top);
}

using Q2Pipe = Queue<uint32_t, 2, false, true>;

TEST_CASE("prefab Queue(pipe): 满时 deq_rdy 放行 enq（无气泡）") {
    Q2Pipe top;
    top.elaborate();
    top.enq.set({false, 0});
    top.deq_rdy.set(false);
    comb(top);
    edge(top);

    // 灌满 [10, 11]
    top.enq.set({true, 10});
    comb(top);
    CHECK(top.enq_rdy.get() == true);
    edge(top);
    top.enq.set({true, 11});
    comb(top);
    CHECK(top.count.get() == 1);
    edge(top);
    // cycle 2：满，deq_rdy=0 → enq_rdy 拉低
    comb(top);
    CHECK(top.count.get() == 2);
    CHECK(top.enq_rdy.get() == false);
    CHECK(top.deq.get().valid == true);
    CHECK(top.deq.get().bits == 10);
    edge(top);
    // cycle 3：满 + deq_rdy → enq_rdy 同拍放行；同拍读写同地址（见旧值）
    top.enq.set({true, 12});
    top.deq_rdy.set(true);
    comb(top);
    CHECK(top.enq_rdy.get() == true);     // pipe：deq_rdy 放行
    CHECK(top.deq.get().bits == 10);      // 读侧见旧值（本拍弹出 10）
    edge(top);
    // cycle 4：仍满（出一进一），内容为 [11, 12]
    top.enq.set({false, 0});
    comb(top);
    CHECK(top.count.get() == 2);
    CHECK(top.deq.get().bits == 11);
    edge(top);                                // deq_rdy=1：本拍弹出 11
    // 排空校验 12
    comb(top);
    CHECK(top.count.get() == 1);
    CHECK(top.deq.get().bits == 12);
    edge(top);                                // 弹出 12
    comb(top);
    CHECK(top.count.get() == 0);
    CHECK(top.deq.get().valid == false);
    CHECK(top.enq_rdy.get() == true);
    edge(top);
}

}  // namespace
