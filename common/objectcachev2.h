#pragma once

#include <photon/common/alog.h>
#include <photon/common/lockfree_queue.h>
#include <photon/photon.h>
#include <photon/thread/list.h>
#include <photon/thread/thread11.h>
#include <photon/thread/timer.h>
#include <photon/thread/workerpool.h>

#include <memory>
#include <unordered_set>
#include <vector>

/**

A simpler but effective object cache implementation.

`ObjectCacheV2` shows better performance compare to `ObjectCache`, with several
improvements:

1. Less lock in both read and write.
2. Object destruction always goes in background photon thread called reclaimer.
3. Self adjustable timeout for reclaimer. No needs to set cycler timer.
4. New `update` API, able to immediately substitute objects.
5. `ObjectCacheV2` no longer support acquire/release API.

It should work as `ObjectCache` in code do not depends on acquire/release API.

**/

template <typename K, typename VPtr>
class ObjectCacheV2 {
protected:
    using V = std::remove_pointer_t<VPtr>;

    struct Box : public intrusive_list_node<Box> {
        const K key;
        std::shared_ptr<V> ref;
        // prevent create multiple time when borrow
        photon::spinlock createlock;
        // create timestamp, for cool-down of borrow
        uint64_t lastcreate = 0;
        // reclaim timestamp
        uint64_t timestamp = 0;
        // Box reference count
        std::atomic<uint64_t> rc{0};

        Box() = default;
        template <typename KeyType>
        explicit Box(KeyType&& key)
            : key(std::forward<KeyType>(key)), ref(nullptr) {}

        std::shared_ptr<V> update(std::shared_ptr<V> r, uint64_t ts = 0) {
            lastcreate = ts;
            return std::atomic_exchange(&ref, r);
        }
        std::shared_ptr<V> reset(uint64_t ts = 0) {
            return update({nullptr}, ts);
        }
        std::shared_ptr<V> reader() { return std::atomic_load(&ref); }

        void acquire() {
            timestamp = photon::now;
            rc.fetch_add(1, std::memory_order_relaxed);
        }

        void release() {
            timestamp = photon::now;
            // release reference should use stronger order
            rc.fetch_sub(1, std::memory_order_seq_cst);
        }
    };
    // Hash and Equal for Box, for unordered_set
    // simply use hash/equal of key
    struct BoxHash {
        size_t operator()(const Box& x) const { return std::hash<K>()(x.key); }
    };
    struct BoxEqual {
        bool operator()(const Box& a, const Box& b) const {
            return a.key == b.key;
        }
    };

    // protect object cache map
    photon::spinlock maplock;
    // protect lru list
    std::unordered_set<Box, BoxHash, BoxEqual> map;
    intrusive_list<Box> lru_list;
    uint64_t lifespan;
    photon::Timer _timer;
    bool _exit = false;

    template <typename KeyType>
    Box& __find_or_create_box(KeyType&& key) {
        Box keybox(key);
        Box* box = nullptr;
        SCOPED_LOCK(maplock);
        auto it = map.find(keybox);
        if (it == map.end()) {
            auto rt = map.emplace(std::forward<KeyType>(key));
            if (rt.second) box = (Box*)&*rt.first;
        } else
            box = (Box*)&*it;
        assert(box);
        lru_list.pop(box);
        box->acquire();
        return *box;
    }
    uint64_t __expire() {
        std::vector<std::shared_ptr<V>> to_release;
        uint64_t now = photon::now;
        uint64_t reclaim_before = photon::sat_sub(now, lifespan);
        {
            SCOPED_LOCK(maplock);
            if (lru_list.empty()) return 0;
            if (lru_list.front()->timestamp > reclaim_before) {
                return lru_list.front()->timestamp - reclaim_before;
            }
            auto x = lru_list.front();
            // here requires lock dance for lru_list and kv
            while (x && x->timestamp < reclaim_before) {
                lru_list.pop(x);
                if (x->rc == 0) {
                    // make vector holds those shared_ptr
                    // prevent object destroy in critical zone
                    to_release.push_back(x->reset());
                    map.erase(*x);
                }
                x = lru_list.front();
            }
        }
        to_release.clear();
        return 0;
    }

public:
    struct Borrow {
        ObjectCacheV2* _oc = nullptr;
        Box* _box = nullptr;
        std::shared_ptr<V> _reader;
        bool _recycle = false;

        Borrow() : _reader(nullptr) {}

        Borrow(ObjectCacheV2* oc, Box* box, const std::shared_ptr<V>& reader)
            : _oc(oc), _box(box), _reader(reader), _recycle(false) {
            _box->acquire();
        }

        Borrow(Borrow&& rhs) : _reader(nullptr) { *this = std::move(rhs); }

        Borrow& operator=(Borrow&& rhs) = default;

        ~Borrow() {
            if (_recycle) {
                _box->reset();
            }
            _box->release();
            if (_box->rc == 0) {
                SCOPED_LOCK(_oc->maplock);
                _oc->lru_list.pop(_box);
                _oc->lru_list.push_back(_box);
            }
        }

        bool recycle() { return _recycle; }

        bool recycle(bool x) { return _recycle = x; }

        V& operator*() const { return *_reader; }
        V* operator->() const { return &*_reader; }
        operator bool() const { return (bool)_reader; }
    };

    template <typename KeyType, typename Ctor>
    Borrow borrow(KeyType&& key, Ctor&& ctor, uint64_t cooldown = 0UL) {
        auto& box = __find_or_create_box(std::forward<KeyType>(key));
        DEFER(box.release());
        std::shared_ptr<V> r{};
        while (!r) {
            if (box.createlock.try_lock() == 0) {
                DEFER(box.createlock.unlock());
                r = box.reader();
                if (!r) {
                    if (photon::sat_add(box.lastcreate, cooldown) <=
                        photon::now) {
                        auto r = std::shared_ptr<V>(ctor());
                        box.update(r, photon::now);
                        return Borrow(this, &box, r);
                    }
                    return Borrow(this, &box, r);
                }
            }
            photon::thread_yield();
            r = box.reader();
        }
        return Borrow(this, &box, r);
    }

    template <typename KeyType>
    Borrow borrow(KeyType&& key) {
        return borrow(std::forward<KeyType>(key),
                      [&]() { return std::make_shared<V>(); });
    }

    template <typename KeyType, typename Ctor>
    Borrow update(KeyType&& key, Ctor&& ctor) {
        auto& box = __find_or_create_box(std::forward<KeyType>(key));
        DEFER(box.release());
        auto r = std::shared_ptr<V>(ctor());
        box.update(r, photon::now);
        return Borrow(this, &box, r);
    }

    ObjectCacheV2(uint64_t lifespan)
        : lifespan(lifespan),
          _timer(1UL * 1000 * 1000, {this, &ObjectCacheV2::__expire}, true,
                 photon::DEFAULT_STACK_SIZE) {}

    ~ObjectCacheV2() {
        _timer.stop();
        // Should be no other access during dtor.
        // modify lru_list do not need a lock.
        lru_list.node = nullptr;
    }
};