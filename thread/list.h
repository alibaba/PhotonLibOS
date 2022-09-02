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
#include <photon/common/utility.h>

class __intrusive_list_node
{
public:
    __intrusive_list_node* __prev_ptr;
    __intrusive_list_node* __next_ptr;
    __intrusive_list_node()
    {
        __prev_ptr = __next_ptr = this;
    }
    bool single()
    {
        return __prev_ptr == this || __next_ptr == this;
    }
    __intrusive_list_node* remove_from_list()
    {
        if (single())
            return nullptr;

        __next_ptr->__prev_ptr = __prev_ptr;
        __prev_ptr->__next_ptr = __next_ptr;
        auto next = __next_ptr;
        __prev_ptr = __next_ptr = this;
        return next;
    }
    void insert_before(__intrusive_list_node* ptr)
    {
        ptr->insert_between(__prev_ptr, this);
    }
    void insert_tail(__intrusive_list_node* ptr)
    {
        insert_before(ptr);
    }
    void insert_after(__intrusive_list_node* ptr)
    {
        ptr->insert_between(this, __next_ptr);
    }
    void insert_list_before(__intrusive_list_node* ptr)
    {
        ptr->insert_list_between(__prev_ptr, this);
    }
    void insert_list_tail(__intrusive_list_node* ptr)
    {
        insert_list_before(ptr);
    }
    void insert_list_after(__intrusive_list_node* ptr)
    {
        ptr->insert_list_between(this, __next_ptr);
    }
    // split into two list ring, one starts from this and another starts from ptr
    // this n1 n2 n3 ... np ptr nn ... nX
    // ==> this n1 n2 n3 ... np
    // ==> ptr nn ... nx
    void split(__intrusive_list_node* ptr)
    {
        assert(ptr != this);
        assert(!single());
        auto front_head = this;
        auto front_tail = ptr->__prev_ptr;
        auto back_head = ptr;
        auto back_tail = this->__prev_ptr;

        front_head->__prev_ptr = front_tail;
        front_tail->__next_ptr = front_head;

        back_head->__prev_ptr = back_tail;
        back_tail->__next_ptr = back_head;
    }

private:
    void insert_between(__intrusive_list_node* prev, __intrusive_list_node* next)
    {
        if (!single()) return;
        prev->__next_ptr = this;
        next->__prev_ptr = this;
        __prev_ptr = prev;
        __next_ptr = next;
    }
    void insert_list_between(__intrusive_list_node* prev, __intrusive_list_node* next)
    {
        auto a = this;
        auto b = __prev_ptr;
        prev->__next_ptr = a;
        a->__prev_ptr = prev;
        next->__prev_ptr = b;
        b->__next_ptr = next;
    }
};

template<typename T>
class intrusive_list_node : public __intrusive_list_node
{
public:
    T* remove_from_list()
    {
        auto ret = __intrusive_list_node::remove_from_list();
        return static_cast<T*>(ret);
    }
    void insert_before(T* ptr)
    {
        __intrusive_list_node::insert_before(ptr);
    }
    void insert_tail(T* ptr)
    {
        __intrusive_list_node::insert_tail(ptr);
    }
    void insert_after(T* ptr)
    {
        __intrusive_list_node::insert_after(ptr);
    }

    void insert_list_before(T* ptr)
    {
        __intrusive_list_node::insert_list_before(ptr);
    }
    void insert_list_tail(T* ptr)
    {
        __intrusive_list_node::insert_list_tail(ptr);
    }
    void insert_list_after(T* ptr)
    {
        __intrusive_list_node::insert_list_after(ptr);
    }

    void insert_before(T& ptr)
    {
        __intrusive_list_node::insert_before(&ptr);
    }
    void insert_tail(T& ptr)
    {
        __intrusive_list_node::insert_tail(&ptr);
    }
    void insert_after(T& ptr)
    {
        __intrusive_list_node::insert_after(&ptr);
    }

    void insert_list_before(T& ptr)
    {
        __intrusive_list_node::insert_list_before(&ptr);
    }
    void insert_list_tail(T& ptr)
    {
        __intrusive_list_node::insert_list_tail(&ptr);
    }
    void insert_list_after(T& ptr)
    {
        __intrusive_list_node::insert_list_after(&ptr);
    }
    void split(T* ptr)
    {
        __intrusive_list_node::split(ptr);
    }

    T* next()
    {
        return static_cast<T*>(__intrusive_list_node::__next_ptr);
    }
    T* prev()
    {
        return static_cast<T*>(__intrusive_list_node::__prev_ptr);
    }

    struct iterator
    {
        __intrusive_list_node* ptr;
        __intrusive_list_node* end;
        T* operator*()
        {
            return static_cast<T*>(ptr);
        }
        iterator& operator++()
        {
            ptr = ptr->__next_ptr;
            if (ptr == end)
                ptr = nullptr;
            return *this;
        }
        iterator operator++(int)
        {
            auto rst = *this;
            ptr = ptr->__next_ptr;
            if (ptr == end)
                ptr = nullptr;
            return rst;
        }
        bool operator == (const iterator& rhs) const
        {
            return ptr == rhs.ptr;
        }
        bool operator != (const iterator& rhs) const
        {
            return  !(*this == rhs);
        }
    };

    iterator begin()
    {
        return {this, this};
    }
    iterator end()
    {
        return {nullptr, nullptr};
    }
};


template<typename NodeType>
class intrusive_list
{
public:
    NodeType* node = nullptr;
    intrusive_list() = default;
    explicit intrusive_list(NodeType* node): node(node) {}
    ~intrusive_list()
    {   // node (NodeType*) MUST be intrusive_list_node<T>, which
        // should be implicitly convertible to __intrusive_list_node*
        __intrusive_list_node* __node = node;
        assert(__node == nullptr);
        _unused(__node);
    }
    void push_back(NodeType* ptr)
    {
        if (!node) {
            node = ptr;
        } else {
            node->insert_tail(ptr);
        }
    }
    void push_back(intrusive_list&& lst)
    {
        if (!node) {
            node = lst.node;
        } else {
            node->insert_list_tail(lst.node);
        }
        lst.node = nullptr;
    }
    void push_front(NodeType* ptr)
    {
        if (node) {
            node->insert_before(ptr);
        }
        node = ptr;
    }
    void push_front(intrusive_list&& lst)
    {
        if (!node) {
            node = lst.node;
        } else {
            node->insert_list_front(lst);
        }
        lst.node = nullptr;
    }
    NodeType* pop_front()
    {
        if (!node)
            return nullptr;

        auto rst = node;
        node = node->remove_from_list();
        return rst;
    }
    NodeType* pop_back()
    {
        if (!node)
            return nullptr;

        auto rst = node->prev();
        if (rst == node) {
            node = nullptr;
        } else {
            rst->remove_from_list();
        }
        return rst;
    }
    NodeType* erase(NodeType* ptr)
    {
        auto nx = ptr->remove_from_list();
        if (ptr == node)
            node = nx;
        return nx;
    }
    void pop(NodeType* ptr)
    {
        erase(ptr);
    }
    NodeType* front()
    {
        return node;
    }
    NodeType* back()
    {
        return node ? node->prev() : nullptr;
    }
    operator bool()
    {
        return node;
    }
    bool empty()
    {
        return node == nullptr;
    }
    typedef typename NodeType::iterator iterator;
    iterator begin()
    {
        return node ? node->begin() : end();
    }
    iterator end()
    {
        return {nullptr, nullptr};
    }
    intrusive_list split_front_inclusive(NodeType* ptr)
    {
        auto ret = node;
        if (!node || !ptr) return intrusive_list();
        if (ptr->__next_ptr == node) {
            // all elements are splitted
            node = nullptr;
            return intrusive_list(ret);
        }
        auto rest_head = ptr->next();
        node->split(rest_head);
        node = rest_head;
        return intrusive_list(ret);
    }
    intrusive_list split_front_exclusive(NodeType* ptr)
    {
        auto ret = node;
        if (!ptr) {
            // all elements are splitted
            node = nullptr;
            return intrusive_list(ret);
        }
        if (!node || ptr == node) {
            return intrusive_list();
        }
        node->split(ptr);
        node = ptr;
        return intrusive_list(ret);
    }
    template<typename Predicate>
    intrusive_list split_by_predicate(Predicate&& pred)
    {
        if (!node) {
            return intrusive_list();
        }
        NodeType* first_not_fit = nullptr;
        for (auto x : *this) {
            if (!pred(x)) {
                first_not_fit = x;
                break;
            }
        }
        return split_front_exclusive(first_not_fit);
    }
    void delete_all()
    {
        auto ptr = node;
        if (ptr) {
            do {
                auto next = ptr->next();
                delete ptr;
                ptr = next;
            } while (ptr != node);
        }
        node = nullptr;
    }
};

