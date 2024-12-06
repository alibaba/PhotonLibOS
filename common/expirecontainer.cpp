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

#include "expirecontainer.h"

#include <photon/thread/thread.h>

ExpireContainerBase::ExpireContainerBase(uint64_t expiration,
                                         uint64_t timer_cycle)
    : _expiration(expiration),
      _timer(std::max(static_cast<uint64_t>(1000), timer_cycle),
             {this, &ExpireContainerBase::expire}, true, 8UL * 1024 * 1024) {}

std::pair<ExpireContainerBase::iterator, bool> ExpireContainerBase::insert(
    Item* item) {
    return _set.emplace(item);
}

ExpireContainerBase::iterator ExpireContainerBase::__find_prelock(
    const Item& key_item) {
    auto it = _set.find((Item*)&key_item);
    return it;
}

ExpireContainerBase::iterator ExpireContainerBase::find(const Item& key_item) {
    SCOPED_LOCK(_lock);
    return __find_prelock(key_item);
}

void ExpireContainerBase::clear() {
    for (auto x : ({
             SCOPED_LOCK(_lock);
             _list.node = nullptr;
             Set(std::move(_set));
         })) {
        delete x;
    }
}

uint64_t ExpireContainerBase::expire() {
    ({
        SCOPED_LOCK(_lock);
        _list.split_by_predicate([&](Item* x) {
            bool ret = x->_timeout.expire() < photon::now;
            if (ret) _set.erase(x);
            return ret;
        });
    }).delete_all();
    return 0;
}

bool ExpireContainerBase::keep_alive(const Item& x, bool insert_if_not_exists) {
    DEFER(expire());
    SCOPED_LOCK(_lock);
    auto it = __find_prelock(x);
    if (it == _set.end() && insert_if_not_exists) {
        auto ptr = x.construct();
        auto pr = insert(ptr);
        if (!pr.second) delete ptr;
        it = pr.first;
    }
    if (it == _set.end()) return false;
    enqueue(*it);
    return true;
}

ObjectCacheBase::Item* ObjectCacheBase::ref_acquire(const Item& key_item,
                                                    Delegate<void*> ctor,
                                                    uint64_t failure_cooldown) {
    Base::iterator holder;
    Item* item = nullptr;
    expire();
    do {
        SCOPED_LOCK(_lock);
        holder = Base::__find_prelock(key_item);
        if (holder == Base::end()) {
            auto x = key_item.construct();
            auto pr = insert(x);
            if (!pr.second) delete x;
            holder = pr.first;
        }
        _list.pop(*holder);
        item = (ItemPtr)*holder;
        if (item->_recycle) {
            holder = end();
            item = nullptr;
            blocker.wait(_lock);
        } else {
            item->_refcnt++;
        }
    } while (!item);
    {
        SCOPED_LOCK(item->_mtx);
        if (!item->_obj && (item->_failure <=
                            photon::sat_sub(photon::now, failure_cooldown))) {
            item->_obj = ctor();
            if (!item->_obj) item->_failure = photon::now;
        }
    }
    if (!item->_obj) {
        ref_release(item, false);
        return nullptr;
    }
    return item;
}

int ObjectCacheBase::ref_release(ItemPtr item, bool recycle) {
    DEFER(expire());
    photon::semaphore sem;
    {
        SCOPED_LOCK(_lock);
        if (item->_recycle) recycle = false;
        if (recycle) {
            item->_recycle = &sem;
        }
        item->_refcnt--;
        if (item->_refcnt == 0) {
            if (item->_recycle) {
                item->_recycle->signal(1);
            } else {
                item->_failure = 0;
                enqueue(item);
            }
        }
    }
    if (recycle) {
        sem.wait(1);
        {
            SCOPED_LOCK(_lock);
            assert(item->_refcnt == 0);
            _set.erase(item);
        }
        delete item;
        blocker.notify_all();
    }
    return 0;
}

// the argument `key` plays the roles of (type-erased) key
int ObjectCacheBase::release(const ObjectCacheBase::Item& key_item,
                             bool recycle) {
    auto item = find(key_item);
    if (item == end()) return -1;
    return ref_release(*item, recycle);
}
