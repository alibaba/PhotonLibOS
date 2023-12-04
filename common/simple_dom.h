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

namespace fs {
class IFileSystem;
}

// simple unified interface for common needs in reading
// structured documents such as xml, json, yaml, ini, etc.
namespace SimpleDOM {

// SimpleDOM emphasize on:
// 1. simple & convenient usage;
// 2. unified interface, common needs (may not fit for corner cases);
// 3. efficient parsing (reading), as well as compiling;

using str = estring_view;

class RootNode;
struct NodeWrapper;

// the interface for internal implementations
class Node : public Object {
protected:
    Node() = default;
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

    RootNode* root() const __attribute__((pure)) {
        auto ptr = this;
        while (ptr->_parent)
            ptr = ptr->_parent;
        return (RootNode*)ptr;
    }

    NodeWrapper wrapper();
};

class RootNode : public Node {
protected:
    RootNode() = default;

    // reference counting is only applied on root
    // node, which represents the whole document
    std::atomic<uint32_t> _refcnt{0};

public:
    void add_ref() {
        _refcnt.fetch_add(1);
    }
    void rm_ref() {
        auto cnt = _refcnt.fetch_sub(1);
        if (cnt == 1)
            delete this;
    }
};

#define IF_RET(e) if (_node) return e; \
                        else return {};

// the interface for users
struct NodeWrapper {
    Node* _node = nullptr;
    NodeWrapper() = default;
    NodeWrapper(Node* node) {
        _node = node;
        _node->root()->add_ref();
    }
    NodeWrapper(const NodeWrapper& rhs) :
        NodeWrapper(rhs._node) { }
    NodeWrapper(NodeWrapper&& rhs) {
        _node = rhs._node;
        rhs._node = nullptr;
    }
    NodeWrapper& operator = (const NodeWrapper& rhs) {
        auto rt = root_node();
        auto rrt = rhs.root_node();
        if (rt != rrt) {
            if (rt) rt->rm_ref();
            if (rrt) rrt->add_ref();
        }
        _node = rhs._node;
        return *this;
    }
    NodeWrapper& operator = (NodeWrapper&& rhs) {
        if (_node)
            _node->root()->rm_ref();
        _node = rhs._node;
        rhs._node = nullptr;
        return *this;
    }
    ~NodeWrapper() {
        _node->root()->rm_ref();
    }
    NodeWrapper root() const       { return root_node(); }
    RootNode* root_node() const    { IF_RET(_node->root()); }
    NodeWrapper parent() const     { IF_RET(_node->_parent); }
    NodeWrapper next() const       { IF_RET(_node->_next); }
    rstring_view32 rkey() const    { IF_RET(_node->_key); }
    rstring_view32 rvalue() const  { IF_RET(_node->_value); }
    str key(const char* b) const   { IF_RET(b | rkey()); }
    str value(const char* b) const { IF_RET(b | rvalue()); }
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
    NodeWrapper get_attributes() const {
        return get("__attributes__");
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
        return get(key).enumerable_same_key_siblings();
    }
};
#undef IF_RET

// lower 8-bit are reserved for doc types
const int DOC_JSON = 0x00;
const int DOC_XML  = 0x01;
const int DOC_YAML = 0x02;
const int DOC_INI  = 0x03;
const int DOC_TYPE_MASK = 0xff;

const int FLAG_FREE_TEXT_IF_PARSING_FAILED = 0x100;

using Document = NodeWrapper;

inline NodeWrapper Node::wrapper() { return {this}; }

// 1. text is moved to the simple_dom object, which frees it when destruct.
// 2. the content of text may be modified in-place to un-escape strings.
// 3. returning a pointer (of Node) is more efficient than an object (of Document),
//    even if they are equivalent in binary form.
Node* parse(char* text, size_t size, int flags);

inline Node* parse(IStream::ReadAll&& buf, int flags) {
    auto node = parse((char*)buf.ptr.get(), (size_t)buf.size, flags);
    if (node || (flags & FLAG_FREE_TEXT_IF_PARSING_FAILED)) {
        buf.ptr.release();
        buf.size = 0;
    }
    return node;
}

inline Node* parse_copy(const char* text, size_t size, int flags) {
    auto copy = strndup(text, size);
    return parse(copy, size, flags | FLAG_FREE_TEXT_IF_PARSING_FAILED);
}

inline Node* parse_copy(const IStream::ReadAll& buf, int flags) {
    return parse_copy((char*)buf.ptr.get(), (size_t)buf.size,
                      flags | FLAG_FREE_TEXT_IF_PARSING_FAILED);
}

// assuming localfs by default
Node* parse_file(const char* filename, int flags, fs::IFileSystem* fs = nullptr);

Node* make_overlay(Node** nodes, int n);




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
    return enumerable(NodeWrapper::SameKeyEnumerator(_node->wrapper()));
}

}
}
