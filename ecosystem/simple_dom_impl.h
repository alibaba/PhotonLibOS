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
#include <inttypes.h>
#include <assert.h>
#include <memory>
#include <math.h>
#include <atomic>
#include <photon/common/object.h>
#include <photon/common/estring.h>
#include <photon/common/enumerable.h>
#include <photon/common/stream.h>


namespace photon {

namespace SimpleDOM {

using str = estring_view;


// the interface for internal implementations
class NodeImpl : public Object {
protected:
    NodeImpl() = default;
    const static uint8_t FLAG_IS_ROOT       = 1;    // the root node
    const static uint8_t FLAG_IS_LAST       = 2;    // last sibling node
    const static uint8_t FLAG_EQUAL_KEY_LAST= 4;    // last node with same key
    const static uint8_t FLAG_TEXT_OWNERSHIP= 8;
    const static size_t  MAX_NODE_SIZE      = 256;
    const static size_t  MAX_NCHILDREN      = UINT16_MAX;
    const static size_t  MAX_KEY_OFFSET     = UINT32_MAX;
    const static size_t  MAX_KEY_LENGTH     = 4095;
    const static size_t  MAX_VALUE_OFFSET   = 4095;
    const static size_t  MAX_VALUE_LENGTH   = MAX_KEY_OFFSET;

union { struct  { // for non-root nodes
    struct {
        uint8_t _flags;
        uint16_t _k_len : 12;       // key length (12 bits)
        uint16_t _v_off : 12;       // value offset (12 bits) to key end
    }__attribute__((packed));
    const NodeImpl* _root;          // root node
};                                  // packed as 20 bytes
struct {         // for the root node
    uint8_t _flags_;                // the same as _flags
    uint8_t _node_size;             // sizeof(the node implementation)
    mutable uint16_t _refcnt;       // reference counter of the document
    const char* _text_begin;
}; };
    uint32_t _k_off;                // key offset to _text_begin
    uint32_t _v_len;                // value length
    uint32_t _nchildren;            // for all nodes

    using AT16 = std::atomic<uint16_t>;
    static_assert(sizeof(AT16) == sizeof(_refcnt), "...");

    void add_doc_ref() const {
        assert(is_root());
        auto refcnt = (AT16*)&_refcnt;
        ++*refcnt;
    }

    void del_doc_ref() const {
        assert(is_root());
        auto refcnt = (AT16*)&_refcnt;
        if (--*refcnt == 0)
            delete this;
    }

    friend class Node;

public:
    size_t num_children() const {
        return _nchildren;
    }

    // get the i-th child node
    // for an array object, it gets the i-th element (doc type determines the starting value)
    // for an object, it gets the i-th element in implementation defined order (same-key
    // nodes are garanteed adjacent)
    virtual const NodeImpl* get(size_t i) const __attribute__((pure)) = 0;

    // get the first child node with a specified `key`
    // XML attributes are treated as a special child node with key "__attributes__"
    virtual const NodeImpl* get(str key) const __attribute__((pure)) = 0;

    bool is_root() const {
        return _flags & FLAG_IS_ROOT;
    }
    const NodeImpl* get_root() const {
        return is_root() ? this : _root;
    }
    str get_key() const {
        return get_key(get_root()->_text_begin);
    }
    str get_value() const {
        return get_value(get_root()->_text_begin);
    }
    str get_key(const char* text_begin) const {
        return {text_begin + _k_off, _k_len};
    }
    str get_value(const char* text_begin) const {
        return {get_key(text_begin).end() + _v_off, _v_len};
    }
    bool has_next_sibling() const {
        return !(_flags & FLAG_IS_LAST);
    }
    const NodeImpl* next_sibling() const {    // assuming consecutive placement
        if (!has_next_sibling()) return nullptr;
        assert(!is_root());
        auto next = (char*)this + _root->_node_size;
        return (NodeImpl*)next;
    }
    bool operator < (const NodeImpl& rhs) const {
        assert(!is_root());
        assert(_root == rhs._root);
        return get_key() < rhs.get_key();
    }
    bool operator < (str key) const {
        return get_key() < key;
    }

    int init_root(const char* text_begin, uint32_t node_size, bool text_ownership);
    int init_non_root(str key, str value, const NodeImpl* root, uint32_t flags);
};


}
}
