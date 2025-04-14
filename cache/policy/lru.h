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
#include <stddef.h>
#include <assert.h>
#include <inttypes.h>
#include <type_traits>
#include <vector>
#include <utility>
#include <limits>

namespace photon {
namespace fs{
// This is a generic LRU container, highly optimized for both speed and memory.
// The default KeyType = uint16_t, which limits the container to 64K entries as most.
// If more entries are needed, use KeyType = uint32_t or bigger.
template <typename ValueType, typename KeyType = uint16_t>
class LRU {
public:
    using value_type = ValueType;
    using key_type = KeyType;
    static_assert(std::is_unsigned<KeyType>::value, "KeyType must be unsigned integer");

    // maximum # of entries in the LRU container
    const static size_t LIMIT = std::numeric_limits<key_type>::max();

    const static uint64_t kInvalid = std::numeric_limits<uint64_t>::max();

    LRU() {
        // This is a dummy node for denoting end of list.
        m_head = do_alloc();
        PTR(m_head)->prev = m_head;
        PTR(m_head)->next = m_head;
    }

    // Insert a value into the LRU container, returning a key that is guaranteed
    // not to change during the lifetime, and that can be used as parameter of use().
    // MUST ensure # entries < LIMIT, before pushing!
    key_type push_front(value_type v) {
        assert(m_size < LIMIT);
        auto i = do_alloc();
        PTR(i)->val = v;
        do_insert(PTR(m_head)->prev, m_head, i);
        m_size++;
        return m_head = i;
    }
    void access(key_type i) {
        assert(i < m_array.size());
        if (m_size == 1 || i == m_head)
            return;
        do_remove(i);
        do_insert(PTR(m_head)->prev, m_head, i);
        m_head = i;
    }
    void mark_key_cleared(key_type i)    // mark `i` as cleared (all space de-allocated),
    {                                    // by removing it from the ring but not inserting
        assert(i < m_array.size());      // it to the free ring, so as to avoid it being
        do_remove(i);                    // returned by back() and considered as a
        PTR(i)->prev = PTR(i)->next = i; // candidate for eviction.
    }                                    // `use()` and `remove()` apply as usual
    void remove(key_type i) {
        assert(i < m_array.size());
        do_remove(i);
        m_size--;
        if (m_free == kInvalid) {
            PTR(i)->prev = PTR(i)->next = m_free = i;
        } else {
            do_insert(m_free, PTR(m_free)->next, i);
        }
    }
    void pop_back() {
        assert(m_size > 0);
        remove(PTR(PTR(m_head)->prev)->prev);
    }
    value_type &front() {
        assert(m_size > 0);
        return PTR(m_head)->val;
    }
    value_type &back() {
        assert(m_size > 0);
        return PTR(PTR(PTR(m_head)->prev)->prev)->val;
    }
    size_t size() {
        return m_size;
    }
    bool empty() {
        return m_head == PTR(m_head)->prev;
    }

protected:
    struct Record {
        key_type prev, next;
        value_type val;
    };
    std::vector<Record> m_array;
    uint64_t m_free = kInvalid;
    uint64_t m_size = 0; // # of valid records (excluding free)
    key_type m_head;

    Record *PTR(key_type i) {
        return &m_array[i];
    }
    void do_insert(key_type prev, key_type next, key_type i) {
        PTR(i)->prev = prev;
        PTR(i)->next = next;
        PTR(prev)->next = PTR(next)->prev = i;
    }
    void do_remove(key_type i) {
        auto prev = PTR(i)->prev;
        auto next = PTR(i)->next;
        if (i == m_head) {
            m_head = next;
        }
        PTR(prev)->next = next;
        PTR(next)->prev = prev;
    }
    key_type do_alloc() {
        if (m_free != kInvalid) {
            auto r = m_free;
            if (PTR(r)->next == r || PTR(r)->prev == r) {
                m_free = kInvalid;
            } else {
                m_free = PTR(r)->next;
                do_remove(r);
            }
            return r;
        } else {
            auto r = m_array.size();
            assert(r < LIMIT);
            m_array.resize(r + 1);
            return (key_type)r;
        }
    }
};
}
} // namespace photon::fs