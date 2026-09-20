/*
Copyright 2022 The Photon Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#pragma once

#include <cstdint>
#include <type_traits>
#include <vector>
#include <photon/common/callback.h>
#include <photon/thread/thread.h>

namespace photon {

// VCPULocal<T> holds one lazily-constructed T per vCPU that ever asks for it,
// keyed by the VCPULocal instance. It is the coroutine-aware analogue of a
// thread_local member: each vCPU gets its own T, so a T never has to guard its
// own state against other vCPUs, yet -- unlike a raw thread_local -- every T is
// destroyed on the vCPU that built it when the VCPULocal instance goes away, or
// when that vCPU shuts down via photon::fini(), whichever comes first.
//
// This matters for a T whose resources are bound to a vCPU (pools, timers, the
// collector threads inside a socket pool): they must be created, used and torn
// down on that one vCPU. A plain thread_local cannot do the teardown, and a
// hand-rolled per-instance registry keeps reappearing; VCPULocal factors it out.
//
// Access model (see vcpu-local.cpp for the full argument):
//   * get() on the vCPU that already built its T is lock-free -- a hash lookup
//     and a ready flag, no atomics beyond what the lookup itself needs.
//   * The first get() on a vCPU builds the T under a per-slot mutex, so a sibling
//     coroutine that arrives mid-construction waits for that one T, not a lock
//     covering the whole table.
//   * ~VCPULocal (on any vCPU, or with no photon context at all) reaches every
//     vCPU that built a T and destroys it there.
//
// Constraints:
//   * get() must not race ~VCPULocal on the same instance -- destroying a thing
//     while another vCPU still uses it is a use-after-free regardless of us.
//   * A T built before fork() is abandoned in the child (never used, never
//     destroyed): its resources belong to the parent. The child lazily builds
//     fresh Ts on demand. Prefer exec() after fork(); this only keeps the child
//     from tripping over the parent's pre-fork state until it does.
class VCPULocalBase {
public:
    VCPULocalBase(const VCPULocalBase&) = delete;
    VCPULocalBase& operator=(const VCPULocalBase&) = delete;

protected:
    explicit VCPULocalBase(void (*destroyer)(void*)) : m_destroyer(destroyer) {}
    ~VCPULocalBase();

    // built by the derived VCPULocal<T>, which knows the type. Construction runs
    // on the current vCPU while the instance is alive; destruction may outlive
    // the instance (a vCPU's fini hook reaping a T after ~VCPULocal), so it goes
    // through the stamped destroyer below, never a virtual on a dead object.
    virtual void* create_value() = 0;         // on the current vCPU; null on failure

    void* get_or_create();   // the T for the current vCPU, building it if needed
    void* peek_current();    // the T for the current vCPU, or null if none yet
    void drain();            // destroy every T; call from the derived destructor

private:
    struct Slot;    // a (instance, vCPU) pair's T; defined in vcpu-local.cpp
    struct Table;   // a vCPU's set of slots; defined in vcpu-local.cpp
    struct DestroyCtx;
    // A back-reference kept per instance so ~VCPULocal can reach a slot's owning
    // vCPU and take its table lock without first dereferencing the slot (which a
    // shutting-down vCPU may already be freeing) -- table/vcpu/epoch are fixed at
    // creation, so the copy stays valid until we win the race for the slot.
    struct SlotRef { Slot* slot; Table* table; vcpu_base* vcpu; uint64_t epoch; };

    void (*m_destroyer)(void*);     // deletes a T*; stamped onto each slot
    photon::spinlock m_lock;        // guards m_refs; taken cross-vCPU at teardown
    std::vector<SlotRef> m_refs;    // one entry per vCPU that built a T for us
    bool m_drained = false;

    bool remove_ref(Slot* s);              // caller holds m_lock
    static Table& current_table();         // the current vCPU's slot table
    static void* destroy_entry(void* ctx); // thread entry, runs on the owning vCPU
    static void destroy_slot(Slot* s, vcpu_base* v);   // erase + destroy, on owning vCPU
};

template<typename T>
class VCPULocal : public VCPULocalBase {
public:
    VCPULocal() : VCPULocalBase(&destroy_impl) {}
    // the factory runs on the current vCPU to build this vCPU's T; returning
    // nullptr is not cached -- the next get() tries again.
    explicit VCPULocal(Delegate<T*> factory)
        : VCPULocalBase(&destroy_impl), m_factory(factory) {}
    ~VCPULocal() { drain(); }

    void set_factory(Delegate<T*> factory) { m_factory = factory; }

    // The T for the current vCPU, built on first use. Steady state is lock-free.
    T* get() { return (T*)get_or_create(); }
    // The T for the current vCPU, or nullptr if this vCPU has not built one.
    T* get_if() { return (T*)peek_current(); }

protected:
    // Build this vCPU's T: use the factory if one was given, otherwise default-
    // construct T. A default-construct fallback that would not compile (T has no
    // default ctor) resolves to nullptr instead, so VCPULocal<T> still compiles
    // for factory-only Ts. Returning nullptr is not cached; the next get() retries.
    void* create_value() override {
        if (m_factory) return (void*)m_factory();
        return default_construct(std::is_default_constructible<T>{});
    }

private:
    static void* default_construct(std::true_type)  { return (void*)new T(); }
    static void* default_construct(std::false_type) { return nullptr; }
    static void destroy_impl(void* p) { delete (T*)p; }
    Delegate<T*> m_factory;
};

}
