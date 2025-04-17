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
#include <cassert>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include <algorithm>

template < class Key,                        // map::key_type
           class T,                          // map::mapped_type
           class Compare = std::less<Key>,   // map::key_compare
           class Alloc = std::allocator<
                std::pair<Key, T> > >       // map::allocator_type
class consistent_hash_map
{
public:
    using key_type          = Key;
    using mapped_type       = T;
    using value_type        = std::pair<Key, T>;
    using key_compare       = Compare;
    using allocator_type    = Alloc;

    struct value_compare
    {
        key_compare m_comp;
        value_compare(const key_compare& comp) : m_comp(comp) { }
        bool operator()(const value_type& a, const value_type& b) const
        {
            return Compare()(a.first, b.first);
        }
        bool operator()(const value_type& a, const key_type& b) const
        {
            return Compare()(a.first, b);
        }
    };

    explicit consistent_hash_map(
        const key_compare& comp = key_compare(),
        const allocator_type& alloc = allocator_type()) :
            m_comp(comp), m_vector(alloc) { }

    using reference         = mapped_type&;
    using const_reference   = const mapped_type&;
    using pointer           = mapped_type*;
    using const_pointer     = const mapped_type*;
    using container_type    = std::vector<value_type, allocator_type>;
    using iterator          = typename container_type::iterator;
    using const_iterator    = typename container_type::const_iterator;
    using reverse_iterator  = typename container_type::reverse_iterator;
    using const_reverse_iterator = typename container_type::const_reverse_iterator;
    using difference_type   = typename container_type::difference_type;
    using size_type         = typename container_type::size_type;

    void sort()
    {
        std::sort(m_vector.begin(), m_vector.end(), value_compare(m_comp));
        auto it = std::unique(m_vector.begin(), m_vector.end());
        m_vector.erase(it, m_vector.end());
    }

    void clear()
    {
        m_vector.clear();
    }

    void push_back(const key_type& k, const mapped_type& v)
    {
        m_vector.push_back({k, v});
    }

    template<typename IT>
    void assign(IT begin, IT end, bool sorted = false)
    {
        m_vector.assign(begin, end);
        if (!sorted) sort();
    }

    void assign(std::initializer_list<value_type> ilist, bool sorted = false)
    {
        m_vector.assign(ilist);
        if (!sorted) sort();
    }

    iterator begin()  { return m_vector.begin(); }
    iterator end()    { return m_vector.end(); }
    iterator rbegin() { return m_vector.rbegin(); }
    iterator rend()   { return m_vector.rend(); }

    const_iterator begin() const  { return m_vector.begin(); }
    const_iterator end() const    { return m_vector.end(); }
    const_iterator cbegin() const { return m_vector.cbegin(); }
    const_iterator cend() const   { return m_vector.cend(); }

    const_reverse_iterator crbegin() const { return m_vector.crbegin(); }
    const_reverse_iterator crend() const   { return m_vector.crend(); }

    bool empty() const         { return m_vector.empty(); }
    size_type size() const     { return m_vector.size(); }
    size_type max_size() const { return m_vector.max_size(); }

    iterator find(const key_type& k)
    {
        if (empty()) return end();
        auto it = std::lower_bound(begin(), end(), k, value_compare(m_comp));
        return (it != end()) ? it : begin();
    }

    const_iterator find(const key_type& k) const
    {
        return const_cast<consistent_hash_map*>(this)->find(k);
    }

    iterator next(iterator it)
    {
        return (++it != end()) ? it : begin();
    }

    const_iterator next(const_iterator it) const
    {
        return (++it != cend()) ? it : cbegin();
    }

    size_type count(const key_type& k) const
    {
        return empty() ? 0 : 1;
    }

    value_type& at(const key_type& k)
    {
        assert(!empty());
        return *find(k);
    }

    const value_type& at(const key_type& k) const
    {
        assert(!empty());
        return *find(k);
    }

    value_type& operator[](const key_type& k)
    {
        return at(k);
    }

    const value_type& operator[](const key_type& k) const
    {
        return at(k);
    }

protected:
    key_compare m_comp;
    container_type m_vector;
};
