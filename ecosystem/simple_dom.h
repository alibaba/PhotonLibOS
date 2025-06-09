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

namespace photon {

namespace fs {
class IFileSystem;
class IFile;
}

// SimpleDOM emphasize on:
// 1. a simple & convenient interface for JSON, XML, YAML, INI, etc;
// 2. fast compilation, efficient accessing;
// 3. common needs;
namespace SimpleDOM {

using str = estring_view;

// the interface for users
class Node {
    const NodeImpl* _impl = nullptr;
public:
    Node() = default;
    Node(const NodeImpl* node) {
        _impl = node;
        if (_impl)
            _impl->get_root()->add_doc_ref();
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
            _impl->get_root()->del_doc_ref();
        _impl = rhs._impl;
        rhs._impl = nullptr;
        return *this;
    }
    ~Node() {
        if (_impl)
            _impl->get_root()->del_doc_ref();
    }

    #define IF_RET(e) if (_impl) return e; else return {};
    Node next() const               { IF_RET(_impl->next_sibling()); }
    bool is_root() const            { IF_RET(_impl->is_root()); }
    Node get_root() const           { IF_RET(_impl->get_root()); }
    const NodeImpl* root_impl()const{ IF_RET(_impl->get_root()); }
    str key() const                 { IF_RET(_impl->get_key()); }
    str value() const               { IF_RET(_impl->get_value()); }
    const char* text_begin() const  { IF_RET(_impl->get_root()->_text_begin); }
    str key(const char* b) const    { IF_RET(_impl->get_key(b)); }
    str value(const char* b) const  { IF_RET(_impl->get_value(b)); }
    bool valid() const              { return _impl; }
    operator bool() const           { return _impl; }
    size_t num_children() const     { IF_RET(_impl->num_children()); }
    Node get(size_t i) const        { IF_RET({_impl->get(i)}); }
    Node get(str key) const         { IF_RET({_impl->get(key)}); }
    Node operator[](str key) const  { return get(key); }
    Node operator[](const char* key) const  { return get(key); }
    Node operator[](size_t i) const { return get(i); }
    Node get_attributes() const     { return get("__attributes__"); }
    str to_string_view() const      { return value(); }
    #undef IF_RET
    int64_t to_int64_t(int64_t def_val = 0) const {
        return value().to_int64(def_val);
    }
    double to_double(double def_val = NAN) const {
        return value().to_double(def_val);
    }

    bool operator==(str rhs) const { return value() == rhs; }
    bool operator!=(str rhs) const { return value() != rhs; }
    bool operator<=(str rhs) const { return value() <= rhs; }
    bool operator< (str rhs) const { return value() <  rhs; }
    bool operator>=(str rhs) const { return value() >= rhs; }
    bool operator> (str rhs) const { return value() >  rhs; }

    bool operator==(int64_t rhs) const { return to_int64_t() == rhs; }
    bool operator!=(int64_t rhs) const { return to_int64_t() != rhs; }
    bool operator<=(int64_t rhs) const { return to_int64_t() <= rhs; }
    bool operator< (int64_t rhs) const { return to_int64_t() <  rhs; }
    bool operator>=(int64_t rhs) const { return to_int64_t() >= rhs; }
    bool operator> (int64_t rhs) const { return to_int64_t() >  rhs; }

    bool operator==(double rhs) const { return to_double() == rhs; }
    bool operator!=(double rhs) const { return to_double() != rhs; }
    bool operator<=(double rhs) const { return to_double() <= rhs; }
    bool operator< (double rhs) const { return to_double() <  rhs; }
    bool operator>=(double rhs) const { return to_double() >= rhs; }
    bool operator> (double rhs) const { return to_double() >  rhs; }

    struct SameKeyEnumerator;
    auto enumerable_same_key_siblings() const ->
            Enumerable_Holder<SameKeyEnumerator>;

    struct ChildrenEnumerator;
    auto enumerable_children() const ->
            Enumerable_Holder<ChildrenEnumerator>;

    auto enumerable_children(str key) const ->
            Enumerable_Holder<SameKeyEnumerator>;
};

// lower 8 bits are reserved for doc types
const int DOC_JSON = 0x00;
const int DOC_XML  = 0x01;
const int DOC_YAML = 0x02;
const int DOC_INI  = 0x03;
const int DOC_TYPE_MASK = 0xff;

const int DOC_FREE_TEXT_IF_PARSING_FAILED   = 0x100;
const int DOC_FREE_TEXT_ON_DESTRUCTION      = 0x200;
const int DOC_OWN_TEXT                      = DOC_FREE_TEXT_IF_PARSING_FAILED |
                                              DOC_FREE_TEXT_ON_DESTRUCTION;

using Document = Node;

// 1. text is handed over to the simple_dom object, and gets freed during destruction
// 2. the content of text may be modified in-place to un-escape strings.
// 3. returning a pointer (of NodeImpl) is more efficient than an object (of Document),
//    even if they are equivalent in binary form.
Node parse(char* text, size_t size, int flags);

inline Node parse(IStream::ReadAll&& buf, int flags) {
    if (!buf.ptr || buf.size <= 0) return nullptr;
    auto node = parse((char*)buf.ptr.get(), (size_t)buf.size, flags);
    if (node) {
        buf.ptr.release();
    } else if (flags & DOC_FREE_TEXT_IF_PARSING_FAILED) {
        buf.ptr.reset();
    }
    buf.size = 0;
    return node;
}

inline Node parse_copy(const char* text, size_t size, int flags) {
    return parse(strndup(text, size), size, flags | DOC_OWN_TEXT);
}

inline Node parse_copy(const IStream::ReadAll& buf, int flags) {
    if (!buf.ptr || buf.size <= 0) return nullptr;
    return parse_copy((const char*)buf.ptr.get(), (size_t)buf.size, flags);
}

Node parse_file(fs::IFile* file, int flags);

// assuming localfs by default
Node parse_file(const char* filename, int flags, fs::IFileSystem* fs = nullptr);

Node make_overlay(Node* nodes, int n);

struct Node::ChildrenEnumerator {
    const NodeImpl* _impl;
    bool valid() const {
        return _impl;
    }
    Node get() const {
        return _impl;
    }
    int next() {
        _impl = _impl->next_sibling();
        return _impl ? 0 : -1;
    }
};

inline auto Node::enumerable_children() const ->
        Enumerable_Holder<Node::ChildrenEnumerator> {
    return enumerable<Node::ChildrenEnumerator>({_impl->get(0)});
}

struct Node::SameKeyEnumerator : public Node::ChildrenEnumerator {
    const char* _base = nullptr;
    SameKeyEnumerator(const NodeImpl* node) {
        if ((_impl = node)) {
            _base = node->get_root()->_text_begin;
        }
    }
    int next() {
        if (!valid() || (_impl->_flags & NodeImpl::FLAG_EQUAL_KEY_LAST)) return -1;
        auto key = _impl->get_key(_base);
        do {
            _impl = _impl->next_sibling();
            if (!valid()) return -1;
        } while (key != _impl->get_key(_base));
        return 0;
    }
};

inline auto Node::enumerable_same_key_siblings() const ->
        Enumerable_Holder<Node::SameKeyEnumerator> {
    return enumerable<Node::SameKeyEnumerator>({_impl});
}

inline auto Node::enumerable_children(str key) const ->
        Enumerable_Holder<Node::SameKeyEnumerator> {
    return get(key).enumerable_same_key_siblings();
}


}
}
