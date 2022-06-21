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

#include <photon/common/ring.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>

#include <thread>
#include <vector>

namespace photon {

class WorkPool::impl {
public:
    static constexpr uint32_t RING_SIZE = 256;

    std::vector<std::thread> workers;
    photon::ticket_spinlock queue_lock;
    std::atomic<bool> stop;
    photon::semaphore queue_sem;
    RingQueue<Delegate<void>> ring;
    int th_num;

    impl(int thread_num, int ev_engine, int io_engine)
        : stop(false), queue_sem(0), ring(RING_SIZE), th_num(thread_num) {
        for (int i = 0; i < thread_num; ++i)
            workers.emplace_back(&WorkPool::impl::worker_thread_routine, this,
                                 ev_engine, io_engine);
    }

    ~impl() {
        stop = true;
        queue_sem.signal(th_num);
        for (auto& worker : workers) worker.join();
    }

    void enqueue(Delegate<void> call) {
        {
            photon::locker<photon::ticket_spinlock> lock(queue_lock);
            ring.push_back(call);
        }
        queue_sem.signal(1);
    }

    void do_call(Delegate<void> call) {
        photon::semaphore sem(0);
        auto task = [call, &sem] {
            call();
            sem.signal(1);
        };
        enqueue(Delegate<void>(task));
        sem.wait(1);
    }

    void worker_thread_routine(int ev_engine, int io_engine) {
        photon::init(ev_engine, io_engine);
        DEFER(photon::fini());
        for (;;) {
            Delegate<void> task;
            {
                queue_sem.wait(1);
                if (this->stop && ring.empty()) return;
                photon::locker<photon::ticket_spinlock> lock(queue_lock);
                ring.pop_front(task);
            }
            task();
        }
    }
};

WorkPool::WorkPool(int thread_num, int ev_engine, int io_engine)
    : pImpl(new impl(thread_num, ev_engine, io_engine)) {}

WorkPool::~WorkPool() {}

void WorkPool::do_call(Delegate<void> call) { pImpl->do_call(call); }
void WorkPool::enqueue(Delegate<void> call) { pImpl->enqueue(call); }

}  // namespace photon