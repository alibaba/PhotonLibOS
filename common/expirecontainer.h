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
#include <photon/common/object.h>
#include <photon/common/string_view.h>
#include <photon/common/timeout.h>
#include <photon/common/utility.h>
#include <photon/thread/list.h>
#include <photon/thread/thread.h>
#include <photon/thread/timer.h>

#include <algorithm>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>

/**
 * @brief ExpireContainerBase is basic class for all expire containers
 * provides index for items and a release list for item recycle.
 *
 */
class ExpireContainerBase : public Object {
protected:
    class Item : public intrusive_list_node<Item> {
    protected:
        Item() : _timeout(0) {}

    public:
        Timeout _timeout;
        virtual ~Item() {}
        virtual size_t key_hash() const = 0;
        virtual bool key_equal(const Item* rhs) const = 0;
        virtual Item* construct() const = 0;
    };

    template <typename BaseItem, typename KeyType>
    class KeyedItem : public BaseItem {
    public:
        constexpr static bool _is_string_key =
            std::is_base_of<std::string, KeyType>::value ||
            std::is_base_of<std::string_view, KeyType>::value ||
            std::is_same<const char*, const KeyType>::value;

        using ItemKey = KeyType;

        using InterfaceKey =
            typename std::conditional<_is_string_key, std::string_view,
                                      ItemKey>::type;

        ItemKey _key;
        KeyedItem(const InterfaceKey& key) : _key(key) {}
        virtual size_t key_hash() const override {
            return std::hash<ItemKey>()(_key);
        }
        virtual bool key_equal(const Item* rhs) const override {
            return _key == static_cast<const KeyedItem*>(rhs)->_key;
        }
        virtual KeyedItem* construct() const override {
            return new KeyedItem(_key);
        }
        const ItemKey& key() { return _key; }
    };

    intrusive_list<Item> _list;
    uint64_t _expiration;
    photon::Timer _timer;
    photon::spinlock _lock; // protect _list/_set operations

    using ItemPtr = Item*;
    struct ItemHash {
        size_t operator()(const ItemPtr& x) const { return x->key_hash(); }
    };
    struct ItemEqual {
        size_t operator()(const ItemPtr& x, const ItemPtr& y) const {
            return x->key_equal(y);
        }
    };

    using Set = std::unordered_set<ItemPtr, ItemHash, ItemEqual>;
    Set _set;

    ExpireContainerBase(uint64_t expiration, uint64_t timer_cycle);
    ~ExpireContainerBase() { clear(); }

    using iterator = decltype(_set)::iterator;
    std::pair<iterator, bool> insert(Item* item);
    iterator begin() { return _set.begin(); }
    iterator end() { return _set.end(); }
    iterator find(const Item& key_item);
    iterator __find_prelock(const Item& key_item);

    template <typename T>
    struct TypedIterator : public iterator {
        TypedIterator(const iterator& rhs) : iterator(rhs) {}
        using TPtr = T*;
        TPtr operator*() const { return (TPtr)iterator::operator*(); }
        TPtr* operator->() const { return (TPtr*)iterator::operator->(); }
    };

    bool keep_alive(const Item& item, bool insert_if_not_exists);

    void enqueue(Item* item) {
        _list.pop(item);
        item->_timeout.timeout(_expiration);
        _list.push_back(item);
    }

public:
    void clear();
    uint64_t expire();
    size_t size() { return _set.size(); }
    size_t expiration() { return _expiration; }
};

template <typename KeyType, typename... Ts>
class ExpireContainer : public ExpireContainerBase {
public:
    using Base = ExpireContainerBase;
    ExpireContainer(uint64_t expiration) : Base(expiration, expiration / 16) {}
    ExpireContainer(uint64_t expiration, uint64_t timer_cycle)
        : Base(expiration, timer_cycle) {}

protected:
    using KeyedItem = typename Base::KeyedItem<Base::Item, KeyType>;
    class Item : public KeyedItem {
    public:
        std::tuple<Ts...> payload;

        using typename KeyedItem::InterfaceKey;
        using typename KeyedItem::ItemKey;
        using iterator = typename intrusive_list_node<Item>::iterator;

        template <typename... Gs>
        Item(const InterfaceKey& key, Gs&&... gs)
            : KeyedItem(key), payload(std::forward<Gs>(gs)...) {}

        template <size_t idx,
                  typename = typename std::enable_if<(idx > 0)>::type>
        decltype(auto) get_payload() {
            return std::get<idx - 1>(payload);
        }
        template <size_t idx,
                  typename = typename std::enable_if<(idx == 0)>::type>
        InterfaceKey get_payload() {
            return KeyedItem::_key;
        }
    };
    intrusive_list<Item>& list() { return (intrusive_list<Item>&)_list; }

public:
    using ItemKey = typename Item::ItemKey;
    using InterfaceKey = typename Item::InterfaceKey;

    using iterator = typename ExpireContainerBase::TypedIterator<Item>;
    iterator begin() { return Base::begin(); }
    iterator end() { return Base::end(); }
    iterator find(const InterfaceKey& key) {
        return Base::find(KeyedItem(key));
    }

    template <typename... Gs>
    iterator insert(const InterfaceKey& key, Gs&&... xs) {
        auto item = new Item(key, std::forward<Gs>(xs)...);
        SCOPED_LOCK(_lock);
        auto pr = Base::insert(item);
        if (!pr.second) {
            delete item;
            return end();
        }
        enqueue(item);
        return pr.first;
    }

    void refresh(Item* item) {
        DEFER(expire());
        SCOPED_LOCK(_lock);
        enqueue(item);
    }
};

// a set / list like structure
// able to query whether an item not expired in it.

template <typename T>
class ExpireList : public ExpireContainer<T> {
public:
    using Base = ExpireContainer<T>;
    using Base::Base;
    using typename Base::Item;
    bool keep_alive(const T& x, bool insert_if_not_exists) {
        return Base::keep_alive(Item(x), insert_if_not_exists);
    }
};

class ObjectCacheBase : public ExpireContainerBase {
protected:
    using Base = ExpireContainerBase;
    using Base::Base;
    using Base::KeyedItem;

    class Item : public Base::Item {
    public:
        Item() : Base::Item() {
            _obj = nullptr;
            _refcnt = 0;
            _recycle = nullptr;
            _failure = 0;
        }
        void* _obj;
        photon::mutex _mtx;
        uint32_t _refcnt;
        photon::semaphore* _recycle;
        uint64_t _failure;
    };

    photon::condition_variable blocker;

    using ItemPtr = Item*;

    // in case of missing, ref_acquire() performs a 2 phase construction:
    // (1) creating an item with _obj==nullptr, preventing
    //     concurrent construction of objects with the same key;
    // (2) construction of the object itself, and possibly do
    //     clean-up in case of failure
    Item* ref_acquire(const Item& key_item, Delegate<void, void*> ctor,
                      uint64_t failure_cooldown = 0);

    int ref_release(ItemPtr item, bool recycle = false);

    void* acquire(const Item& key_item, Delegate<void, void*> ctor,
                  uint64_t failure_cooldown = 0) {
        auto ret = ref_acquire(key_item, ctor, failure_cooldown);
        return ret ? ret->_obj : nullptr;
    }

    // the argument `key` plays the roles of (type-erased) key
    int release(const Item& key_item, bool recycle = false);

    template <typename ObjectCache, typename ItemPtr, typename ValEntity,
              typename ValPtr>
    class Borrow {
    public:
        ObjectCache* _oc;
        ItemPtr _ref;
        bool _recycle = false;

        Borrow(ObjectCache* oc, ItemPtr ref, bool recycle)
            : _oc(oc), _ref(ref), _recycle(recycle) {}
        ~Borrow() {
            if (_ref) _oc->ref_release(_ref, _recycle);
        }

        Borrow() = delete;
        Borrow(const Borrow&) = delete;
        Borrow(Borrow&& rhs) { move(std::move(rhs)); }
        void operator=(const Borrow&) = delete;
        void operator=(Borrow&& rhs) { move(rhs); }

        operator bool() const { return _ref; }

        bool recycle() const { return _recycle; }

        bool recycle(bool x) { return _recycle = x; }

    protected:
        ValPtr get_ptr() { return (ValPtr)_ref->_obj; }
        void move(Borrow&& rhs) {
            _oc = rhs._oc;
            rhs._oc = nullptr;
            _ref = rhs._ref;
            rhs._ref = nullptr;
            _recycle = rhs._recycle;
        }
    };

public:
    template<typename KeyType, typename ValPtr>
    class PtrItem
        : public KeyedItem<Item, KeyType> {
    public:
        using KeyedItem<Item, KeyType>::KeyedItem;
        virtual PtrItem* construct() const override {
            auto item = new PtrItem(this->_key);
            item->_obj = nullptr;
            item->_refcnt = 0;
            item->_recycle = nullptr;
            return item;
        }
        ~PtrItem() override { delete (ValPtr)this->_obj; }
    };

    template<typename KeyType, typename ValEntity>
    class ListItem
        : public KeyedItem<Item, KeyType> {
    public:
        ValEntity _list;
        using KeyedItem<Item, KeyType>::KeyedItem;
        virtual ListItem* construct() const override {
            auto item = new ListItem(this->_key);
            item->_obj = nullptr;
            item->_refcnt = 0;
            item->_recycle = nullptr;
            return item;
        }
        ~ListItem() { _list.delete_all(); }
    };
};

template<typename KeyType, typename ValPtr, typename ValEntity, typename ObjectCache, typename ItemType>
class ObjectCacheCommon : public ObjectCacheBase {
public:
    using Base = ObjectCacheBase;
    using KeyedItem = Base::KeyedItem<Base::Item, KeyType>;

    using ItemKey = typename KeyedItem::ItemKey;
    using InterfaceKey = typename KeyedItem::InterfaceKey;
    using Item = ItemType;
    using ItemPtr = ItemType*;

    ObjectCacheCommon(uint64_t expiration) : Base(expiration, expiration / 16) {}
    ObjectCacheCommon(uint64_t expiration, uint64_t timer_cycle)
        : Base(expiration, timer_cycle) {}

    int ref_release(ItemPtr item, bool recycle = false) {
        return Base::ref_release(item, recycle);
    }

    int release(const InterfaceKey& key, bool recycle = false) {
        return Base::release(Item(key), recycle);
    }

    using iterator = typename ExpireContainerBase::TypedIterator<Item>;
    iterator begin() { return Base::begin(); }
    iterator end() { return Base::end(); }
    iterator find(const InterfaceKey& key) {
        return Base::find(KeyedItem(key));
    }
};

// Resource pool based on reference count
// when the pool is fulled, it will try to remove items which can be sure is not
// referenced the base m_list works as gc list when object acquired, construct
// or findout the object, add reference count; when object release, reduce
// refcount. if some resource is not referenced, it will be put back to gc list
// waiting to release.
template <typename KeyType, typename ValPtr>
class ObjectCache
    : public ObjectCacheCommon<
          KeyType, ValPtr, typename std::remove_pointer<ValPtr>::type,
          ObjectCache<KeyType, ValPtr>, ObjectCacheBase::PtrItem<KeyType, ValPtr>> {
protected:
    using Item = ObjectCacheBase::PtrItem<KeyType, ValPtr>;
    using Base = ObjectCacheBase;
    using Common = ObjectCacheCommon<
        KeyType, ValPtr, typename std::remove_pointer<ValPtr>::type,
        ObjectCache<KeyType, ValPtr>, Item>;
    using InterfaceKey = typename Item::InterfaceKey;
    using ItemPtr = Item*;
    using ValEntity = typename std::remove_pointer<ValPtr>::type;

public:
    using typename Common::iterator;

    using Common::Common;
    using Common::release;
    using Common::ref_release;
    using Common::begin;
    using Common::end;
    using Common::find;

    template <typename Constructor>
    ItemPtr ref_acquire(const InterfaceKey& key, const Constructor& ctor,
                        uint64_t failure_cooldown = 0) {
        auto _ctor = [&](void* item) {
            ((Item*)item)->_obj = ctor();
        };
        // _ctor can always implicit cast to `Delegate<void*>`
        return (ItemPtr)Base::ref_acquire(Item(key), _ctor, failure_cooldown);
    }

    template <typename Constructor>
    ValPtr acquire(const InterfaceKey& key, const Constructor& ctor,
                   uint64_t failure_cooldown = 0) {
        auto item = ref_acquire(key, ctor, failure_cooldown);
        return (ValPtr)(item ? item->_obj : nullptr);
    }

    class Borrow: public Base::Borrow<Common, ItemPtr, ValEntity, ValPtr> {
    public:
        using Base::Borrow<Common, ItemPtr, ValEntity, ValPtr>::Borrow;
        ValEntity& operator*() { return *Borrow::get_ptr(); }
        ValPtr operator->() { return Borrow::get_ptr(); }  
    };

    template <typename Constructor>
    Borrow borrow(const InterfaceKey& key, const Constructor& ctor,
                  uint64_t failure_cooldown = 0) {
        return Borrow(this, ref_acquire(key, ctor, failure_cooldown), false);
    }

    Borrow borrow(const InterfaceKey& key) {
        return borrow(key, [] { return new ValEntity(); });
    }
};

template <typename KeyType, typename NodeType>
class ObjectCache<KeyType, intrusive_list<NodeType>>
    : public ObjectCacheCommon<KeyType, intrusive_list<NodeType>*,
                               intrusive_list<NodeType>,
                               ObjectCache<KeyType, intrusive_list<NodeType>>,
                               ObjectCacheBase::ListItem<KeyType, intrusive_list<NodeType>>> {
protected:
    using ListType = intrusive_list<NodeType>;
    using Item = ObjectCacheBase::ListItem<KeyType, ListType>;
    using Base = ObjectCacheBase;
    using Common = ObjectCacheCommon<KeyType, intrusive_list<NodeType>*,
                               intrusive_list<NodeType>,
                               ObjectCache<KeyType, intrusive_list<NodeType>>,
                               ObjectCacheBase::ListItem<KeyType, intrusive_list<NodeType>>>;
    using InterfaceKey = typename Item::InterfaceKey;
    using ItemPtr = Item*;

public:
    using typename Common::iterator;

    using Common::Common;
    using Common::release;
    using Common::ref_release;
    using Common::begin;
    using Common::end;
    using Common::find;

    template <typename Constructor>
    ItemPtr ref_acquire(const InterfaceKey& key, const Constructor& ctor,
                        uint64_t failure_cooldown = 0) {
        auto _ctor = [&](void* item) {
            ((Item*)item)->_list = ctor();
            // always not nullptr
            ((Item*)item)->_obj = item;
        };
        return (ItemPtr)ObjectCacheBase::ref_acquire(Item(key), _ctor, failure_cooldown);
    }

    template <typename Constructor>
    ListType& acquire(const InterfaceKey& key, const Constructor& ctor,
                   uint64_t failure_cooldown = 0) {
        auto item = ref_acquire(key, ctor, failure_cooldown);
        assert(item);
        return item->_list;
    }

    class Borrow: public Base::Borrow<Common, ItemPtr, ListType, ListType*> {
    public:
        using Base::Borrow<Common, ItemPtr, ListType, ListType*>::Borrow;
        ListType& operator*() { return *this->_ref->_list; }
        ListType* operator->() { return &this->_ref->_list; }  
    };

    template <typename Constructor>
    Borrow borrow(const InterfaceKey& key, const Constructor& ctor,
                  uint64_t failure_cooldown = 0) {
        return Borrow(this, ref_acquire(key, ctor, failure_cooldown), false);
    }

    Borrow borrow(const InterfaceKey& key) {
        return borrow(key, [] { return ListType(); });
    }
};
