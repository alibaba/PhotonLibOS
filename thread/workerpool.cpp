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

#include "workerpool.h"

#include <photon/common/lockfree_queue.h>
#include <photon/photon.h>
#include <photon/thread/thread-pool.h>
#include <photon/thread/thread.h>

#include <algorithm>
#include <future>
#include <random>
#include <thread>
#include <atomic>

namespace photon {

class WorkPool::impl {
public:
    static constexpr uint32_t RING_SIZE = 65536;
    static constexpr uint64_t QUEUE_YIELD_COUNT = 256;
    static constexpr uint64_t QUEUE_YIELD_US = 1024;

    photon::mutex worker_mtx;
    std::vector<std::thread> owned_std_threads;
    std::vector<photon::vcpu_base *> vcpus;
    std::atomic<uint64_t> vcpu_index{0};
    photon::semaphore queue_sem;
    photon::condition_variable exit_cv;
    photon::common::RingChannel<
        LockfreeMPMCRingQueue<Delegate<void>, RING_SIZE>>
        ring;
    int mode;

    impl(size_t vcpu_num, int ev_engine, int io_engine, int mode)
        : queue_sem(0), mode(mode), ready_vcpu(0) {
        for (size_t i = 0; i < vcpu_num; ++i) {
            owned_std_threads.emplace_back(
                &WorkPool::impl::worker_thread_routine, this, ev_engine,
                io_engine);
        }
        ready_vcpu.wait(vcpu_num);
    }

    ~impl() {
        auto num = vcpus.size();
        for (size_t i = 0; i < num; i++) {
            enqueue({});
        }
        for (auto &worker : owned_std_threads) worker.join();
        photon::scoped_lock lock(worker_mtx);
        while (vcpus.size()) exit_cv.wait(lock, 1UL * 1000);
    }

    void enqueue(Delegate<void> call, AutoContext = {}) {
        if (likely(CURRENT)) ring.send<PhotonPause>(call);
        else                 ring.send<ThreadPause>(call);
    }
    void enqueue(Delegate<void> call, StdContext) {
        ring.send<ThreadPause>(call);
    }
    void enqueue(Delegate<void> call, PhotonContext) {
        ring.send<PhotonPause>(call);
    }
    template <typename Context>
    void do_call(Delegate<void> call) {
        Awaiter<Context> aop;
        auto task = [call, &aop] {
            call();
            aop.resume();
        };
        enqueue(task, Context());
        aop.suspend();
    }

    int get_vcpu_num() {
        photon::scoped_lock _(worker_mtx);
        return vcpus.size();
    }

    void worker_thread_routine(int ev_engine, int io_engine) {
        photon::init(ev_engine, io_engine);
        DEFER(photon::fini());
        main_loop();
    }

    void add_vcpu() {
        photon::scoped_lock _(worker_mtx);
        vcpus.push_back(photon::get_vcpu());
    }

    void remove_vcpu() {
        photon::scoped_lock _(worker_mtx);
        auto v = photon::get_vcpu();
        auto it = std::find(vcpus.begin(), vcpus.end(), v);
        vcpus.erase(it);
        exit_cv.notify_all();
    }

    struct TaskLB {
        Delegate<void> task;
        std::atomic<uint64_t> *count;
    };

    void main_loop() {
        add_vcpu();
        DEFER(remove_vcpu());
        std::atomic<uint64_t> running_tasks{0};
        photon::ThreadPoolBase *pool = nullptr;
        if (mode > 0) pool = photon::new_thread_pool(mode);
        DEFER(if (pool) delete_thread_pool(pool));
        ready_vcpu.signal(1);
        for (;;) {
            auto task = ring.recv(running_tasks.load() ? 0 : QUEUE_YIELD_COUNT,
                                  QUEUE_YIELD_US);
            if (!task) break;
            running_tasks.fetch_add(1, std::memory_order_acq_rel);
            TaskLB tasklb{task, &running_tasks};
            if (mode < 0) {
                delegate_helper(&tasklb);
            } else if (mode == 0) {
                auto th = photon::thread_create(
                    &WorkPool::impl::delegate_helper, &tasklb);
                photon::thread_yield_to(th);
            } else {
                auto th = pool->thread_create(&WorkPool::impl::delegate_helper,
                                              &tasklb);
                photon::thread_yield_to(th);
            }
        }
        while (running_tasks.load(std::memory_order_acquire))
            photon::thread_yield();
    }

    static void *delegate_helper(void *arg) {
        // must copy to keep tasklb alive
        TaskLB tasklb = *(TaskLB*)arg;
        tasklb.task();
        tasklb.count->fetch_sub(1, std::memory_order_acq_rel);
        return nullptr;
    }

    photon::vcpu_base *get_vcpu_in_pool(size_t index) {
        auto size = vcpus.size();
        if (index >= size) {
            index = vcpu_index++ % size;
        }
        return vcpus[index];
    }

    int join_current_vcpu_into_workpool() {
        if (!photon::CURRENT) return -1;
        main_loop();
        return 0;
    }

private:
    class StdSemaphore {
    public:
        explicit StdSemaphore(size_t count = 0) : count_(count) {
        }

        void signal(size_t n = 1) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                count_ += n;
            }
            cv_.notify_one();
        }

        void wait(size_t n = 1) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this, n] { return count_ >= n; });
            count_ -= n;
        }

    private:
        std::mutex mutex_;
        std::condition_variable cv_;
        size_t count_;
    };

    StdSemaphore ready_vcpu;
};

WorkPool::WorkPool(size_t vcpu_num, int ev_engine, int io_engine, int mode)
    : pImpl(new impl(vcpu_num, ev_engine, io_engine, mode)) {}

WorkPool::~WorkPool() {}

template <>
void WorkPool::do_call<AutoContext>(Delegate<void> call) {
    pImpl->do_call<AutoContext>(call);
}
template <>
void WorkPool::do_call<StdContext>(Delegate<void> call) {
    pImpl->do_call<StdContext>(call);
}
template <>
void WorkPool::do_call<PhotonContext>(Delegate<void> call) {
    pImpl->do_call<PhotonContext>(call);
}

void WorkPool::enqueue(Delegate<void> call) { pImpl->enqueue(call); }
photon::vcpu_base *WorkPool::get_vcpu_in_pool(size_t index) {
    return pImpl->get_vcpu_in_pool(index);
}
int WorkPool::join_current_vcpu_into_workpool() {
    return pImpl->join_current_vcpu_into_workpool();
}
int WorkPool::get_vcpu_num() { return pImpl->get_vcpu_num(); }
}  // namespace photon