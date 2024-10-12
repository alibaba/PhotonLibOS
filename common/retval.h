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
#include <errno.h>
#include <type_traits>
#include <photon/common/utility.h>

namespace photon {

struct reterr {
    // use int64_t to make sure the result is returned
    // via another register, so that it is accessed easily
    int64_t _errno = 0;
    bool failed() const { return unlikely(_errno); }
    bool succeeded() const { return !failed(); }
    int get_errno() const { return (int)_errno; }
    operator int() const { return get_errno(); }
};

template<typename T> inline typename
std::enable_if<std::is_signed<T>::value, T>::type
failure_value() { return -1; }

template<typename T> inline typename
std::enable_if<!std::is_signed<T>::value, T>::type
failure_value() { return 0; }

template<typename T> inline typename
std::enable_if<std::is_signed<T>::value, bool>::type
is_failure(T x) { return unlikely(x < 0); }

template<typename T> inline typename
std::enable_if<!std::is_signed<T>::value, bool>::type
is_failure(T x) { return unlikely(x == 0); }

template<typename T>
struct retval : public reterr {
    T _val;
    retval(const retval&) = default;
    retval(T val) : reterr{is_failure(val) ? errno : 0}, _val(val) { }
    retval(T val, int _errno) : reterr{_errno}, _val(val) { }
    retval(const reterr& rvb) : reterr(rvb) {  // for failure
        _val = failure_value<T>();
        assert(failed());
    }
    operator T() const { return get(); }
    T operator->() { return get(); }
    T get() const { return _val; }
    reterr error() const { return (const reterr) *this; }
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
struct retval<void> : public reterr {
    retval(int errno_ = 0) : reterr{errno_} { }
    retval(const reterr& rvb) : reterr(rvb) { }
    void get() const { }
    reterr error() const { return (const reterr) *this; }
    bool operator==(const retval& rhs) const {
        return _errno == rhs._errno;
    }
    bool operator!=(const retval& rhs) const {
        return !(*this == rhs);
    }
};

}
