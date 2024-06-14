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

namespace photon {

struct retval_base {
    // use uint64_t to make sure the result is returned
    // via another register, so that it is accessed easily
    uint64_t _errno = 0;
    bool failed() const { return _errno; }
    bool succeeded() const { return !failed(); }
    int get_errno() const { assert(_errno > 0); return (int)_errno; }
};

template<typename T> inline
T failure_value() { return 0; }

template<typename T>
struct retval : public retval_base {
    T _val;
    retval(T x) : _val(x) { }
    retval(int _errno, T val) : retval_base{(uint64_t)_errno}, _val(val) {
        assert(failed());
    }
    retval(const retval_base& rvb) : retval_base(rvb) {
        assert(failed());
        _val = failure_value<T>();
    }
    operator T() const {
        return get();
    }
    T operator->() {
        return get();
    }
    T get() const {
        return _val;
    }
    retval_base base() const {
        return *this;
    }
    bool operator==(const retval& rhs) const {
        return _errno ? (_errno == rhs._errno) : (_val == rhs._val);
    }
    bool operator!=(const retval& rhs) const {
        return !(*this == rhs);
    }
    bool operator==(T rhs) const {
        return _val == rhs;
    }
    bool operator!=(T rhs) const {
        return _val != rhs;
    }
};

template<>
struct retval<void> : public retval_base {
    retval(int _errno = 0) : retval_base{(uint64_t)_errno} { }
    retval(const retval_base& rvb) : retval_base(rvb) { }
    void get() const { }
    retval_base base() const {
        return *this;
    }
    bool operator==(const retval& rhs) const {
        return _errno == rhs._errno;
    }
    bool operator!=(const retval& rhs) const {
        return !(*this == rhs);
    }
};

}

#define DEFINE_FAILURE_VALUE(type, val) namespace photon {  \
    template<> inline type failure_value<type>() { return val; } }

DEFINE_FAILURE_VALUE(int8_t,  -1)
DEFINE_FAILURE_VALUE(int16_t, -1)
DEFINE_FAILURE_VALUE(int32_t, -1)
DEFINE_FAILURE_VALUE(int64_t, -1)

