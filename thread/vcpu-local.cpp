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

#include "vcpu-local.h"

#include <atomic>
#include <sched.h>
#include <unordered_map>
#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/intrusive_list.h>

namespace photon {

// One (instance, vCPU) pair's T. Lives on the heap so that a cross-vCPU
// ~VCPULocal can hold a stable pointer to it while the owning vCPU rehashes its
// table; whoever wins the race for it (the instance's ~ or the vCPU's fini hook)
// frees it. Linked into its owning vCPU's `live` list for that arbitration.
struct VCPULocalBase::Slot : intrusive_list_node<Slot> {
    photon::mutex ctor_mtx;         // serialises construction; siblings wait here
    void* ptr = nullptr;            // the T; read lock-free by the owning vCPU
    bool ready = false;             // ptr fully built; only touched on owning vCPU
    std::atomic<bool> disowned{false}; // ~VCPULocal gave up; owning vCPU reaps
    vcpu_base* vcpu = nullptr;      // the owning vCPU
    VCPULocalBase* key = nullptr;   // the instance (map key); never deref if disowned
    Table* table = nullptr;         // the owning vCPU's table
    void (*destroy)(void*, void*) = nullptr;
    std::shared_ptr<void> destroyer_state;
};

// One vCPU's set of slots. `map` is the O(1) lookup for get() and is touched
// only by its own vCPU -- lock-free reads, structural changes on create/destroy
// -- so no other vCPU ever races its rehash. `live` mirrors it for teardown and
// is the arbiter between ~VCPULocal and the fini hook, so it is guarded.
struct VCPULocalBase::Table {
    photon::spinlock lock;                              // guards live + destroying
    std::unordered_map<VCPULocalBase*, Slot*> map;      // owning-vCPU-only structure
    intrusive_list<Slot, false> live;
    // slots claimed by a cross-vCPU ~VCPULocal but not yet destroyed. The fini
    // hook must not let fini() reach vcpu_fini() -- which frees the vcpu_t the
    // pending thread_migrate still writes to -- until every handoff has landed.
    int destroying = 0;
    bool hook = false;

    bool contains_locked(Slot* s) {
        for (auto n : live)
            if (n == s) return true;
        return false;
    }
    void ensure_hook() {
        if (!hook) {
            hook = true;
            photon::fini_hook({this, &Table::at_fini});
        }
    }
    void at_fini();
};

VCPULocalBase::Table& VCPULocalBase::current_table() {
    static thread_local Table t;
    return t;
}

VCPULocalBase::VCPULocalBase(void (*destroyer)(void*, void*),
                           std::shared_ptr<void> destroyerState)
    : m_destroyer(destroyer), m_destroyer_state(std::move(destroyerState)) {
}

bool VCPULocalBase::remove_ref(Slot* s) {
    for (auto it = m_refs.begin(); it != m_refs.end(); ++it)
        if (it->slot == s) {
            m_refs.erase(it);
            return true;
        }
    return false;
}

void* VCPULocalBase::get_or_create() {
    auto& t = current_table();
    Slot* retired = nullptr;

    auto it = t.map.find(this);
    if (it != t.map.end()) {
        Slot* s = it->second;
        if (!s->disowned.load(std::memory_order_acquire)) {
            if (s->ready) return s->ptr;   // steady state: lock-free
            // a sibling coroutine on this vCPU is building it (or a build failed
            // and left it empty); wait on just this slot, then take/retry it
            SCOPED_LOCK(s->ctor_mtx);
            if (!s->ready) { s->ptr = create_value(); s->ready = (s->ptr != nullptr); }
            return s->ptr;
        }
        t.map.erase(it);
        // Detach the previous instance's disowned slot. Its destroyer may
        // yield, so defer reclamation until the replacement is published and
        // its construction mutex is held.
        {
            SCOPED_LOCK(t.lock);
            if (t.contains_locked(s)) t.live.erase(s);
        }
        if (s->vcpu == photon::get_vcpu()) retired = s;
        // Otherwise its original vCPU has already gone away. Leak the old
        // value rather than destroy a vCPU-bound object in the new runtime.
    }

    // Publish an empty slot before building, so a sibling that arrives during
    // construction finds it (not ready) and waits, instead of building a second
    // T. None of the steps below yields, so no sibling runs until create_value.
    Slot* s = new Slot;
    s->vcpu = photon::get_vcpu();
    s->key = this;
    s->table = &t;
    s->destroy = m_destroyer;
    s->destroyer_state = m_destroyer_state;
    t.map[this] = s;
    {
        SCOPED_LOCK(t.lock);
        t.live.push_back(s);
        t.ensure_hook();
    }
    {
        SCOPED_LOCK(m_lock);
        m_refs.push_back({s, &t, s->vcpu});
    }
    SCOPED_LOCK(s->ctor_mtx);
    if (retired) {
        retired->destroy(retired->ptr, retired->destroyer_state.get());
        delete retired;
    }
    s->ptr = create_value();
    s->ready = (s->ptr != nullptr);
    return s->ptr;
}

void* VCPULocalBase::peek_current() {
    auto& t = current_table();
    auto it = t.map.find(this);
    if (it == t.map.end()) return nullptr;
    Slot* s = it->second;
    if (s->ready &&
        !s->disowned.load(std::memory_order_acquire))
        return s->ptr;
    return nullptr;
}

// Built-in Ts must be destroyed on their own vCPU (pools, timers, collector
// threads live there). Never migrate CURRENT for this: after landing on another
// OS thread, reads of photon::CURRENT may hit the stale TLS slot the compiler
// cached. Send a helper thread over instead, and wait for it.
struct VCPULocalBase::DestroyCtx {
    Slot* s;
    photon::semaphore done{0};
    void wait() {
        // Even an infinite semaphore wait returns on ETIMEDOUT/ESHUTDOWN
        // interrupts. The helper must finish before this stack context and
        // the owner's handoff pin can be released.
        while (done.wait(1) < 0) {}
    }
};
void* VCPULocalBase::destroy_entry(void* arg) {
    auto c = (DestroyCtx*)arg;
    auto s = c->s;
    if (s) {
        s->table->map.erase(s->key); // structural changes stay on the owning vCPU
        s->destroy(s->ptr, s->destroyer_state.get());
        delete s;
    }
    c->done.signal(1);
    return nullptr;
}
void VCPULocalBase::destroy_slot(Slot* s, vcpu_base* v) {
    if (v == photon::get_vcpu()) {
        s->table->map.erase(s->key);
        s->destroy(s->ptr, s->destroyer_state.get());
        delete s;
        return;
    }
    auto defer_cleanup = [s] {
        s->disowned.store(true, std::memory_order_release);
        SCOPED_LOCK(s->table->lock);
        s->table->live.push_back(s);
    };
    DestroyCtx ctx{s};
    auto th = photon::thread_create(&destroy_entry, &ctx);
    if (!th) {
        defer_cleanup();
        LOG_ERROR("failed to create the vCPU-local destroy helper, deferring cleanup");
        return;
    }
    if (photon::thread_migrate(th, v) < 0) {
        // Failed migration leaves the helper runnable here. Let it finish
        // without touching the owner's map/value before releasing its context.
        ctx.s = nullptr;
        ctx.wait();
        defer_cleanup();
        LOG_WARN("failed to migrate the vCPU-local destroy helper, deferring cleanup");
        return;
    }
    ctx.wait();
}

void VCPULocalBase::drain() {
    if (m_drained) return;
    m_drained = true;
    while (true) {
        SlotRef ref;
        Slot* s;
        bool claimed = false;
        bool disowned = false;
        {
            // Keep the back-reference pinned until its slot has been arbitrated
            // under the table lock. at_fini() drops the table lock before taking
            // this one, so the nesting here cannot form a lock-order cycle.
            SCOPED_LOCK(m_lock);
            if (m_refs.empty()) break;
            ref = m_refs.back();
            s = ref.slot;

            SCOPED_LOCK(ref.table->lock);
            if (ref.table->contains_locked(s)) {
                if (!photon::CURRENT) {
                    // There is no Photon context here to destroy on the owning
                    // vCPU; let that vCPU's fini hook reap this disowned slot.
                    s->disowned.store(true, std::memory_order_release);
                    disowned = true;
                } else {
                    ref.table->live.erase(s);
                    // Pin the owning vCPU's fini until the destroy handoff lands.
                    ref.table->destroying++;
                    claimed = true;
                }
                remove_ref(s);
            }
        }

        if (!photon::CURRENT) {
            if (!disowned)
                ::sched_yield();   // the fini hook is dropping our backref
            continue;
        }
        if (!claimed) {
            // the fini hook of its vCPU claimed it and will drop our backref
            photon::thread_yield();
            continue;
        }
        destroy_slot(s, ref.vcpu);
        { SCOPED_LOCK(ref.table->lock); ref.table->destroying--; }
    }
}

VCPULocalBase::~VCPULocalBase() {
    // the derived VCPULocal<T> must have drained already: destroy_value is gone
    // by now, and any surviving slot would dangle back to this instance
    assert(m_drained && m_refs.empty());
}

void VCPULocalBase::Table::at_fini() {
    // reap every T this vCPU still owns, here, where they belong
    for (;;) {
        lock.lock();
        Slot* s = live.pop_front();
        bool busy = destroying > 0;
        lock.unlock();
        if (!s) {
            // A failed helper can return its claimed slot to live before the
            // handoff unpins this vCPU. Recheck both under the same lock rather
            // than finishing the list pass and then only waiting on the pin.
            if (!busy) break;
            photon::thread_usleep(1000);
            continue;
        }
        if (!s->disowned.load(std::memory_order_acquire)) {
            // the instance is still alive (its ~ blocks until we drop this
            // backref, so key is safe to touch); keep its m_refs consistent
            SCOPED_LOCK(s->key->m_lock);
            s->key->remove_ref(s);
        }
        map.erase(s->key);
        s->destroy(s->ptr, s->destroyer_state.get());
        delete s;
    }
    // The list is empty and every claimed handoff has landed (or returned its
    // slot for reclamation above), so no helper can access this table afterward.
    map.clear();
    hook = false;   // photon::fini() clears the hook vector; re-arm on next init
}

}
