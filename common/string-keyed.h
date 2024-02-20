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

#include <photon/common/string_view.h>
#include <cstring>
#include <unordered_map>
#include <map>

// string_key is a string_view with dedicated storage,
// primarily used as stored keys in std::map and std::unordered_map,
// so as to accept string_view as query keys.
class string_key : public std::string_view {
public:
    string_key() : std::string_view(0, 0) { }
    explicit string_key(const string_key& rhs) {
        auto n = rhs.size();
        auto ptr = (char*)malloc(n + 1);
        do_copy(ptr, rhs);
        assign({ptr, n});
    }
    explicit string_key(string_key&& rhs) {
        assign(rhs);
        rhs.assign({0, 0});
    }
    ~string_key() {
        free((void*)begin());
    }

    string_key& operator = (const string_key& rhs) = delete;
    string_key& operator = (string_key&& rhs) = delete;

protected:
    void do_copy(char* ptr, std::string_view s) {
        memcpy(ptr, s.data(), s.size());
        ptr[s.size()] = '\0';
    }
    void assign(std::string_view sv) {
        *(std::string_view*)this = sv;
    }
};

// The basic class for maps with string keys, aims to avoid temp
// std::string construction in queries, by accepting string_views.
// M is the underlying map, either std::map or std::unordered_map
template <class M>
class basic_map_string_key : public M
{
public:
    using base = M;
    using typename base::const_iterator;
    using typename base::iterator;
    using typename base::mapped_type;
    using typename base::size_type;
    using base::base;

    using base::erase;

    using key_type = std::string_view;
    using value_type = std::pair<const key_type, mapped_type>;

    mapped_type& operator[] ( const key_type& k )
    {
        return base::operator[]((const string_key&)k);
    }
    mapped_type& at ( const key_type& k )
    {
        return base::at((const string_key&)k);
    }
    const mapped_type& at ( const key_type& k ) const
    {
        return base::at((const string_key&)k);
    }
    iterator find ( const key_type& k )
    {
        return base::find((const string_key&)k);
    }
    const_iterator find ( const key_type& k ) const
    {
        return base::find((const string_key&)k);
    }
    size_type count ( const key_type& k ) const
    {
        return base::count((const string_key&)k);
    }
    std::pair<iterator,iterator> equal_range ( const key_type& k )
    {
        return base::equal_range((const string_key&)k);
    }
    std::pair<const_iterator,const_iterator> equal_range ( const key_type& k ) const
    {
        return base::equal_range((const string_key&)k);
    }
    template <class... Args>
    std::pair<iterator, bool> emplace (const key_type& k, Args&&... args )
    {
        return base::emplace((const string_key&)k, std::forward<Args>(args)...);
    }
    template <class... Args>
    iterator emplace_hint ( const_iterator position, const key_type& k, Args&&... args )
    {
        return base::emplace_hint(position, (const string_key&)k, std::forward<Args>(args)...);
    }
    std::pair<iterator,bool> insert ( const value_type& k )
    {
        return emplace(k.first, k.second);
    }
    template <class P>
    std::pair<iterator,bool> insert ( P&& val )
    {
        return emplace(val.first, std::move(val.second));
    }
    iterator insert ( const_iterator hint, const value_type& val )
    {
        return emplace_hint(hint, val.first, val.second);
    }
    template <class P>
    iterator insert ( const_iterator hint, P&& val )
    {
        return emplace_hint(hint, val.first, std::move(val.second));
    }
    template <class InputIterator>
    void insert ( InputIterator first, InputIterator last )
    {
        for (auto it = first; it != last; ++it)
            insert(*it);
    }
    void insert ( std::initializer_list<value_type> il )
    {
        insert(il.begin(), il.end());
    }
    size_type erase ( const std::string_view& k )
    {
        return base::erase((const string_key&)k);
    }
};

template<class T,
    class Hasher = std::hash<std::string_view>,
    class KeyEqual = std::equal_to<std::string_view>,
    class Alloc = std::allocator<std::pair<const string_key, T>>>
using unordered_map_string_key = basic_map_string_key<
    std::unordered_map<string_key, T, Hasher, KeyEqual, Alloc>>;

template<class T,
    class Pred = std::less<string_key>,
    class Alloc = std::allocator<std::pair<const string_key,T>>>
class map_string_key : public basic_map_string_key<
    std::map<string_key, T, Pred, Alloc>>
{
public:
    using base = basic_map_string_key<std::map<string_key, T, Pred, Alloc>>;
    using typename base::key_type;
    using typename base::const_iterator;
    using typename base::iterator;
    using typename base::mapped_type;
    using typename base::size_type;
    using base::base;

    iterator lower_bound (const key_type& k)
    {
        return base::lower_bound((const string_key&)k);
    }
    const_iterator lower_bound (const key_type& k) const
    {
        return base::lower_bound((const string_key&)k);
    }
    iterator upper_bound (const key_type& k)
    {
        return base::upper_bound((const string_key&)k);
    }
    const_iterator upper_bound (const key_type& k) const
    {
        return base::upper_bound((const string_key&)k);
    }
};

// the String Key-Value (Mutable), stored together
// in a consecutive area, so as to save one allocation
class skvm : public string_key {
public:
    explicit skvm(std::string_view k, std::string_view v) {
        auto nk = k.size();
        auto nv = v.size();
        auto ptr = (char*) malloc(nk+1 + nv+1);
        do_copy(ptr, k);
        do_copy(ptr + nk+1, v);
        assign({ptr, nk});
    }
    const char* get_value() const {
        return this->end() + 1;
    }
    void replace_value(std::string_view v) {
        auto nk = this->size();
        auto ptr = (char*) realloc((void*)this->data(), nk+1 + v.size()+1);
        do_copy(ptr + nk+1, v);
        assign({ptr, nk});
    }
};

template <class M>
class basic_map_string_kv : public M
{
public:
    using base = M;
    using base::base;
    using key_type = std::string_view;
    using mapped_type = std::string_view;
    using typename base::size_type;
    using base::erase;

    using value_type = std::pair<const key_type, mapped_type>;

    struct MutableValue {
        std::string_view _v;
        skvm* _skvm;
        operator std::string_view() const {
            return _v;
        }
        MutableValue& operator = (std::string_view v) {
            _skvm->replace_value(v);
            _v = {_skvm->get_value(), v.size()};
        }
    };

    struct iterator {
        using base_it = typename base::iterator;
        base_it _b_it;

        using mutable_value_type = std::pair<std::string, MutableValue>;
        mutable_value_type _val;
        iterator(base_it b_it) : _b_it(b_it) { }
        void _init() {
            _val.first = _b_it->first;
            _val.second = {_b_it->first.end() + 1, _b_it->second};
        }
        const mutable_value_type* operator->() const {
            return &_val;
        }
        const mutable_value_type& operator*() const {
            return _val;
        }
        mutable_value_type* operator->() {
            return &_val;
        }
        mutable_value_type& operator*() {
            return _val;
        }
        iterator operator++(int) {
            auto temp = *this;
            ++*this;
            return temp;
        }
        iterator& operator++() {
            ++_b_it;
            _init();
            return *this;
        }
        iterator operator--(int) {
            auto temp = *this;
            --*this;
            return temp;
        }
        iterator& operator--() {
            --_b_it;
            _init();
            return *this;
        }
        bool operator==(const iterator& rhs) const {
            return this->_b_it == rhs._b_it;
        }
        bool operator!=(const iterator& rhs) const {
            return !(*this == rhs);
        }
    };

    using const_iterator = const iterator;

    mapped_type operator[] ( const key_type& k ) const
    {
        return find(k)->second;
    }
    MutableValue operator[] ( const key_type& k )
    {
        return find(k)->second;
    }
    mapped_type at ( const key_type& k ) const
    {
        return find(k)->second;
    }
    MutableValue at ( const key_type& k )
    {
        return find(k)->second;
    }
    const_iterator find ( const key_type& k ) const
    {
        return base::find((const skvm&)k);
    }
    iterator find ( const key_type& k )
    {
        return base::find((const skvm&)k);
    }
    size_type count ( const key_type& k ) const
    {
        return base::count((const skvm&)k);
    }
    std::pair<const_iterator,const_iterator> equal_range ( const key_type& k ) const
    {
        return base::equal_range((const skvm&)k);
    }
    std::pair<iterator, bool> emplace (const key_type& k, const mapped_type& v )
    {
        return base::emplace({k, v}, v.size());
    }
    iterator emplace_hint ( const_iterator position, const key_type& k, const mapped_type& v )
    {
        return base::emplace_hint(position, {k, v}, v.size());
    }
    std::pair<iterator,bool> insert ( const value_type& k )
    {
        return emplace(k.first, k.second);
    }
    iterator insert ( const_iterator hint, const value_type& val )
    {
        return emplace_hint(hint, val.first, val.second);
    }
    template <class InputIterator>
    void insert ( InputIterator first, InputIterator last )
    {
        for (auto it = first; it != last; ++it)
            insert(*it);
    }
    void insert ( std::initializer_list<value_type> il )
    {
        insert(il.begin(), il.end());
    }
    size_type erase ( const std::string_view& k )
    {
        return base::erase((const skvm&)k);
    }
};

template<class Hasher = std::hash<std::string_view>,
         class KeyEqual = std::equal_to<std::string_view>,
         class Alloc = std::allocator<std::pair<const skvm, size_t>>>
using unordered_map_string_kv = basic_map_string_kv<
    std::unordered_map<skvm, size_t, Hasher, KeyEqual, Alloc>>;

template<class Pred = std::less<skvm>,
         class Alloc = std::allocator<std::pair<const skvm, size_t>>>
class map_string_kv : public basic_map_string_kv<
    std::map<skvm, size_t, Pred, Alloc>>
{
public:
    using base = basic_map_string_kv<std::map<skvm, size_t, Pred, Alloc>>;
    using typename base::key_type;
    using typename base::const_iterator;
    using typename base::iterator;
    using typename base::mapped_type;
    using typename base::size_type;
    using base::base;

    iterator lower_bound (const key_type& k)
    {
        return base::lower_bound((const skvm&)k);
    }
    const_iterator lower_bound (const key_type& k) const
    {
        return base::lower_bound((const skvm&)k);
    }
    iterator upper_bound (const key_type& k)
    {
        return base::upper_bound((const skvm&)k);
    }
    const_iterator upper_bound (const key_type& k) const
    {
        return base::upper_bound((const skvm&)k);
    }
};

