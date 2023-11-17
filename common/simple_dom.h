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
#include <photon/common/object.h>
#include <photon/common/estring.h>
#include <photon/common/enumerable.h>


class IStream;
namespace photon {

// simple unified interface for common needs in reading
// structured documents such as xml, json, yaml, ini, etc.
namespace SimpleDOM {

// SimpleDOM emphasize on:
// 1. simple & convenient usage;
// 2. unified interface, common needs (may not fit for corner cases);
// 3. efficient parsing (reading), as well as compiling;

using str = estring_view;

// the interface for internal implementations
class Node : public Object {
protected:
    Node* _parent;
union {
    Node* _next;
    const char* _text_begin;     // the root node have text begin (base
};  rstring_view32 _key, _value; // of _key and _value of rstring_view)

    friend struct NodeWrapper;

public:
    virtual size_t num_children() const __attribute__((pure)) = 0;

    // get the i-th child node
    virtual Node* get(size_t i) const __attribute__((pure)) = 0;

    // get the first child node with a specified `key`
    virtual Node* get(str key) const __attribute__((pure)) = 0;
};

#define IF_RET(e) if (_node) return e; \
                        else return {};

// the interface for users
struct NodeWrapper {
    Node* _node = nullptr;
    NodeWrapper parent() const     { IF_RET({_node->_parent}); }
    NodeWrapper next() const       { IF_RET({_node->_next}); }
    rstring_view32 rkey() const    { IF_RET(_node->_key); }
    rstring_view32 rvalue() const  { IF_RET(_node->_value); }
    str key(const char* b) const   { IF_RET(b | rkey()); }
    str value(const char* b) const { IF_RET(b | rvalue()); }
    NodeWrapper root() const __attribute__((pure)) {
        if (!_node) return {};
        auto ptr = _node;
        while (ptr->_parent)
            ptr = ptr->_parent;
        return {ptr};
    }
    const char* text_begin() const {
        IF_RET(root()._node->_text_begin);
    }
    str key() const    { IF_RET(text_begin() | rkey()); }
    str value() const  { IF_RET(text_begin() | rvalue()); }
    bool valid() const { return _node; }
    operator bool() const { return _node; }

    size_t num_children() const {
        return _node ? _node->num_children() : 0;
    }
    NodeWrapper get(size_t i) const {
        IF_RET({_node->get(i)});
    }
    NodeWrapper get(str key) const {
        IF_RET({_node->get(key)});
    }
    template<size_t N>
    NodeWrapper operator[](const char (&key)[N]) const {
        return get(key);
    }
    NodeWrapper operator[](str key) const {
        return get(key);
    }
    NodeWrapper operator[](size_t i) const {
        return get(i);
    }
    str to_string() const {
        return value();
    }
    int64_t to_integer(int64_t def_val = 0) const {
        return value().to_uint64(def_val);
    }
    double to_number(double def_val = NAN) const {
        return value().to_double(def_val);
    }

    struct SameKeyEnumerator;
    Enumerable<SameKeyEnumerator> enumerable_same_key_siblings() const;

    struct ChildrenEnumerator;
    Enumerable<ChildrenEnumerator> enumerable_children() const;
    Enumerable<SameKeyEnumerator>  enumerable_children(str key) const {
        return (*this)[key].enumerable_same_key_siblings();
    }
};
#undef IF_RET

struct NodeWrapper::ChildrenEnumerator {
    NodeWrapper _node;
    NodeWrapper get() const {
        return _node;
    }
    int next() {
        _node = _node.next();
        return _node.valid() ? 0 : -1;
    }
};

inline Enumerable<NodeWrapper::ChildrenEnumerator>
NodeWrapper::enumerable_children() const {
    return enumerable(NodeWrapper::ChildrenEnumerator{_node->get(0)});
}

struct NodeWrapper::SameKeyEnumerator {
    NodeWrapper _node;
    const char* _base;
    str _key;
    SameKeyEnumerator(NodeWrapper node) : _node(node) {
        _base = node.text_begin();
        _key = node.key(_base);
    }
    NodeWrapper get() const {
        return _node;
    }
    int next() {
        _node = _node.next();
        return (_node.valid() && _node.key(_base) == _key) ? 0 : -1;
    }
};

inline Enumerable<NodeWrapper::SameKeyEnumerator>
NodeWrapper::enumerable_same_key_siblings() const {
    return enumerable(NodeWrapper::SameKeyEnumerator({_node}));
}

// the whole document, as well as the root node
struct Document : public NodeWrapper {
    Document(Node* node) : NodeWrapper{node} { }
    Document(Document&& rhs) : NodeWrapper{rhs._node} {
        rhs._node = nullptr;
    }
    Document& operator=(Document&& rhs) {
        delete _node;
        _node = rhs._node;
        rhs._node = nullptr;
        return *this;
    }
    ~Document() { delete _node; }
};

const int DOC_JSON = 0x00;
const int DOC_XML  = 0x01;
const int DOC_YAML = 0x02;
const int DOC_INI  = 0x03;
const int TEXT_OWNERSHIP = 0x100;  // free(text) when destruct

// the content of text may be modified in-place to un-escape strings
Document parse(char* text, size_t size, int flags);

inline Document parse_copy(const char* text, size_t size, int flags) {
    auto copy = strndup(text, size);
    return parse(copy, size, flags | TEXT_OWNERSHIP);
}

Document parse_stream(IStream* s, int flags, size_t max_size = 1024 * 1024 * 1024);

Document parse_filename(const char* filename, int flags);

Node* make_overlay(Node** nodes, int n);

}
}
