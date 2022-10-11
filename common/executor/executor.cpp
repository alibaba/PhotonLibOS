#include "executor.h"

#include <photon/common/alog.h>
#include <photon/common/event-loop.h>
#include <photon/common/executor/executor.h>
#include <photon/common/lockfree_queue.h>
#include <photon/common/utility.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread-pool.h>
#include <photon/thread/thread11.h>

#include <atomic>
#include <thread>

namespace photon {

class ExecutorImpl {
public:
    using CBList = LockfreeMPMCRingQueue<Delegate<void>, 32UL * 1024>;
    std::unique_ptr<std::thread> th;
    photon::thread *pth = nullptr;
    EventLoop *loop = nullptr;
    CBList queue;
    photon::ThreadPoolBase *pool;
    bool quiting;
    photon::semaphore sem;
    std::atomic_bool waiting;

    ExecutorImpl() {
        loop = new_event_loop({this, &ExecutorImpl::wait_for_event},
                              {this, &ExecutorImpl::on_event});
        th.reset(new std::thread(&ExecutorImpl::do_loop, this));
        quiting = false;
        waiting = true;
        while (!loop || loop->state() != loop->WAITING) ::sched_yield();
    }

    ~ExecutorImpl() {
        photon::thread_interrupt(pth);
        th->join();
    }

    int wait_for_event(EventLoop *) {
        std::atomic_thread_fence(std::memory_order_acquire);
        sem.wait(1);
        waiting.store(true, std::memory_order_release);
        return quiting ? -1 : 1;
    }

    struct CallArg {
        Delegate<void> task;
        photon::thread *backth;
    };

    static void *do_event(void *arg) {
        auto a = (CallArg *)arg;
        auto task = a->task;
        photon::thread_yield_to(a->backth);
        task();
        return nullptr;
    }

    int on_event(EventLoop *) {
        CallArg arg;
        arg.backth = photon::CURRENT;
        size_t cnt = 0;
        while (!queue.empty()) {
            arg.task = queue.recv<PhotonPause>();
            auto th =
                pool->thread_create(&ExecutorImpl::do_event, (void *)&arg);
            photon::thread_yield_to(th);
            cnt++;
        }
        return 0;
    }

    void do_loop() {
        photon::vcpu_init();
        photon::fd_events_init();
        pth = photon::CURRENT;
        LOG_INFO("worker start");
        pool = photon::new_thread_pool(32);
        loop->async_run();
        photon::thread_usleep(-1);
        LOG_INFO("worker finished");
        while (!queue.empty()) {
            photon::thread_usleep(1000);
        }
        quiting = true;
        sem.signal(1);
        delete loop;
        photon::delete_thread_pool(pool);
        pool = nullptr;
        photon::fd_events_fini();
        photon::vcpu_fini();
    }
};

ExecutorImpl *_new_executor() { return new ExecutorImpl(); }

void _delete_executor(ExecutorImpl *e) { delete e; }

void _issue(ExecutorImpl *e, Delegate<void> act) {
    e->queue.send<ThreadPause>(act);
    bool cond = true;
    if (e->waiting.compare_exchange_weak(cond, false,
                                         std::memory_order_acq_rel)) {
        e->sem.signal(1);
    }
}

}  // namespace photon