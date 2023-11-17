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
#include <cstdio>
#include <cassert>
#include <cstring>
#include <math.h>
#include <limits>
#include <string>
#include <bitset>
#include <tuple>
#include <type_traits>

#include <photon/common/string_view.h>
#include <photon/common/utility.h>

struct charset : std::bitset<256>
{
    charset() = default;
    charset(char ch)
    {
        this->set(ch);
    }
    charset(const std::string_view& sv)
    {
        assert(!sv.empty());
        for (auto ch: sv)
            this->set(ch);
    }
    // template<size_t N>
    // charset(const char (&s) [N]) :
    //     charset(std::string_view(s, N - 1)) { }

    // bool test(char ch) const
    // {
    //     return std::bitset<256>::test((unsigned char)ch);
    // }
    // bitset& set(char ch, bool value = true)
    // {
    //     return std::bitset<256>::set((unsigned char)ch, value);
    // }
};

class estring_view : public std::string_view
{
public:
    using std::string_view::string_view;
    estring_view() : estring_view(nullptr, (size_type)0) { }
    estring_view(const char* begin, const char* end) :
        estring_view(begin , end - begin) { }
    estring_view(std::string_view& sv) : std::string_view(sv) {}
    estring_view(const std::string_view& sv) : std::string_view(sv) {}
    estring_view(const std::string& s) : std::string_view(s) {}

    using std::string_view::find_first_of;
    using std::string_view::find_first_not_of;
    using std::string_view::find_last_of;
    using std::string_view::find_last_not_of;
    size_t find_first_of(const charset& set) const;
    size_t find_first_not_of(const charset& set) const;
    size_t find_last_of(const charset& set) const;
    size_t find_last_not_of(const charset& set) const;

    operator std::string ()
    {
        return to_string();
    }
    std::string to_string() const
    {
        return std::string(data(), length());
    }
    bool all_digits() const
    {
        return to_uint64_check(nullptr);
    }
    estring_view substr(size_type pos = 0, size_type count = npos) const
    {
        auto ret = std::string_view::substr(pos, count);
        return (estring_view&)ret;
    }

    estring_view trim(const charset& spaces = charset(" \t\r\n")) const
    {
        auto start = find_first_not_of(spaces);
        if (start == npos)
            return {};

        auto end = find_last_not_of(spaces);
        assert(end >= start);
        return substr(start, end - start + 1);
    }
#if __cplusplus < 202000L
    bool starts_with(estring_view x)
    {
        auto len = x.size();
        return length() >= len &&
            memcmp(data(), x.data(), len) == 0;
    }

    bool ends_with(estring_view x)
    {
        auto len = x.size();
        return length() >= len &&
            memcmp(&*end() - len, x.data(), len) == 0;
    }
#endif

    bool istarts_with(estring_view x) {
        return strncasecmp(data(), x.data(), x.size()) == 0;
    }

    int icmp(estring_view x) {
        auto ret = strncasecmp(data(), x.data(), std::min(size(), x.size()));
        if (ret != 0) return ret;
        return size() - x.size();
    }

    template<typename Separator>
    struct _split
    {
        std::string_view str;
        Separator sep;
        bool consecutive_merge;

        class iterator
        {
        public:
            iterator() = default;
            iterator(const _split* host, size_t pos = 0)
            {
                _host = host;
                _part = _host->find_part(_host->str.data() + pos);
            }
            estring_view operator*() const
            {
                return _part;
            }
            const estring_view* operator->() const
            {
                return static_cast<const estring_view*>(&_part);
            }
            estring_view remainder() const
            {
                auto it = *this; ++it;
                return {&*it._part.begin(), &*_host->str.end()};
            }
            iterator& operator++()
            {
                _part = _host->find_part(_part.end());
                return *this;
            }
            iterator& operator++(int)
            {
                auto ret = *this;
                ++(*this);
                return ret;
            }
            bool operator == (const iterator& rhs) const
            {
                return _part == rhs._part;
            }
            bool operator != (const iterator& rhs) const
            {
                return !(*this == rhs);
            }

        protected:
            const _split* _host;
            std::string_view _part;
        };

        const estring_view& get_sep_ref(const estring_view*) const
        {
            return *sep;
        }
        const estring_view& get_sep_ref(const estring_view&) const
        {
            return sep;
        }
        const charset& get_sep_ref(const charset*) const
        {
            return *sep;
        }
        const charset& get_sep_ref(const charset&) const
        {
            return sep;
        }

        estring_view find_part(const char* begin) const
        {
            auto end = &*str.end();
            auto& separator = get_sep_ref(sep);

            if (consecutive_merge)
            {   // skip leading separators, if consecutive_merge
                auto pos = estring_view(begin, end).find_first_not_of(separator);
                if (pos == estring_view::npos) {
                    begin = end;
                } else {
                    begin += pos;
                }
            }

            if (begin >= end)
                return {};

            // count non-separators
            estring_view sv(begin, end);
            auto len = sv.find_first_of(separator);
            if (len == estring_view::npos)
                len = sv.size();

            return estring_view(begin, len);
        }

        // class const_iterator : iterator
        // {
        // public:
        //     using iterator::iterator;
        //     const string_view operator* () const
        //     {
        //         return _part;
        //     }
        // };

        iterator begin() const
        {
            return iterator(this);
        }
        iterator end() const
        {
            return iterator(this, str.size());
        }
        estring_view front() const
        {
            return *begin();
        }
        estring_view operator[](size_t i) const
        {
            // TODO: support for negative index
            if (i == 0)
                return front();

            auto it = begin();
            auto _end = end();
            while(i-- && it != _end) ++it;
            return *it;
        }
    };
    template<size_t N>
    _split<charset> split(const char (&s)[N], bool consecutive_merge = true) const
    {
        return {*this, (charset(s)), consecutive_merge};
    }
    _split<charset> split(charset&& sep, bool consecutive_merge = true) const
    {
        return {*this, (sep), consecutive_merge};
    }
    _split<const charset*> split(const charset& sep, bool consecutive_merge = true) const
    {
        return {*this, &sep, consecutive_merge};
    }
    _split<std::string> split(std::string&& sep, bool consecutive_merge = true) const
    {
        return {*this, (sep), consecutive_merge};
    }
    _split<const std::string*> split(const std::string& sep, bool consecutive_merge = true) const
    {
        return {*this, &sep, consecutive_merge};
    }
    _split<charset> split_lines(bool consecutive_merge = true) const
    {
        return split(charset("\r\n"), consecutive_merge);
    }
    bool     to_uint64_check(uint64_t* v = nullptr) const;
    uint64_t to_uint64(uint64_t default_val = 0) const
    {
        uint64_t val;
        return to_uint64_check(&val) ? val : default_val;
    }
    bool     to_int64_check(int64_t* v = nullptr) const
    {
        if (this->empty()) return false;
        if (this->front() != '-') return to_uint64_check((uint64_t*)v);
        bool ret = this->substr(1).to_uint64_check((uint64_t*)v);
        if (ret) *v = -*v;
        return ret;
    }
    int64_t to_int64(int64_t default_val = 0) const
    {
        int64_t val;
        return to_int64_check(&val) ? val : default_val;
    }
    bool to_double_check(double* v = nullptr)
    {
        char buf[32];
        auto len = std::max(this->size(), sizeof(buf) - 1 );
        memcpy(buf, data(), len);
        buf[len] = '0';
        return sscanf(buf, "%lf", v) == 1;
    }
    double to_double(double default_val = NAN)
    {
        double val;
        return to_double_check(&val) ? val : default_val;
    }
    // not including 0x/0X prefix
    bool hex_to_uint64_check(uint64_t* v = nullptr) const;
    uint64_t hex_to_uint64(uint64_t default_val = 0) const
    {
        uint64_t val;
        return hex_to_uint64_check(&val) ? val : default_val;
    }
};

inline bool operator == (const std::string_view& sv, const std::string& s)
{
    return sv == std::string_view(s);
}

inline bool operator == (const std::string& s, const std::string_view& sv)
{
    return sv == s;
}

template<typename OffsetType, typename LengthType = OffsetType>
class rstring_view  // relative string_view, that stores values relative
{                   //  to a standard string, string_view or char*
protected:
    static_assert(std::is_integral<OffsetType>::value, "...");
    static_assert(std::is_integral<LengthType>::value, "...");
    OffsetType _offset = 0;
    LengthType _length = 0;

    estring_view to_abs(const char* s) const
    {
        return estring_view(s + _offset, _length);
    }

public:
    constexpr rstring_view() = default;
    constexpr rstring_view(uint64_t offset, uint64_t length) {
        assert(offset <= std::numeric_limits<OffsetType>::max());
        assert(length <= std::numeric_limits<LengthType>::max());
        _offset = (OffsetType)offset;
        _length = (LengthType)length;
    }
    estring_view operator | (const char* s) const
    {
        return to_abs(s);
    }
    estring_view operator | (const std::string_view& s) const
    {
        assert(s.length() >= _offset + _length);
        return to_abs(s.data());
    }
    bool operator == (const rstring_view& rhs) const
    {
        return _offset == rhs._offset && _length == rhs._length;
    }
    bool operator != (const rstring_view& rhs) const
    {
        return !(*this == rhs);
    }
    rstring_view& operator += (int x)
    {
        _offset += x;
        return *this;
    }
    LengthType& size()
    {
        return _length;
    }
    LengthType size() const
    {
        return _length;
    }
    LengthType& length()
    {
        return _length;
    }
    LengthType length() const
    {
        return _length;
    }
    OffsetType& offset()
    {
        return _offset;
    }
    OffsetType offset() const
    {
        return _offset;
    }
};

template<typename T1, typename T2>
inline estring_view operator | (const std::string_view& s,
                                const rstring_view<T1, T2>& rsv) {
    return rsv | s;
}

using rstring_view8 = rstring_view<uint8_t>;
using rstring_view16 = rstring_view<uint16_t>;
using rstring_view32 = rstring_view<uint32_t>;

class estring : public std::string
{
public:
    using std::string::string;

    estring() = default;
    estring(const std::string& v) : std::string(v) {}
    estring(estring_view sv) : std::string(sv) { }
    estring(std::string_view sv) : std::string(sv) { }

    estring_view view() const
    {
        return {c_str(), size()};
    }
    estring_view trim(const charset& spaces = charset(" \t\r\n")) const
    {
        return view().trim(spaces);
    }
#if __cplusplus < 202000L
    bool starts_with(estring_view x)
    {
        return view().starts_with(x);
    }
    bool ends_with(estring_view x)
    {
        return view().ends_with(x);
    }
#endif
    bool all_digits() const
    {
        return view().all_digits();
    }

    template<typename...Ts>
    auto split(const Ts&...xs) const ->
        decltype(view().split(xs...))
    {
        return view().split(xs...);
    }

    estring_view::_split<charset> split_lines(bool consecutive_merge = true) const
    {
        return view().split(charset("\r\n"), consecutive_merge);
    }

    template<size_t N = 4096, typename...Ts>
    static estring snprintf(const Ts&...xs)
    {
        char buf[N];
        int n = ::snprintf(buf, N, xs...);
        return estring(buf, n < 0 ? 0 :
            n > (int)N ? N :
            n);
    }

    // using std::string::operator+=;
    template<typename T>
    estring& operator+=(T&& t) {
        return (estring&)std::string::operator+=(std::forward<T>(t));
    }
    estring& operator += (const std::string_view &rhs)
    {
        return append(rhs);
    }
    // using std::string::append;
    template <typename... Args>
    estring& append(Args&&... t) {
        return (estring&)std::string::append(std::forward<Args>(t)...);
    }
    estring& append(uint64_t x);
    estring& append(const std::string_view& sv)
    {
        return append(sv.data(), sv.size());
    }

    template<typename...Ts>
    estring& appends(const Ts&...xs)
    {
        return cat(make_cat_list(xs...));
    }

protected:
    template<typename...Ts>
    class CatList : public std::tuple<Ts...>
    {
    public:
        CatList(const Ts&...xs) : std::tuple<Ts...>(xs...) { }
    };

    template<typename...Ts>
    class ConditionalCatList
    {
    public:
        CatList<Ts...> _cl;
        bool _cond;

        ConditionalCatList(bool cond, const Ts&...xs) :
            _cl(xs...), _cond(cond) { }
    };

    template<typename A, typename B>
    class ConditionalCatList2
    {
    public:
        A _cl1;
        B _cl2;
        bool _cond;

        ConditionalCatList2(bool cond, const A& a, const B& b) :
            _cl1(a), _cl2(b), _cond(cond) { }
    };

    template<typename...Ts>
    __attribute__((always_inline))
    estring& cat(const CatList<Ts...>& cl)
    {
        UpperBoundEstimator estimator;
        this->enumerate(cl, estimator);
        reserve(size() + estimator.upper_bound);
        Appender apppender{this};
        this->enumerate(cl, apppender);
        return *this;
    }

    template<typename...Ts>
    estring& cat(const ConditionalCatList<Ts...>& ccl)
    {
        return ccl._cond ? cat(ccl._cl) : *this;
    }

    template<typename A, typename B>
    estring& cat(const ConditionalCatList2<A, B>& x)
    {
        return x._cond ? cat(x._cl1) : cat(x._cl2);
    }

    template<typename...Ts>
    static const ConditionalCatList<Ts...>&
                                 cat_item_filter(const ConditionalCatList<Ts...>& x)
                                                                          { return x; }
    template<typename...Ts>
    static const CatList<Ts...>& cat_item_filter(const CatList<Ts...>& x) { return x; }
    static std::string_view      cat_item_filter(std::string_view x)      { return x; }
    static uint64_t              cat_item_filter(uint64_t x)              { return x; }

public:
    template<typename...Ts> static
    auto make_cat_list(const Ts&...xs) ->
        CatList<decltype(cat_item_filter(xs))...>
    {
        return {cat_item_filter(xs)...};
    }

    template<typename...Ts> static
    auto make_conditional_cat_list(bool cond, const Ts&...xs) ->
        ConditionalCatList<decltype(cat_item_filter(xs))...>
    {
        return {cond, cat_item_filter(xs)...};
    }

    template<typename ...As, typename ...Bs> static
    auto make_conditional_cat_list2(bool cond, const CatList<As...>& a, const CatList<Bs...>& b) ->
        ConditionalCatList2<CatList<As...>, CatList<Bs...>>
    {
        return {cond, a, b};
    }

protected:
    template<size_t I = 0, typename...Ts, typename Mapper>
    typename std::enable_if<I == sizeof...(Ts), void>::type
    static enumerate(const CatList<Ts...>& cl, Mapper& m, int = 0) { }

    template<size_t I = 0, typename...Ts, typename Mapper>
    typename std::enable_if<I < sizeof...(Ts), void>::type
    static enumerate(const CatList<Ts...>& cl, Mapper& m)
    {
        m(std::get<I>(cl));
        enumerate<I+1>(cl, m);
    }

    template<typename D>
    struct Visitor
    {
        template<typename...Ts>
        void operator()(const CatList<Ts...>& x)   { enumerate(x, (D&)*this); }
        template<typename...Ts>
        void operator()(const ConditionalCatList<Ts...>& x)
                                  { if (x._cond) enumerate(x._cl, (D&)*this); }
    };
    struct UpperBoundEstimator : public Visitor<UpperBoundEstimator>
    {
        size_t upper_bound = 0;
        using Visitor::operator();
        void operator()(const std::string_view& x) { upper_bound += x.size(); }
        void operator()(uint64_t x)                { upper_bound += 24; }
    };
    struct Appender : public Visitor<Appender>
    {
        estring* _this;
        Appender(estring* this_) : _this(this_) { }
        using Visitor::operator();
        void operator()(const std::string_view& x) { _this->append(x); }
        void operator()(uint64_t x)                { _this->append(x); }
    };
};

template<typename T,
         typename = typename std::enable_if<!std::is_same<T, estring>::value>::type,
         typename = typename std::enable_if<!std::is_same<T, std::string>::value>::type>
inline estring operator+(const estring& s, const T& rhs)
{
    return estring(s) += std::string_view(rhs);
}
template<typename T,
         typename = typename std::enable_if<!std::is_same<T, estring>::value>::type,
         typename = typename std::enable_if<!std::is_same<T, std::string>::value>::type>
inline estring operator+(const T& lhs, const estring& s)
{
    return estring(lhs) += std::string_view(s);
}

namespace std {
template<>
struct hash<estring_view> {
    std::hash<std::string_view> hasher;

    size_t operator()(const estring_view& x) const {
        return hasher(x);
    }
};

template<>
struct hash<estring> {
    std::hash<std::string> hasher;

    size_t operator()(const estring& x) const {
        return hasher(x);
    }
};
} // namespace std

