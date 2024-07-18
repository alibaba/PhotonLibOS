#include <photon/common/alog.h>
#include <photon/common/rcuptr.h>
#include <photon/photon.h>
#include <photon/thread/thread11.h>

#include <chrono>
#include <thread>
#include <vector>

struct IntObj {
    int val;
    IntObj(int val) : val(val) {}
    ~IntObj() {}
};

int main() {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE,
                 {.libaio_queue_depth = 32,
                  .use_pooled_stack_allocator = true,
                  .bypass_threadpool = true});
    DEFER(photon::fini());
    photon::semaphore sem(0);
    std::atomic<int> cntr(1);
    {
        RCUPtr<IntObj> ptr;
        // ptr.write_lock();
        auto n = new IntObj(cntr.fetch_add(1, std::memory_order_acq_rel));
        auto old = ptr.update(n);
        // ptr.write_unlock();
        ptr.synchronize();
        delete old;
        std::vector<std::thread> ths;
        for (int th = 0; th < 10; th++) {
            ths.emplace_back([&] {
                photon::vcpu_init();
                DEFER(photon::vcpu_fini());
                for (int i = 0; i < 100; i++) {
                    photon::thread_create11([&cntr, &ptr, &sem]() {
                        for (int j = 0; j < 1000; j++) {
                            auto x = rand() % 10;
                            if (x) {  // 99% reader
                                // ptr.read_lock();
                                auto x = ptr->val;
                                (void)x;
                                ptr.quiescent();
                                // ptr.read_unlock();
                                photon::thread_yield();
                            } else {  // 1% writer
                                // ptr.write_lock();
                                auto n = new IntObj(cntr.fetch_add(
                                    1, std::memory_order_acq_rel));
                                auto old = ptr.update(n);
                                // ptr.write_unlock();
                                // ptr.rcu_call(old);
                                ptr.synchronize();
                                delete old;
                                photon::thread_yield();
                            }
                        }
                        sem.signal(1);
                    });
                }
            });
        }

        auto start = std::chrono::high_resolution_clock::now();
        sem.wait(1000);
        auto done = std::chrono::high_resolution_clock::now();

        for (auto &x : ths) x.join();

        // ptr.write_lock();
        auto last = ptr.update(nullptr);
        // ptr.write_unlock();
        ptr.synchronize();
        delete last;

        LOG_INFO(VALUE(
            std::chrono::duration_cast<std::chrono::microseconds>(done - start)
                .count()));
    }
    LOG_INFO("!!!!!!!!!!");
    {
        photon::rwlock rwlock;
        std::atomic<IntObj *> ptr(
            new IntObj(cntr.fetch_add(1, std::memory_order_acq_rel)));
        std::vector<std::thread> ths;
        for (int th = 0; th < 10; th++) {
            ths.emplace_back([&] {
                photon::vcpu_init();
                DEFER(photon::vcpu_fini());
                for (int i = 0; i < 100; i++) {
                    photon::thread_create11([&cntr, &ptr, &sem, &rwlock]() {
                        for (int j = 0; j < 1000; j++) {
                            auto x = rand() % 10;
                            if (x) {  // 90% reader
                                photon::scoped_rwlock _(rwlock, photon::RLOCK);
                                (void)ptr.load()->val;
                            } else {  // 10% writer
                                photon::scoped_rwlock _(rwlock, photon::WLOCK);
                                auto n = new IntObj(cntr.fetch_add(
                                    1, std::memory_order_acq_rel));
                                auto old = ptr.exchange(n);
                                delete old;
                            }
                        }
                        sem.signal(1);
                    });
                }
            });
        }

        auto start = std::chrono::high_resolution_clock::now();
        sem.wait(1000);
        auto done = std::chrono::high_resolution_clock::now();

        for (auto &x : ths) x.join();

        delete ptr;

        LOG_INFO(VALUE(
            std::chrono::duration_cast<std::chrono::microseconds>(done - start)
                .count()));
    }

    return 0;
}
