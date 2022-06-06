#include "executor.h"
#include <photon/common/alog.h>
#include <photon/common/event-loop.h>
#include <photon/common/executor/executor.h>
#include <photon/common/utility.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread-pool.h>
#include <photon/thread/thread11.h>

#include <boost/lockfree/spsc_queue.hpp>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace photon {

class ExecutorImpl {
public:
    using CBList = typename boost::lockfree::spsc_queue<
        Delegate<void>, boost::lockfree::capacity<32UL * 1024>>;
    std::unique_ptr<std::thread> th;
    photon::thread *pth = nullptr;
    EventLoop *loop = nullptr;
    CBList queue;
    photon::ThreadPoolBase *pool;
    std::mutex mutex;

    ExecutorImpl() {
        loop = new_event_loop({this, &ExecutorImpl::wait_for_event},
                              {this, &ExecutorImpl::on_event});
        th.reset(new std::thread(&ExecutorImpl::do_loop, this));
        while (!loop || loop->state() != loop->WAITING) ::sched_yield();
    }

    ~ExecutorImpl() {
        photon::thread_interrupt(pth);
        th->join();
    }

    int wait_for_event(EventLoop *) {
        if (!queue.empty()) return 1;
        auto th = photon::CURRENT;
        int ret = photon::thread_usleep_defer(-1UL, [&] {
            if (!queue.empty()) photon::thread_interrupt(th, EINPROGRESS);
        });
        if (ret < 0) {
            ERRNO err;
            if (err.no == EINPROGRESS)
                return 1;
            else if (err.no == EINTR)
                return -1;
        }
        return 0;
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
        while (!queue.empty()) {
            CallArg arg;
            arg.backth = photon::CURRENT;
            if (queue.pop(arg.task)) {
                auto th =
                    pool->thread_create(&ExecutorImpl::do_event, (void *)&arg);
                photon::thread_yield_to(th);
            }
        }
        return 0;
    }

    void do_loop() {
        photon::thread_init();
        photon::fd_events_init();
        pth = photon::CURRENT;
        LOG_INFO("worker start");
        pool = photon::new_thread_pool(32);
        loop->async_run();
        photon::thread_usleep(-1);
        LOG_INFO("worker finished");
        while (!queue.empty()) photon::thread_usleep(1000);
        delete loop;
        photon::delete_thread_pool(pool);
        pool = nullptr;
        photon::fd_events_fini();
        photon::thread_fini();
    }
};

ExecutorImpl *_new_executor() { return new ExecutorImpl(); }

void _delete_executor(ExecutorImpl *e) { delete e; }

void _issue(ExecutorImpl *e, Delegate<void> act) {
    {
        std::lock_guard<std::mutex> lock(e->mutex);
        while (!e->queue.push(act)) ::sched_yield();
    }
    photon::thread_interrupt(e->loop->loop_thread(), EINPROGRESS);
}

}  // namespace photon