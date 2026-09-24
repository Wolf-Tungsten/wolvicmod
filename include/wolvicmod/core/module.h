#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "wolvicmod/core/entity.h"
#include "wolvicmod/core/mem.h"
#include "wolvicmod/core/reg.h"
#include "wolvicmod/core/signal.h"
#include "wolvicmod/sim/errors.h"

namespace wolvicmod {

class Action;

namespace detail {
struct SimState;
// Stack of modules under construction. The Module base constructor pushes;
// createChildModule pops after the child is built, so the top is always the
// module whose constructor body is running.
inline std::vector<Module*>& ctxStack() {
    static thread_local std::vector<Module*> s;
    return s;
}
}  // namespace detail

// Tree node of the hierarchy (§2.2): holds signals, state, actions and child
// modules. Modeling happens entirely in the constructor (§4.1). Modules are
// non-copyable (§6.1).
class Module {
public:
    Module() { detail::ctxStack().push_back(this); }
    virtual ~Module();  // defined in elab/elaborate.h
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    // Creation methods: construct the entity, register its name, and return a
    // reference to bind as a member (§6.1).
    template <class T>
    In<T>& createIn(std::string_view name) { return addEntity<In<T>>(name); }
    template <class T>
    Out<T>& createOut(std::string_view name) { return addEntity<Out<T>>(name); }
    template <class T>
    Wire<T>& createWire(std::string_view name) { return addEntity<Wire<T>>(name); }
    template <class T>
    Reg<T>& createReg(std::string_view name) { return addEntity<Reg<T>>(name); }
    template <class T, size_t R>
    Mem<T, R>& createMem(std::string_view name) { return addEntity<Mem<T, R>>(name); }

    template <class T>
    T& createChildModule(std::string_view name) {
        static_assert(std::is_base_of_v<Module, T>, "createChildModule<T>: T must derive from Module");
        registerName(name);
        auto child = std::make_unique<T>();
        detail::ctxStack().pop_back();  // balance the push in Module::Module()
        T& ref = *child;
        ref.parent_ = this;
        ref.name_ = name;
        children_.push_back(std::move(child));
        return ref;
    }

    const std::string& name() const { return name_; }
    Module* parent() const { return parent_; }

    std::string hierPath() const {
        if (parent_ == nullptr) return name_;
        std::string base = parent_->hierPath();
        return base.empty() ? name_ : base + "." + name_;
    }

    bool elaborated() const { return sim_ != nullptr; }

    // Elaboration & simulation entry points (§3.5), defined in elab/ and sim/.
    void elaborate(std::string_view topName = "top");
    void eval();

#ifdef WOLVICMOD_WITH_FST
    // Waveform dump (§6.3), defined in wave/fst.h — Verilator-style: waveOn()
    // once, then waveDump(time) after each eval() with a user-controlled time.
    void waveOn(const std::string& filename);
    void waveOff();
    void waveDump(uint64_t time);
#endif

    // Round-level trace (§6.3), defined in dbg/trace.h: each round's edge
    // hits, activated Updates, and committed state writes.
    void traceOn(std::ostream& os);
    void traceOff();

    // Debug switches (§6.4), defined in dbg/audit.h.
    void auditOn();   // read-set accounting (needs a WOLVICMOD_AUDIT build)
    void auditOff();
    void assertUpdateMutexOn();   // report simultaneously-active Updates
    void assertUpdateMutexOff();

    // --- internal (framework use) ---
    const std::vector<std::unique_ptr<Entity>>& entities() const { return entities_; }
    const std::vector<std::unique_ptr<Module>>& children() const { return children_; }
    const std::vector<std::unique_ptr<Action>>& actions() const { return actions_; }
    void addAction(std::unique_ptr<Action> a);  // defined in core/action.h

    void registerName(std::string_view name) {
        if (!names_.emplace(name).second)
            detail::fail("duplicate member name '" + std::string(name) + "' in module " +
                         (name_.empty() ? std::string("<root>") : hierPath()));
    }

    detail::SimState* sim() const { return sim_.get(); }
    void resetSim(std::unique_ptr<detail::SimState> s) { sim_ = std::move(s); }
    void setRootName(std::string_view n) { name_ = n; }

private:
    template <class E>
    E& addEntity(std::string_view name) {
        registerName(name);
        auto e = std::make_unique<E>();
        E& ref = *e;
        ref.attach(this, name);
        entities_.push_back(std::move(e));
        return ref;
    }

    Module* parent_ = nullptr;
    std::string name_;
    std::vector<std::unique_ptr<Entity>> entities_;
    std::vector<std::unique_ptr<Module>> children_;
    std::vector<std::unique_ptr<Action>> actions_;
    std::unordered_set<std::string> names_;
    std::unique_ptr<detail::SimState> sim_;
};

// Defined here: needs Module complete.
inline void Entity::attach(Module* owner, std::string_view name) {
    owner_ = owner;
    name_ = name;
}

inline std::string Entity::hierPath() const {
    if (owner_ == nullptr) return "<unnamed>";
    std::string base = owner_->hierPath();
    return base.empty() ? name_ : base + "." + name_;
}

// External drive: root-module inputs only, after elaboration (§3.5, §4.2).
template <class T>
void In<T>::set(const T& v) {
    Module* o = this->owner();
    if (o == nullptr || o->parent() != nullptr)
        detail::fail("set(): only root-module inputs are driven externally ('" + this->hierPath() + "')");
    if (!o->elaborated())
        detail::fail("set(): call elaborate() before driving inputs ('" + this->hierPath() + "')");
    this->mutableValue() = v;
}

}  // namespace wolvicmod

// One-line member declarations (§6.1). When T contains commas (e.g.
// std::array<T, N>), alias it first.
#define IN(T, name)     ::wolvicmod::In<T>&      name = createIn<T>(#name)
#define OUT(T, name)    ::wolvicmod::Out<T>&     name = createOut<T>(#name)
#define WIRE(T, name)   ::wolvicmod::Wire<T>&    name = createWire<T>(#name)
#define REG(T, name)    ::wolvicmod::Reg<T>&     name = createReg<T>(#name)
#define MEM(T, R, name) ::wolvicmod::Mem<T, R>&  name = createMem<T, R>(#name)
#define SUB(T, name)    T&                       name = createChildModule<T>(#name)
