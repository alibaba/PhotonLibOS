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

#pragma once
#include <cerrno>

#include <memory>
#include <tuple>
#include <type_traits>
#include <unordered_map>

#include <photon/common/object.h>
#include <photon/thread/list.h>
#include <photon/thread/thread.h>
#include <photon/thread/timer.h>
#include <photon/common/string-keyed.h>
#include <photon/common/timeout.h>
#include <photon/common/utility.h>

template <typename T, typename... Ts>
struct Payload : public Payload<Ts...> {
    T p;

    template <typename Arg, typename... Args>
    Payload(Arg&& t, Args&&... ts)
        : Payload<Ts...>(std::forward<Args>(ts)...), p(std::forward<Arg>(t)) {}
    Payload() : Payload<Ts...>(), p() {}
};

template <typename T>
struct Payload<T> {
    T p;

    template <typename Arg>
    Payload(Arg&& t) : p(std::forward<Arg>(t)) {}
    Payload() = default;
};

template <int idx, typename T, typename... Ts>
struct __type_idx_helper : public __type_idx_helper<idx - 1, Ts...> {
    using __type_idx_helper<idx - 1, Ts...>::type;
    using __type_idx_helper<idx - 1, Ts...>::ptype;
};

template <typename T, typename... Ts>
struct __type_idx_helper<0, T, Ts...> {
    using type = T;
    using ptype = Payload<T, Ts...>;
};

template <int idx, typename T, typename... Ts>
inline typename __type_idx_helper<idx, T, Ts...>::type& GetPayload(
    Payload<T, Ts...>& payload) {
    return ((typename __type_idx_helper<idx, T, Ts...>::ptype&)(payload)).p;
}

template <typename KeyType, typename... Ts>
class ExpireContainer : public Object {
public:
    using MapKey =
        typename std::conditional<std::is_base_of<std::string, KeyType>::value,
                                  std::string_view, KeyType>::type;
    struct ItemRef : public intrusive_list_node<ItemRef> {
        Timeout timeout;
        // Actual key stores in payload
        Payload<MapKey, Ts...> payload;

        template <typename... Gs>
        ItemRef(const MapKey& key, uint64_t expire, Gs&&... xs)
            : timeout(expire), payload(key, std::forward<Gs>(xs)...) {}

        MapKey key() { return GetPayload<0>(payload); }
    };

    using Map =
        typename std::conditional<std::is_base_of<std::string, KeyType>::value,
                                  unordered_map_string_key<ItemRef*>,
                                  std::unordered_map<KeyType, ItemRef*>>::type;

    explicit ExpireContainer(uint64_t expiration)
        : m_expiration(expiration),
          m_timer(expiration, {this, &ExpireContainer::expire}, true,
                  8UL * 1024 * 1024) {}

    void clear() {
        m_list.node = nullptr;
        for (auto& x : m_map) {
            delete x.second;
        }
        m_map.clear();
    }
    ~ExpireContainer() override {
        m_timer.stop();
        clear();
    }

    template <typename... Gs>
    typename Map::iterator insert(const MapKey& key, Gs&&... xs) {
        // when key is string view for outside,
        // it should change to an string view object that
        // refers key_string inside m_map
        // so emplace first, then create ItemRef by m_map iter
        auto it = insert_into_map(key, std::forward<Gs>(xs)...);
        if (it == m_map.end()) return m_map.end();
        m_list.push_back(it->second);
        return it;
    }

    uint64_t expire() {
        photon::scoped_lock lock(m_mtx);
        while (!m_list.empty() &&
               m_list.front()->timeout.expire() < photon::now) {
            auto p = m_list.pop_front();
            // make sure that m_map erased p before release p
            m_map.erase(p->key());
            delete p;
        }
        return 0;
    };

    // return the # of items currently in the list
    size_t size() { return m_map.size(); }

    // time to expire, in us
    size_t expiration() { return m_expiration; }

    struct iterator : public Map::iterator {
        using base = typename Map::iterator;
        iterator() = default;
        iterator(base it) : base(it) {}
        MapKey operator*() { return base::operator*().second->key(); }
    };

    iterator begin() { return m_map.begin(); }

    iterator end() { return m_map.end(); }

    iterator find(const MapKey& key) { return m_map.find(key); }

protected:
    uint64_t m_expiration;
    intrusive_list<ItemRef> m_list;
    Map m_map;
    photon::Timer m_timer;
    photon::mutex m_mtx;

    // update timestamp of a reference to item.
    void refresh(ItemRef* ref) {
        m_list.pop(ref);
        ref->timeout.timeout(m_expiration);
        m_list.push_back(ref);
    }

    template <typename... Gs>
    typename Map::iterator insert_into_map(const MapKey& key, Gs&&... xs) {
        // when key is string view for outside,
        // it should change to an string view object that
        // refers key_string inside m_map
        // so emplace first, then create ItemRef by m_map iter
        auto res = m_map.emplace(key, nullptr);
        if (!res.second) return m_map.end();
        auto it = res.first;
        auto node =
            new ItemRef(it->first, m_expiration, std::forward<Gs>(xs)...);
        it->second = node;
        return it;
    }
};

// a set / list like structure
// able to query whether an item not expired in it.
template <typename ItemType>
class ExpireList : public ExpireContainer<ItemType> {
protected:
    using base = ExpireContainer<ItemType>;
    using base::base;
    using base::expire;
    using base::insert;
    using base::m_map;
    using base::refresh;

public:
    bool keep_alive(ItemType item, bool insert_if_not_exists) {
        DEFER(expire());
        photon::scoped_lock lock(this->m_mtx);
        auto iter = m_map.find(item);
        if (iter != m_map.end()) {
            refresh(iter->second);
        } else if (insert_if_not_exists) {
            insert(item);
        } else
            return false;
        return true;
    }
};

// Resource pool based on reference count
// when the pool is fulled, it will try to remove items which can be sure is not
// referenced the base m_list works as gc list when object acquired, construct
// or findout the object, add reference count; when object release, reduce
// refcount. if some resource is not referenced, it will be put back to gc list
// waiting to release.
template <typename T,
          typename D = typename std::default_delete<
              typename std::remove_pointer<T>::type>,
          typename = typename std::enable_if<std::is_pointer<T>::value>::type>
using UniqPtr =
    typename std::unique_ptr<typename std::remove_pointer<T>::type, D>;

template <typename KeyType, typename ValType,
          typename Deleter = typename std::default_delete<
              typename std::remove_pointer<ValType>::type>>
class ObjectCache : public ExpireContainer<KeyType, UniqPtr<ValType>, uint64_t,
                                           bool, photon::mutex> {
protected:
    using base = ExpireContainer<KeyType, UniqPtr<ValType>, uint64_t, bool,
                                 photon::mutex>;
    using base::base;
    using base::expire;
    using base::insert_into_map;
    using base::m_list;
    using base::m_map;
    using typename base::ItemRef;
    using typename base::MapKey;

    photon::condition_variable release_cv, block_cv;

public:
    static inline MapKey& get_key(ItemRef* ref) {
        return GetPayload<0>(ref->payload);
    }
    static inline UniqPtr<ValType>& get_ptr(ItemRef* ref) {
        return GetPayload<1>(ref->payload);
    }
    static inline uint64_t& get_refcnt(ItemRef* ref) {
        return GetPayload<2>(ref->payload);
    }
    static inline bool& get_block_flag(ItemRef* ref) {
        return GetPayload<3>(ref->payload);
    }
    static inline photon::mutex& get_ref_mtx(ItemRef* ref) {
        return GetPayload<4>(ref->payload);
    }

    template <typename Constructor>
    ItemRef* ref_acquire(const MapKey& key, const Constructor& ctor) {
        DEFER(expire());
        ItemRef* ref = nullptr;
        {
            photon::scoped_lock lock(this->m_mtx);
            auto iter = m_map.find(key);
            // check if it is in immediately release
            // if it is true, block till complete
            while (iter != m_map.end() && get_block_flag(iter->second)) {
                block_cv.wait(lock);
                iter = m_map.find(key);
            }
            if (iter == m_map.end()) {
                // create an empty reference for item, block before make sure
                iter = insert_into_map(key, nullptr, 0, true);
                if (iter == m_map.end()) return nullptr;
            } else {
                m_list.pop(iter->second);
            };
            // here got a item reference
            ref = iter->second;
            assert(ref != nullptr);
        }
        {
            photon::scoped_lock reflock(get_ref_mtx(ref));
            if (get_ptr(ref) == nullptr) {
                get_ptr(ref).reset(ctor());
                get_block_flag(ref) = false;
            }
            if (get_ptr(ref) != nullptr) {
                get_refcnt(ref)++;
            }
        }
        {
            photon::scoped_lock lock(this->m_mtx);
            if (get_ptr(ref) == nullptr) {
                if (get_refcnt(ref) == 0) {
                    ref->timeout.timeout(this->m_expiration);
                    m_list.push_back(ref);
                }
                ref = nullptr;
            }
            block_cv.notify_all();
        }
        return ref;
    }

    ItemRef* ref_acquire(const MapKey& key) {
        return ref_acquire(key, [] {
            return new typename std::remove_pointer<ValType>::type();
        });
    }

    // acquire resource with identity key. It can be sure to get an resource
    // resources are reusable, managed by reference count and key.
    // when the pool is full and not able to create any resource, it will block
    // till resourece created
    template <typename Constructor>
    ValType acquire(const MapKey& key, const Constructor& ctor) {
        auto ret = ref_acquire(key, ctor);
        return ret ? get_ptr(ret).get() : nullptr;
    }

    ValType acquire(const MapKey& key) {
        return acquire(key, [] {
            return new typename std::remove_pointer<ValType>::type();
        });
    }

    // once user finished using a resource, it should call release(key) to tell
    // the pool that the reference is over
    int ref_release(ItemRef* ref, bool recycle = false) {
        DEFER(expire());
        if (!ref) return 0;
        bool cycler = false;
        uint64_t ret = 0;
        {
            photon::scoped_lock reflock(get_ref_mtx(ref));
            if (recycle &&
                !get_block_flag(ref)) {  // this thread should do recycle
                get_block_flag(ref) = true;
                cycler = true;
            }
            auto& refcnt = get_refcnt(ref);
            if (!cycler) {
                refcnt--;
                release_cv.notify_all();
                if (!recycle) ret = refcnt;
            } else {
                while (refcnt > 1) release_cv.wait(reflock);
                refcnt--;
                ret = refcnt;
                assert(ret == 0);
                get_ptr(ref).reset(nullptr);
                get_block_flag(ref) = false;
            }
        }
        {
            photon::scoped_lock lock(this->m_mtx);
            if (get_refcnt(ref) == 0) {
                ref->timeout.timeout(this->m_expiration);
                m_list.push_back(ref);
            }
            block_cv.notify_all();
        }
        return ret;
    }

    int release(const MapKey& key, bool recycle = false) {
        ItemRef* ref = nullptr;
        {
            photon::scoped_lock lock(this->m_mtx);
            auto iter = m_map.find(key);
            if (iter == m_map.end()) {
                return -1;
            }
            ref = iter->second;
        }
        return ref_release(ref, recycle);
    }

    class Borrow {
        ObjectCache* _oc;
        ItemRef* _ref;
        bool _recycle = false;
        using ValPtr = ValType;
        using ValEntity = typename std::remove_pointer<ValType>::type;

    public:
        Borrow(ObjectCache* oc, ItemRef* ref, bool recycle)
            : _oc(oc), _ref(ref), _recycle(recycle) {}
        ~Borrow() {
            if (_ref) _oc->ref_release(_ref, _recycle);
        }

        Borrow() = delete;
        Borrow(const Borrow&) = delete;
        Borrow(Borrow&& rhs) { move(std::move(rhs)); }
        void operator=(const Borrow&) = delete;
        void operator=(Borrow&& rhs) { move(rhs); }

        ValEntity& operator*() { return *get_ptr(_ref); }

        ValPtr operator->() { return get_ptr(_ref).get(); }

        operator bool() { return _ref; }

        bool recycle() { return _recycle; }

        bool recycle(bool x) { return _recycle = x; }

    private:
        void move(Borrow&& rhs) {
            _oc = rhs._oc;
            rhs._oc = nullptr;
            _ref = rhs._ref;
            rhs._ref = nullptr;
            _recycle = rhs._recycle;
        }
    };

    template <typename Constructor>
    Borrow borrow(const MapKey& key, const Constructor& ctor) {
        return Borrow(this, ref_acquire(key, ctor), false);
    }

    Borrow borrow(const MapKey& key) {
        return borrow(key, [] {
            return new typename std::remove_pointer<ValType>::type();
        });
    }
};
