#include <assert.h>
#include <photon/common/callback.h>
#include <photon/thread/list.h>
#include <photon/thread/thread-local.h>
#include <photon/thread/thread11.h>

#include <atomic>

/***
RCUPtr<T>

Make a RCU protected pointer.

Reader of this pointer should call quiescent() after read done.

Writer of this pointer should use update(T*) to make object update, then
* use `synchronize(domain)` to wait all readers finished reading old object then
delete it, or
* use `rcu_call(old)` to create a standalone photon thread waiting others finish
reading then clear it.

CAUTION: RCUPtr is a mechanism to protect high concurrent read with a little
write. This implemention is based on userspace qsbr RCU, but basic interface
likes kernel RCU, but with a few limitations. Photon thread will be take as
Reader and Writer instead of system thread.

If you dont know how to use it, or not sure if you need it, just ignore it.

***/

struct RCUDomain;

// photon thread local counter
struct RCUReader : public intrusive_list_node<RCUReader> {
    std::atomic<uint64_t> ctr;
    RCUDomain *domain;

    explicit RCUReader(RCUDomain *domain);
    ~RCUReader();

    uint64_t get_gp() { return ctr.load(std::memory_order_acquire); }

    void quiescent();

    void offline() { ctr.store(0, std::memory_order_release); }

    void online() { quiescent(); }
};

// Global gp counter
struct RCUDomain {
    std::atomic<uint64_t> ctr;
    intrusive_list<RCUReader> tlist;
    photon::mutex tlock;
    photon::thread_local_ptr<RCUReader, RCUDomain *> reader;

    RCUDomain() : ctr(0), tlock(), reader(this) {}

    ~RCUDomain() { tlist.delete_all(); }

    uint64_t get_gp() { return ctr.load(std::memory_order_acquire); }

    void register_reader(RCUReader *x) {
        SCOPED_LOCK(tlock);
        tlist.push_back(x);
    }

    void unregister_reader(RCUReader *x) {
        SCOPED_LOCK(tlock);
        tlist.pop(x);
    }

    void online_cuurent() { reader->online(); }

    void offline_current() { reader->offline(); }

    void __do_synchronize(uint64_t ts) {
        reader->get_gp();
        for (;;) {
            {
                SCOPED_LOCK(tlock);
                if (reader->get_gp()) reader->quiescent();
                auto all_release = true;
                for (auto t : tlist) {
                    auto x = t->get_gp();
                    if (x && (x < ts)) {
                        all_release = false;
                        break;
                    }
                }
                if (all_release) return;
            }
            photon::thread_yield();
        }
    }

    void synchronize() {
        auto x = ctr.fetch_add(1, std::memory_order_acq_rel);
        __do_synchronize(x);
    }

    template <typename T>
    void call_rcu(T *data, Delegate<void, T *> func) {
        auto x = ctr.fetch_add(1, std::memory_order_acq_rel);
        if (reader->get_gp()) reader->quiescent();
        photon::thread_create11([x, data, func, this]() {
            __do_synchronize(x);
            func(data);
        });
    }
};

inline RCUReader::RCUReader(RCUDomain *domain) : ctr(0), domain(domain) {
    domain->register_reader(this);
}

inline RCUReader::~RCUReader() { domain->unregister_reader(this); }

inline void RCUReader::quiescent() {
    ctr.store(domain->get_gp(), std::memory_order_release);
}

inline RCUDomain *global_rcu_domain() {
    static RCUDomain domain;
    return &domain;
}

template <typename T>
struct RCUPtr {
    std::atomic<T *> ptr;

    RCUPtr() : ptr(nullptr) {}
    ~RCUPtr() { assert(ptr.load() == nullptr); }

    static void __default_deleter(void *, T *x) { delete x; }

    // read_lock/unlock is just a non-action
    void read_lock(RCUDomain *domain = nullptr) {}
    void read_unlock(RCUDomain *domain = nullptr) {}

    // Reader should call quiescent in a few rounds of read
    // and make sure all readers finished reading old object when calling it.
    void quiescent(RCUDomain *domain = nullptr) {
        if (!domain) domain = global_rcu_domain();
        domain->reader->quiescent();
    }
    // A atomic update for writer
    T *update(T *new_ptr, RCUDomain *domain = nullptr) {
        return ptr.exchange(new_ptr, std::memory_order_acq_rel);
    }
    // synchronize with domain, wait for grace period
    void synchronize(RCUDomain *domain = nullptr) {
        if (!domain) domain = global_rcu_domain();
        domain->synchronize();
    }
    // async wait for grace period
    void rcu_call(T *old, RCUDomain *domain = nullptr,
                  Delegate<void, T *> func = {nullptr,
                                              &RCUPtr::__default_deleter}) {
        if (!domain) domain = global_rcu_domain();
        domain->call_rcu(old, func);
    }
    T *dereference() const { return ptr.load(std::memory_order_acquire); }
    T &operator*() const { return *dereference(); }
    T *operator->() const { return &*dereference(); }
};
