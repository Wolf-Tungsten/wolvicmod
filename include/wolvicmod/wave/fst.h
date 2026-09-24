#pragma once

#ifdef WOLVICMOD_WITH_FST

#include <memory>
#include <string>
#include <vector>

#include "wolvicmod/core/module.h"
#include "wolvicmod/elab/graph.h"
#include "wolvicmod/sim/errors.h"

#include <fstapi.h>

namespace wolvicmod::detail {

// FST waveform dumper (§6.3): one var per formattable entity, scopes mirror
// the module tree, the time axis counts eval() calls.
class FstDumper : public WaveDumperBase {
public:
    FstDumper(Module* root, const std::string& file) {
        ctx_ = fstWriterCreate(file.c_str(), 1);
        if (ctx_ == nullptr) fail("waveOn(): cannot open '" + file + "' for writing");
        fstWriterSetPackType(ctx_, FST_WR_PT_LZ4);
        fstWriterSetTimescale(ctx_, -9);  // ns scale; 1 unit = 1 eval()
        fstWriterSetVersion(ctx_, "wolvicmod");
        walkModule(root);
    }

    ~FstDumper() override {
        if (ctx_ != nullptr) fstWriterClose(ctx_);
    }

    void dump(uint64_t time) override {
        if (!first_ && time < lastTime_)
            fail("waveDump(): time went backward (" + std::to_string(time) + " < " +
                 std::to_string(lastTime_) + "); FST time must be non-decreasing");
        if (first_ || time > lastTime_) fstWriterEmitTimeChange(ctx_, time);
        lastTime_ = time;
        for (Var& v : vars_) {
            std::string bits;
            v.entity->waveFormat(bits);
            if (first_ || bits != v.last) {
                fstWriterEmitValueChange(ctx_, v.handle, bits.c_str());
                v.last = std::move(bits);
            }
        }
        first_ = false;
    }

private:
    struct Var {
        Entity* entity;
        fstHandle handle;
        std::string last;
    };

    void walkModule(Module* m) {
        fstWriterSetScope(ctx_, FST_ST_VCD_MODULE, m->name().c_str(), nullptr);
        for (auto& ep : m->entities()) {
            Entity* e = ep.get();
            if (!e->waveSupported()) continue;
            fstHandle h = fstWriterCreateVar(ctx_, varTypeOf(e), varDirOf(e), e->waveBits(),
                                             e->name().c_str(), 0);
            vars_.push_back({e, h, {}});
        }
        for (auto& c : m->children()) walkModule(c.get());
        fstWriterSetUpscope(ctx_);
    }

    static fstVarType varTypeOf(const Entity* e) {
        return e->kind() == EntityKind::Reg ? FST_VT_VCD_REG : FST_VT_VCD_WIRE;
    }
    static fstVarDir varDirOf(const Entity* e) {
        switch (e->kind()) {
            case EntityKind::In: return FST_VD_INPUT;
            case EntityKind::Out: return FST_VD_OUTPUT;
            default: return FST_VD_IMPLICIT;
        }
    }

    fstWriterContext* ctx_ = nullptr;
    std::vector<Var> vars_;
    bool first_ = true;
    uint64_t lastTime_ = 0;
};

}  // namespace wolvicmod::detail

namespace wolvicmod {

// §6.3: open an FST waveform file. Sampling is explicit and Verilator-style:
// call waveDump(time) after each eval() (or whenever a sample is wanted) with
// a user-controlled, non-decreasing time. Mem entities are skipped (FST has
// no memory abstraction); types without an FstFormat specialization too.
inline void Module::waveOn(const std::string& filename) {
    if (sim_ == nullptr) detail::fail("waveOn(): call elaborate() first ('" + hierPath() + "')");
    if (sim_->wave != nullptr) detail::fail("waveOn(): a wave file is already open");
    sim_->wave = std::make_unique<detail::FstDumper>(this, filename);
}

// Sample every formattable signal and state at the given time; only changed
// values are emitted. Time must be non-decreasing across calls.
inline void Module::waveDump(uint64_t time) {
    if (sim_ == nullptr || sim_->wave == nullptr)
        detail::fail("waveDump(): call waveOn() first ('" + hierPath() + "')");
    sim_->wave->dump(time);
}

// Flush and close; safe when no dump is active.
inline void Module::waveOff() {
    if (sim_ != nullptr) sim_->wave.reset();
}

}  // namespace wolvicmod

#endif  // WOLVICMOD_WITH_FST
