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
#include <stddef.h>
#include <assert.h>
#include <photon/common/string_view.h>
#include <photon/common/utility.h>

namespace photon {

template<size_t N>
class strBuider {
    char _buf[N];
    size_t _size = 0;
public:
    strBuider() = default;
    template<typename...Ts>
    strBuider(const Ts&...xs) { appends(xs...); }
    strBuider& operator +  (std::string_view s) { return append(s); }
    strBuider& operator += (std::string_view s) { return append(s); }
    strBuider& operator +  (char c) { return append(c); }
    strBuider& operator += (char c) { return append(c); }
    strBuider& append(std::string_view s) {
        assert(_size + s.size() < N);
        if (likely(_size + s.size() < N)) {
            memcpy(_buf + _size, s.data(), s.size());
            _size += s.size();
        }
        return *this;
    }
    strBuider& append(char c) {
        return append({&c, 1});
    }
    strBuider& appends() { return *this; }
    template<typename T, typename...Ts>
    strBuider& appends(const T& x, const Ts&...xs) {
        return append(x), appends(xs...);
    }
    operator std::string_view() {
        return to_string_view();
    }
    operator std::string_view() const {
        return to_string_view();
    }
    std::string_view to_string_view() {
        _buf[_size] = '\0';
        return {_buf, _size};
    }
    std::string_view to_string_view() const {
        return {_buf, _size};
    }
    bool operator == (std::string_view s) const {
        return to_string_view() == s;
    }
    bool operator != (std::string_view s) const {
        return to_string_view() != s;
    }
    bool operator < (std::string_view s) const {
        return to_string_view() < s;
    }
    bool operator <= (std::string_view s) const {
        return to_string_view() <= s;
    }
    bool operator > (std::string_view s) const {
        return to_string_view() > s;
    }
    bool operator >= (std::string_view s) const {
        return to_string_view() >= s;
    }
};

}