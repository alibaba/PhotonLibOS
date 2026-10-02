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

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <new>
#include <set>
#include <thread>

#include <photon/common/alog.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/thread/workerpool.h>
#include <photon/thread/vcpu-local.h>

#include "../../test/gtest.h"

using namespace photon;

namespace {

std::atomic<int> g_ctor{0};
std::atomic<int> g_dtor{0};

// Records the vCPU it was built on, so a test can assert each T is created --
// and later destroyed -- on its owning vCPU.
struct Value {
    vcpu_base* built_on;
    Value() : built_on(photon::get_vcpu()) { g_ctor.fetch_add(1, std::memory_order_relaxed); }
    ~Value() { g_dtor.fetch_add(1, std::memory_order_relaxed); }
};

void reset() {
    g_ctor.store(0, std::memory_order_relaxed);
    g_dtor.store(0, std::memory_order_relaxed);
}

// Fan a Value build out to every worker of `pool`, one per vCPU: each task
// blocks until all N have arrived, so no worker can serve two tasks and every
// vCPU ends up with its own slot. Collects the distinct Value* built.
void build_one_per_vcpu(WorkPool& pool, int n, VCPULocal<Value>& local,
                        std::set<Value*>& ptrs) {
    photon::mutex mtx;
    photon::semaphore arrived(0), release(0);
    std::atomic<int> done{0};
    for (int i = 0; i < n; ++i) {
        pool.async_call(new auto([&] {
            Value* v = local.get();
            EXPECT_EQ(v, local.get());                  // same T reused on this vCPU
            EXPECT_EQ(photon::get_vcpu(), v->built_on);  // built here
            { SCOPED_LOCK(mtx); ptrs.insert(v); }
            arrived.signal(1);
            release.wait(1);                             // hold the vCPU: force fan-out
            done.fetch_add(1, std::memory_order_release);
        }));
    }
    ASSERT_EQ(0, arrived.wait(n, 5ULL * 1000 * 1000));
    release.signal(n);
    while (done.load(std::memory_order_acquire) < n)
        photon::thread_yield();
}

}  // namespace

// The current vCPU reuses its own T across get(); get_if() sees it only after
// it is built; ~VCPULocal destroys it on the same vCPU.
TEST(vcpu_local, same_vcpu_reuse) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    DEFER(photon::fini());
    reset();
    {
        VCPULocal<Value> local;
        EXPECT_EQ(nullptr, local.get_if());          // nothing built yet
        Value* a = local.get();
        ASSERT_NE(nullptr, a);
        EXPECT_EQ(a, local.get());                   // reuse, lock-free
        EXPECT_EQ(a, local.get_if());                // now cached
        EXPECT_EQ(1, g_ctor.load());
        EXPECT_EQ(photon::get_vcpu(), a->built_on);
    }
    EXPECT_EQ(1, g_dtor.load());                     // drained by ~VCPULocal
}

// Each vCPU gets its own T; destroying the instance while its vCPUs are still
// alive reaches every one of them and destroys each T on its owning vCPU.
TEST(vcpu_local, per_vcpu_distinct_then_instance_destroy) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    DEFER(photon::fini());
    reset();
    constexpr int N = 4;
    WorkPool pool(N, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE, -1);
    {
        VCPULocal<Value> local;
        std::set<Value*> ptrs;
        build_one_per_vcpu(pool, N, local, ptrs);
        EXPECT_EQ(N, (int)ptrs.size());              // one distinct T per vCPU
        EXPECT_EQ(N, g_ctor.load());
        EXPECT_EQ(0, g_dtor.load());
        // ~local next, on the main vCPU while the workers are idle-but-alive:
        // the claim path migrates a helper to each worker vCPU to destroy its T
    }
    EXPECT_EQ(N, g_dtor.load());                     // all reaped, pool still up
}

// A vCPU shutting down (WorkPool teardown -> photon::fini per worker) reaps its
// own T through the fini hook, before the instance is destroyed.
TEST(vcpu_local, reaped_by_vcpu_fini) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    DEFER(photon::fini());
    reset();
    constexpr int N = 4;
    VCPULocal<Value> local;
    {
        WorkPool pool(N, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE, -1);
        std::set<Value*> ptrs;
        build_one_per_vcpu(pool, N, local, ptrs);
        EXPECT_EQ(N, (int)ptrs.size());
        EXPECT_EQ(N, g_ctor.load());
        EXPECT_EQ(0, g_dtor.load());
        // pool destroyed at end of scope: each worker's fini hook reaps its T
    }
    EXPECT_EQ(N, g_dtor.load());                     // reaped by the vCPUs' fini
    // `local` still alive with an empty ref set; its ~ below must be a no-op
}

TEST(vcpu_local, instance_destroy_races_vcpu_fini) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    DEFER(photon::fini());
    constexpr int ROUNDS = 32;
    for (int i = 0; i < ROUNDS; ++i) {
        reset();
        auto pool = std::make_unique<WorkPool>(
            1, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE, -1);
        struct Local final : VCPULocal<Value> {};
        auto local = new Local;
        pool->call([&] { ASSERT_NE(nullptr, local->get()); });

        std::atomic<bool> start{false};
        std::thread destroyer([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            delete local;
        });
        start.store(true, std::memory_order_release);
        pool.reset();
        destroyer.join();
        EXPECT_EQ(1, g_dtor.load());
    }
}

// A factory is honored, and a factory that returns nullptr is not cached: the
// next get() tries again.
TEST(vcpu_local, factory_and_null_not_cached) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    DEFER(photon::fini());
    reset();
    bool succeed = false;
    VCPULocal<Value> local(Delegate<Value*>((void*)&succeed, [](void* p) -> Value* {
        return *(bool*)p ? new Value() : nullptr;
    }));
    EXPECT_EQ(nullptr, local.get());                 // factory declined
    EXPECT_EQ(0, g_ctor.load());
    succeed = true;
    Value* a = local.get();                          // retried, now built
    ASSERT_NE(nullptr, a);
    EXPECT_EQ(a, local.get());
    EXPECT_EQ(1, g_ctor.load());
}

TEST(vcpu_local, address_reuse_after_disown) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    DEFER(photon::fini());
    reset();
    WorkPool pool(1, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE, -1);

    struct Local final : VCPULocal<Value> {};
    alignas(Local) unsigned char storage[sizeof(Local)];
    auto first = new (storage) Local;
    pool.call([&] { ASSERT_NE(nullptr, first->get()); });
    EXPECT_EQ(1, g_ctor.load());

    // Without a photon context, destruction disowns the worker's slot. Reusing
    // the same object address must not make the new instance observe that slot.
    std::thread destroyer([&] { first->~Local(); });
    destroyer.join();
    EXPECT_EQ(0, g_dtor.load());

    auto second = new (storage) Local;
    pool.call([&] { ASSERT_NE(nullptr, second->get()); });
    EXPECT_EQ(2, g_ctor.load());
    EXPECT_EQ(1, g_dtor.load());
    second->~Local();
    EXPECT_EQ(2, g_dtor.load());
}

TEST(vcpu_local, custom_deleter_survives_native_instance_destruction) {
    ASSERT_EQ(0, photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE));
    DEFER(photon::fini());
    reset();
    auto pool = std::make_unique<WorkPool>(
        1, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE, -1);
    auto state = std::make_shared<int>(42);
    std::weak_ptr<int> retained = state;
    int calls = 0;
    struct Local final : VCPULocal<Value> { using VCPULocal<Value>::VCPULocal; };
    auto local = new Local({}, [state, &calls](Value* value) {
        EXPECT_EQ(42, *state);
        EXPECT_EQ(value->built_on, photon::get_vcpu());
        ++calls;
        delete value;
    });
    state.reset();
    pool->call([&] { ASSERT_NE(nullptr, local->get()); });
    std::thread destroyer([&] { delete local; });
    destroyer.join();
    EXPECT_EQ(0, calls);
    EXPECT_FALSE(retained.expired());
    pool.reset();
    EXPECT_EQ(1, calls);
    EXPECT_EQ(1, g_dtor.load());
    EXPECT_TRUE(retained.expired());
}

TEST(vcpu_local, custom_deleter_on_owner_and_not_for_failed_factory) {
    ASSERT_EQ(0, photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE));
    DEFER(photon::fini());
    reset();
    WorkPool pool(1, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE, -1);
    int calls = 0;
    bool succeed = false;
    auto factory = [&]() -> Value* { return succeed ? new Value : nullptr; };
    {
        VCPULocal<Value> local(factory, [&](Value* value) {
            EXPECT_EQ(value->built_on, photon::get_vcpu());
            ++calls;
            delete value;
        });
        pool.call([&] { EXPECT_EQ(nullptr, local.get()); });
    }
    EXPECT_EQ(0, calls);
    succeed = true;
    {
        VCPULocal<Value> local(factory, [&](Value* value) {
            EXPECT_EQ(value->built_on, photon::get_vcpu());
            ++calls;
            delete value;
        });
        pool.call([&] { ASSERT_NE(nullptr, local.get()); });
    }
    EXPECT_EQ(1, calls);
    EXPECT_EQ(1, g_dtor.load());
}

TEST(vcpu_local, rebuild_after_fork) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    DEFER(photon::fini());
    reset();
    VCPULocal<Value> local;
    auto parent_value = local.get();
    ASSERT_NE(nullptr, parent_value);

    auto pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        auto child_value = local.get();
        bool ok = child_value && child_value != parent_value &&
                  g_ctor.load() == 2 && g_dtor.load() == 0;
        _exit(ok ? 0 : 1);
    }

    int status = 0;
    ASSERT_EQ(pid, waitpid(pid, &status, 0));
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(0, WEXITSTATUS(status));
    EXPECT_EQ(parent_value, local.get());
    EXPECT_EQ(1, g_ctor.load());
}

TEST(vcpu_local, inherited_custom_deleter_state_is_abandoned_in_child) {
    ASSERT_EQ(0, photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE));
    DEFER(photon::fini());
    reset();
    int captureDestroyed = 0;
    auto state = std::shared_ptr<int>(new int(42), [&](int* p) {
        ++captureDestroyed;
        delete p;
    });
    struct Local final : VCPULocal<Value> { using VCPULocal<Value>::VCPULocal; };
    auto local = new Local({}, [state](Value* value) { delete value; });
    state.reset();
    ASSERT_NE(nullptr, local->get());
    auto pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        delete local;
        bool ok = g_dtor.load() == 0 && captureDestroyed == 0;
        ok = photon::fini() == 0 && g_dtor.load() == 0 &&
             captureDestroyed == 0 && ok;
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    ASSERT_EQ(pid, waitpid(pid, &status, 0));
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(0, WEXITSTATUS(status));
    delete local;
    EXPECT_EQ(1, g_dtor.load());
    EXPECT_EQ(1, captureDestroyed);
}

TEST(vcpu_local, destroy_then_fini_after_fork_without_get) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    DEFER(photon::fini());
    reset();
    struct Local final : VCPULocal<Value> {};
    auto local = new Local;
    ASSERT_NE(nullptr, local->get());

    auto pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        delete local;
        bool ok = g_dtor.load() == 0;
        ok = photon::fini() == 0 && g_dtor.load() == 0 && ok;
        _exit(ok ? 0 : 1);
    }

    int status = 0;
    ASSERT_EQ(pid, waitpid(pid, &status, 0));
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(0, WEXITSTATUS(status));
    EXPECT_EQ(0, g_dtor.load());
    delete local;
    EXPECT_EQ(1, g_dtor.load());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
