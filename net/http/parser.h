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
#include <photon/common/estring.h>

namespace photon {
namespace net {
namespace http {

class Parser {
public:
    Parser(std::string_view headers)
    {
        _begin = _ptr = headers.data();
        _end = _begin + headers.size();
    }
    void skip_string(std::string_view sv)
    {
        if (estring_view(_ptr, _end - _ptr).starts_with(sv)) _ptr += sv.length();
    }
    void skip_chars(char c, bool continuously = false)
    {
        while (*_ptr == c) {
            _ptr++;
            if (!continuously)
                return;
        }
    }
    void skip_until_string(const std::string& s)
    {
        auto esv = estring_view(_ptr, _end - _ptr);
        auto pos = esv.find(s);
        if (pos == esv.npos) {
            _ptr = _end;
            return;
        }
        _ptr += pos;
    }
    uint64_t extract_integer()
    {
        auto esv = estring_view(_ptr, _end - _ptr);
        auto ret = esv.to_uint64();
        auto pos = esv.find_first_not_of(charset("0123456789"));
        if (pos == esv.npos) _ptr = _end; else _ptr += pos;
        return ret;
    }
    rstring_view16 extract_until_char(char c)
    {
        auto esv = estring_view(_ptr, _end - _ptr);
        auto pos = esv.find_first_of(c);
        auto ptr = _ptr;
        if (pos == esv.npos) _ptr = _end; else _ptr += pos;
        return {(uint16_t)(ptr - _begin), (uint16_t)(_ptr - ptr)};
    }
    bool is_done() { return _ptr == _end; }
    char operator[](size_t i) const
    {
        return _ptr[i];
    }
    char *cur() {
        return (char*)_ptr;
    }

protected:
    const char* _ptr;
    const char* _begin;
    const char* _end;
};

} // namespace http
} // namespace net
} // namespace photon
