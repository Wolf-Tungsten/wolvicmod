#pragma once

#include <ostream>
#include <string>

#include "wolvicmod/core/module.h"
#include "wolvicmod/elab/graph.h"
#include "wolvicmod/sim/errors.h"

namespace wolvicmod::detail {

// Text round-level trace (§6.3): every round's edge hits, activated Updates,
// and committed state writes, for backtracing.
class TextTracer : public TraceSinkBase {
public:
    explicit TextTracer(std::ostream& os) : os_(os) {}

    void onRound(uint64_t evalCount, size_t round) override {
        os_ << "[eval " << evalCount << " round " << round << "]\n";
    }
    void onEdge(const std::string& desc) override { os_ << "  edge: " << desc << "\n"; }
    void onActivate(const Action* a) override { os_ << "  activate: " << a->describe() << "\n"; }
    void onCommit(const Entity* e) override {
        os_ << "  commit: " << e->hierPath();
        if (e->waveSupported()) {
            std::string bits;
            e->waveFormat(bits);
            os_ << " = 0b" << bits;
        }
        os_ << "\n";
    }

private:
    std::ostream& os_;
};

}  // namespace wolvicmod::detail

namespace wolvicmod {

// §6.3: start a round-level trace on an output stream (the stream must
// outlive the trace). Requires elaborate() first.
inline void Module::traceOn(std::ostream& os) {
    if (sim_ == nullptr) detail::fail("traceOn(): call elaborate() first ('" + hierPath() + "')");
    sim_->trace = std::make_unique<detail::TextTracer>(os);
    detail::traceDescFlag() = true;
    // edgeBit 快径在 trace 模式回退 prev 检测：写入点自此恢复 prev 维护
    // （entity.h watchTraceMode），这里把所有事件历史重新同步到当前值，
    // 消除快径期间 prev 停更留下的陈旧。
    detail::watchTraceMode() = true;
    for (auto& item : sim_->execOrder) {
        if (item.single != nullptr) {
            item.single->initEventPrev();
        } else {
            for (Action* a : sim_->groups[item.group]->actions) a->initEventPrev();
        }
    }
}

inline void Module::traceOff() {
    if (sim_ != nullptr) sim_->trace.reset();
    detail::traceDescFlag() = false;
    detail::watchTraceMode() = false;
}

}  // namespace wolvicmod
