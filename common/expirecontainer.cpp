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
                                         uint64_t recycle_timeout)
    : _expiration(expiration),
      _timer(recycle_timeout, {this, &ExpireContainerBase::expire}, true,
             8UL * 1024 * 1024) {}

std::pair<ExpireContainerBase::iterator, bool> ExpireContainerBase::insert(
    Item* item) {
    return _set.emplace(item);
}

ExpireContainerBase::iterator ExpireContainerBase::find(const Item& key_item) {
    ItemPtr tp((Item*)&key_item);
    auto it = _set.find(tp);
    (void)tp.release();
    return it;
}

void ExpireContainerBase::clear() {
    _set.clear();
    _list.node = nullptr;
}

uint64_t ExpireContainerBase::expire() {
    photon::scoped_lock lock(_mtx);
    while (!_list.empty() && _list.front()->_timeout.expire() < photon::now) {
        auto p = _list.pop_front();
        ItemPtr ptr(p);
        _set.erase(ptr);
        (void)ptr.release();  // p has been deleted inside erase()
    }
    return 0;
}

bool ExpireContainerBase::keep_alive(const Item& x, bool insert_if_not_exists) {
    DEFER(expire());
    photon::scoped_lock lock(_mtx);
    auto it = find(x);
    if (it == _set.end() && insert_if_not_exists) {
        auto ptr = x.construct();
        auto pr = insert(ptr);
        if (!pr.second) delete ptr;
        it = pr.first;
    }
    if (it == _set.end()) return false;
    enqueue(it->get());
    return true;
}

ObjectCacheBase::Item* ObjectCacheBase::ref_acquire(const Item& key_item,
                                                    Delegate<void*> ctor) {
    Base::iterator holder;
    Item* item = nullptr;
    do {
        photon::scoped_lock lock(_mtx);
        holder = Base::find(key_item);
        if (holder == Base::end()) {
            auto x = key_item.construct();
            auto pr = insert(x);
            if (!pr.second) delete x;
            holder = pr.first;
        }
        _list.pop(holder->get());
        item = (Item*)holder->get();
        if (item->_recycle) {
            holder = end();
            item = nullptr;
            blocker.wait(lock);
        } else {
            item->_refcnt++;
        }
    } while (!item);
    {
        photon::scoped_lock lock(item->_mtx);
        if (!item->_obj) {
            item->_obj = ctor();
        }
    }
    if (!item->_obj) {
        ref_release(item, false);
        return nullptr;
    }
    return item;
}

int ObjectCacheBase::ref_release(ObjectCacheBase::Item* item, bool recycle) {
    photon::semaphore sem;
    {
        photon::scoped_lock lock(_mtx);
        if (item->_recycle) recycle = false;
        if (recycle) {
            item->_recycle = &sem;
        }
        item->_refcnt--;
        if (item->_refcnt == 0) {
            if (item->_recycle) {
                item->_recycle->signal(1);
            } else {
                enqueue(item);
            }
        }
    }
    if (recycle) {
        sem.wait(1);
        {
            photon::scoped_lock lock(_mtx);
            assert(item->_refcnt == 0);
            Base::ItemPtr ptr(item);
            _set.erase(ptr);
            (void)ptr.release();
        }
        blocker.notify_all();
    }
    return 0;
}

// the argument `key` plays the roles of (type-erased) key
int ObjectCacheBase::release(const ObjectCacheBase::Item& key_item,
                             bool recycle) {
    auto item = find(key_item);
    if (item == end()) return -1;
    return ref_release(item->get(), recycle);
}
