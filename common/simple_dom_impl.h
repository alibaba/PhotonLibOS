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

struct Node;

// the interface for internal implementations
class NodeImpl : public Object {
protected:
    NodeImpl() = default;
    NodeImpl* _root;
union {
    NodeImpl* _next;
    const char* _text_begin;     // the root node have text begin (base
};                               // of _key and _value of rstring_view)
union {
    rstring_view32 _key;        // root node doesn't have a valid key, do not try to get it
    std::atomic<uint32_t> _refcnt{0};
};
    rstring_view32 _value;

    void add_doc_ref() {
        assert(this == _root);
        ++_refcnt;
    }

    void del_doc_ref() {
        assert(this == _root);
        if (--_refcnt == 0)
            delete this;
    }

    friend struct Node;

public:
    virtual size_t num_children() const __attribute__((pure)) = 0;

    // get the i-th child node
    // for an array object, it gets the i-th element (doc type determines the starting value)
    // for an object, it gets the i-th element in implementation defined order
    virtual NodeImpl* get(size_t i) const __attribute__((pure)) = 0;

    // get the first child node with a specified `key`
    // XML attributes are treated as a special child node with key "__attributes__"
    virtual NodeImpl* get(str key) const __attribute__((pure)) = 0;
};

}
}
