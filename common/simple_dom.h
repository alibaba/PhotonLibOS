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
#include "simple_dom_impl.h"
// #include <inttypes.h>
// #include <assert.h>
// #include <memory>
// #include <math.h>
// #include <atomic>
// #include <photon/common/object.h>
// #include <photon/common/estring.h>
// #include <photon/common/enumerable.h>
// #include <photon/common/stream.h>


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

#define IF_RET(e) if (_impl) return e; \
                        else return {};

// the interface for users
struct Node {
    NodeImpl* _impl = nullptr;
    Node() = default;
    Node(NodeImpl* node) {
        _impl = node;
        _impl->root()->add_doc_ref();
    }
    Node(const Node& rhs) :
        Node(rhs._impl) { }
    Node(Node&& rhs) {
        _impl = rhs._impl;
        rhs._impl = nullptr;
    }
    Node& operator = (const Node& rhs) {
        auto rt = root_impl();
        auto rrt = rhs.root_impl();
        if (rt != rrt) {
            if (rt) rt->del_doc_ref();
            if (rrt) rrt->add_doc_ref();
        }
        _impl = rhs._impl;
        return *this;
    }
    Node& operator = (Node&& rhs) {
        if (_impl)
            _impl->root()->del_doc_ref();
        _impl = rhs._impl;
        rhs._impl = nullptr;
        return *this;
    }
    ~Node() {
        _impl->root()->del_doc_ref();
    }
    Node parent() const     { IF_RET(_impl->_parent); }
    Node next() const       { IF_RET(_impl->_next); }
    bool is_root() const    { IF_RET(_impl->is_root()); }
    Node root() const       { IF_RET(_impl->root()); }
    NodeImpl* root_impl() const    { IF_RET(_impl->root()); }
    rstring_view32 rkey() const    { assert(!is_root()); IF_RET(_impl->_key); }
    rstring_view32 rvalue() const  { IF_RET(_impl->_value); }
    str key(const char* b) const   { IF_RET(b | rkey()); }
    str value(const char* b) const { IF_RET(b | rvalue()); }
    const char* text_begin() const {
        IF_RET(root()._impl->_text_begin);
    }
    str key() const    { IF_RET(text_begin() | rkey()); }
    str value() const  { IF_RET(text_begin() | rvalue()); }
    bool valid() const { return _impl; }
    operator bool() const { return _impl; }

    size_t num_children() const {
        return _impl ? _impl->num_children() : 0;
    }
    Node get(size_t i) const {
        IF_RET({_impl->get(i)});
    }
    Node get(str key) const {
        IF_RET({_impl->get(key)});
    }
    template<size_t N>
    Node operator[](const char (&key)[N]) const {
        return get(key);
    }
    Node operator[](str key) const {
        return get(key);
    }
    Node operator[](size_t i) const {
        return get(i);
    }
    Node get_attributes() const {
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

// lower 8 bits are reserved for doc types
const int DOC_JSON = 0x00;
const int DOC_XML  = 0x01;
const int DOC_YAML = 0x02;
const int DOC_INI  = 0x03;
const int DOC_TYPE_MASK = 0xff;

const int FLAG_FREE_TEXT_IF_PARSING_FAILED = 0x100;

using Document = Node;

inline Node NodeImpl::wrap() { return {this}; }

// 1. text is moved to the simple_dom object, which frees it when destruct.
// 2. the content of text may be modified in-place to un-escape strings.
// 3. returning a pointer (of NodeImpl) is more efficient than an object (of Document),
//    even if they are equivalent in binary form.
NodeImpl* parse(char* text, size_t size, int flags);

inline NodeImpl* parse(IStream::ReadAll&& buf, int flags) {
    auto node = parse((char*)buf.ptr.get(), (size_t)buf.size, flags);
    if (node || (flags & FLAG_FREE_TEXT_IF_PARSING_FAILED)) {
        buf.ptr.reset();
        buf.size = 0;
    }
    return node;
}

inline NodeImpl* parse_copy(const char* text, size_t size, int flags) {
    auto copy = strndup(text, size);
    return parse(copy, size, flags | FLAG_FREE_TEXT_IF_PARSING_FAILED);
}

inline NodeImpl* parse_copy(const IStream::ReadAll& buf, int flags) {
    return parse_copy((char*)buf.ptr.get(), (size_t)buf.size, flags);
}

// assuming localfs by default
NodeImpl* parse_filename(const char* filename, int flags, fs::IFileSystem* fs = nullptr);

NodeImpl* make_overlay(NodeImpl** nodes, int n);




struct Node::ChildrenEnumerator {
    Node _impl;
    Node get() const {
        return _impl;
    }
    int next() {
        _impl = _impl.next();
        return _impl.valid() ? 0 : -1;
    }
};

inline Enumerable<Node::ChildrenEnumerator>
Node::enumerable_children() const {
    return enumerable(Node::ChildrenEnumerator{_impl->get(0)});
}

struct Node::SameKeyEnumerator {
    Node _impl;
    const char* _base;
    str _key;
    SameKeyEnumerator(Node node) : _impl(node) {
        _base = node.text_begin();
        _key = node.key(_base);
    }
    Node get() const {
        return _impl;
    }
    int next() {
        _impl = _impl.next();
        return (_impl.valid() && _impl.key(_base) == _key) ? 0 : -1;
    }
};

inline Enumerable<Node::SameKeyEnumerator>
Node::enumerable_same_key_siblings() const {
    return enumerable(Node::SameKeyEnumerator(_impl->wrap()));
}

}
}
