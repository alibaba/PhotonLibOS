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
        photon::spinlock boxlock;
        uint64_t lastcreate = 0;
        uint64_t timestamp = 0;

        struct Updater {
            Box* box = nullptr;

            ~Updater() {}

            // Returned RefPtr is the old one
            // other reader will get new one after updated

            std::shared_ptr<V> update(std::shared_ptr<V>& r, uint64_t ts = 0) {
                box->lastcreate = ts;
                r = std::atomic_exchange(&box->ref, r);
                return r;
            }
            std::shared_ptr<V> update(V* val, uint64_t ts = 0,
                                      std::shared_ptr<V>* newptr = nullptr) {
                auto r = std::shared_ptr<V>(val);
                if (newptr) *newptr = r;
                return update(r, ts);
            }

            explicit Updater(Box* b) : box(b) {}

            Updater(const Updater&) = delete;
            Updater(Updater&& rhs) : box(nullptr) { *this = std::move(rhs); }
            Updater& operator=(const Updater&) = delete;
            Updater& operator=(Updater&& rhs) {
                std::swap(box, rhs.box);
                return *this;
            }
            operator bool() const { return box != nullptr; }
        };

        Box() = default;
        template <typename KeyType>
        explicit Box(KeyType&& key)
            : key(std::forward<KeyType>(key)), ref(nullptr) {}

        ~Box() {}

        Updater writer() { return Updater(this); }
        std::shared_ptr<V> reader() { return std::atomic_load(&ref); }
    };
    struct ItemHash {
        size_t operator()(const Box& x) const { return std::hash<K>()(x.key); }
    };
    struct ItemEqual {
        bool operator()(const Box& a, const Box& b) const {
            return a.key == b.key;
        }
    };

    photon::thread* reclaimer = nullptr;
    photon::common::RingChannel<LockfreeMPMCRingQueue<std::shared_ptr<V>, 4096>>
        reclaim_queue;
    photon::spinlock maplock;
    std::unordered_set<Box, ItemHash, ItemEqual> map;
    intrusive_list<Box> cycle_list;
    uint64_t lifespan;
    photon::Timer _timer;
    bool _exit = false;

    std::shared_ptr<V> __update(Box& item, std::shared_ptr<V> val) {
        auto ret_val = val;
        auto old_val = item.writer().update(val, photon::now);
        reclaim_queue.template send<PhotonPause>(old_val);
        return ret_val;
    }

    std::shared_ptr<V> __update(Box& item, V* val) {
        std::shared_ptr<V> ptr(val);
        return __update(item, ptr);
    }

    template <typename KeyType>
    Box& __find_or_create_item(KeyType&& key) {
        Box keyitem(key);
        Box* item = nullptr;
        SCOPED_LOCK(maplock);
        auto it = map.find(keyitem);
        if (it == map.end()) {
            // item = new Box(std::forward<KeyType>(key));
            auto rt = map.emplace(key);
            if (rt.second) item = (Box*)&*rt.first;
        } else
            item = (Box*)&*it;
        assert(item);
        item->timestamp = photon::now;
        cycle_list.pop(item);
        cycle_list.push_back(item);
        return *item;
    }

    uint64_t __expire() {
        intrusive_list<Box> delete_list;
        uint64_t now = photon::now;
        {
            SCOPED_LOCK(maplock);
            if (cycle_list.empty()) return 0;
            if (photon::sat_add(cycle_list.front()->timestamp, lifespan) >
                now) {
                return photon::sat_add(cycle_list.front()->timestamp,
                                       lifespan) -
                       now;
            }
            auto x = cycle_list.front();
            while (x && (photon::sat_add(x->timestamp, lifespan) < now)) {
                cycle_list.pop(x);
                __update(*x, nullptr);
                map.erase(*x);
                x = cycle_list.front();
            }
        }
        return 0;
    }

public:
    struct Borrow {
        ObjectCacheV2* _oc = nullptr;
        Box* _item = nullptr;
        std::shared_ptr<V> _reader;
        bool _recycle = false;

        Borrow() : _reader(nullptr) {}

        Borrow(ObjectCacheV2* oc, Box* item, std::shared_ptr<V>&& reader)
            : _oc(oc),
              _item(item),
              _reader(std::move(reader)),
              _recycle(false) {}

        Borrow(Borrow&& rhs) : _reader(nullptr) { *this = std::move(rhs); }

        Borrow& operator=(Borrow&& rhs) {
            std::swap(_oc, rhs._oc);
            std::swap(_item, rhs._item);
            _reader = std::atomic_exchange(&rhs._reader, _reader);
            std::swap(_reader, rhs._reader);
            std::swap(_recycle, rhs._recycle);
            return *this;
        }

        ~Borrow() {
            if (_recycle) {
                _oc->__update(*_item, nullptr);
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
        auto& item = __find_or_create_item(std::forward<KeyType>(key));
        auto r = item.reader();
        while (!r) {
            if (item.boxlock.try_lock() == 0) {
                DEFER(item.boxlock.unlock());
                r = item.reader();
                if (!r) {
                    if (photon::sat_add(item.lastcreate, cooldown) <=
                        photon::now) {
                        r = __update(item, ctor());
                    }
                    return Borrow(this, &item, std::move(r));
                }
            }
            photon::thread_yield();
            r = item.reader();
        }
        return Borrow(this, &item, std::move(r));
    }

    template <typename KeyType>
    Borrow borrow(KeyType&& key) {
        return borrow(std::forward<KeyType>(key),
                      [&]() { return std::make_shared<V>(); });
    }

    template <typename KeyType, typename Ctor>
    Borrow update(KeyType&& key, Ctor&& ctor) {
        auto item = __find_or_create_item(std::forward<KeyType>(key));
        auto r = __update(item, ctor());
        return Borrow(this, item, std::move(r));
    }

    ObjectCacheV2(uint64_t lifespan)
        : lifespan(lifespan),
          _timer(1UL * 1000 * 1000, {this, &ObjectCacheV2::__expire}, true,
                 photon::DEFAULT_STACK_SIZE),
          _exit(false) {
        reclaimer = photon::thread_create11([this] {
            while (!_exit) {
                reclaim_queue.recv();
            }
        });
        photon::thread_enable_join(reclaimer);
    }

    ~ObjectCacheV2() {
        _timer.stop();
        if (reclaimer) {
            _exit = true;
            reclaim_queue.template send<PhotonPause>(nullptr);
            photon::thread_join((photon::join_handle*)reclaimer);
            reclaimer = nullptr;
        }
        SCOPED_LOCK(maplock);
        cycle_list.node = nullptr;
    }
};