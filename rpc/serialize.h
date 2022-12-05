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
#include <cinttypes>
#include <cstddef>
#include <cstring>
#include <cassert>
#include <string>
#include <vector>
#include <sys/uio.h>
#include <photon/common/iovector.h>
#include <photon/common/utility.h>
#include <photon/common/checksum/crc32c.h>

namespace photon {
namespace rpc
{
    struct buffer
    {
        void* _ptr = nullptr;
        size_t _len = 0;
        buffer() { }
        buffer(void* buf, size_t size) : _ptr(buf), _len(size) { }
        void* addr() const { return _ptr; }
        size_t size() const { return _len; }
        size_t length() const { return size(); }
        void assign(const void* ptr, size_t len) { _ptr = (void*)ptr; _len = len; }
    };

    // aligned buffer, which will be processed in the 1st pass of
    // serialization / deserialization process, helping to ensure alignment
    struct aligned_buffer : public buffer
    {
    };

    template<typename T>
    struct fixed_buffer : public buffer
    {
        using buffer::buffer;
        fixed_buffer(const T* x) : buffer(x, sizeof(*x)) { }
        void assign(const T* x) { buffer::assign(x, sizeof(*x)); }
        const T* get() const { return (T*)addr(); }
    };

    template<typename T>
    struct array : public buffer
    {
        array() { }
        array(const T* ptr, size_t size) { assign(ptr, size); }
        array(const std::vector<T>& vec) { assign(vec); }
        size_t size() const { return _len / sizeof(T); }
        const T* begin() const    { return (T*)_ptr; }
        const T* end() const      { return begin() + size(); }
        const T& operator[](long i) const { return ((char*)_ptr)[i]; }
        const T& front() const    { return (*this)[0]; }
        const T& back() const     { return (*this)[(long)size() - 1]; }
        void assign(const T* x, size_t size) { buffer::assign(x, sizeof(*x) * size); }
        void assign(const std::vector<T>& vec)
        {
            assign(&vec[0], vec.size());
        }

        struct iterator {
            T* p;
            iterator(T* t):p(t) {}
            const T& operator*() {
                return *p;
            }
            iterator& operator++(int) {
                p++;
                return *this;
            }
            bool operator!=(iterator rhs) {
                return p != rhs.p;
            }
            bool operator==(iterator rhs) {
                return p == rhs.p;
            }
        };
    };

    struct string : public array<char>
    {
        using base = array<char>;
        using base::base;

        template<size_t LEN>
        string(const char (&s)[LEN]) : base(s, LEN) { }
        string(const char* s) : base(s, strlen(s)+1) { }
        string(const std::string& s) { assign(s); }
        string() : base(nullptr, 0) { }
        const char* c_str() const { return begin(); }
        operator const char* () const { return c_str(); }
        void assign(const char* s) {
            base::assign(s, strlen(s) + 1);
        }
        void assign(const std::string& s)
        {
            array<char>::assign(s.c_str(), s.size()+1);
        }
    };

    struct iovec_array : public array<iovec>
    {
        // using base = array<iovec>;
        // using base::base;
        size_t summed_size;
        iovec_array() { }
        iovec_array(iovec* iov, int iovcnt)
        {
            assign(iov, iovcnt);
        }
        size_t assign(const iovec* iov, int iovcnt)
        {
            assert(iovcnt >= 0);
            array<iovec>::assign(iov, (size_t)iovcnt);
            return sum();
        }
        size_t sum()
        {
            summed_size = 0;
            for (auto& v: *this)
                summed_size += v.iov_len;
            return summed_size;
        }
    };

    // Represents an iovec[] that has aligned iov_bases and iov_lens.
    // It will be processed in the 1st pass of serialization /
    // deserialization process, helping to ensure alignment.
    struct aligned_iovec_array : public iovec_array
    {
    };

    // structs of concrete messages MUST derive from `Message`,
    // and implement serialize_fields(), possible with SERIALIZE_FIELDS()
    struct Message
    {
    public:
        template<typename AR>
        void serialize_fields(AR& ar) {}

        void add_checksum(iovector* iov) {}

        bool validate_checksum(iovector* iov, void* body, size_t body_length) { return true; }

    protected:
        template<typename AR>
        void reduce(AR& ar)
        {
        }
        template<typename AR, typename T, typename...Ts>
        void reduce(AR& ar, T& x, Ts&...xs)
        {
            ar.process_field(x);
            reduce(ar, xs...);
        }
    };

    struct Crc32Hasher {
        using ValueType = uint32_t;

        static constexpr ValueType init_value() { return 0; };

        static void extend_hash(ValueType& value, const iovector* iov) {
            for (const auto iter : *iov)
                value = crc32c_extend(iter.iov_base, iter.iov_len, value);
        }

        static void extend_hash(ValueType& value, void* buf, size_t length) {
            value = crc32c_extend(buf, length, value);
        }
    };

    template<typename Hasher = Crc32Hasher>
    struct CheckedMessage : public Message {
    public:
        using ValueType = typename Hasher::ValueType;

        CheckedMessage() : m_checksum(Hasher::init_value()) {}

        void add_checksum(iovector* iov) {
            assert(m_checksum == Hasher::init_value());
            Hasher::extend_hash(m_checksum, iov);
        }

        bool validate_checksum(iovector* iov, void* body, size_t body_length) {
            assert(iov->sum() != 0);
            auto dst = m_checksum;
            m_checksum = Hasher::init_value();
            Hasher::extend_hash(m_checksum, iov);
            if (body != nullptr && body_length != 0)
                Hasher::extend_hash(m_checksum, body, body_length);
            if (dst != m_checksum)
                return false;
            return true;
        }

    private:
        ValueType m_checksum;
    };

#define PROCESS_FIELDS(...)                     \
        template<typename AR>                   \
        void process_fields(AR& ar) {           \
            return reduce(ar, __VA_ARGS__);     \
        }

    template<typename Derived>  // The Curiously Recurring Template Pattern (CRTP)
    class ArchiveBase
    {
    public:
        Derived* d()
        {
            return static_cast<Derived*>(this);
        }

        template<typename T>
        void process_field(T& x)
        {
        }

        void process_field(buffer& x)
        {
            assert("must be re-implemented in derived classes");
        }

        template<typename T>
        void process_field(fixed_buffer<T>& x)
        {
            static_assert(
                !std::is_base_of<Message, T>::value,
                "no Messages are allowed");

            d()->process_field((buffer&)x);
            d()->process_field(*x.get());
        }

        template<typename T>
        void process_field(array<T>& x)
        {
            static_assert(
                !std::is_base_of<Message, T>::value,
                "no Messages are allowed");

            d()->process_field((buffer&)x);
            for (auto& i: x)
                d()->process_field(i);
        }

        void process_field(string& x)
        {
            d()->process_field((buffer&)x);
        }

        void process_field(iovec_array& x)
        {
            assert("must be re-implemented in derived classes");
        }

        // overload for embedded Message
        template<typename T, ENABLE_IF_BASE_OF(Message, T)>
        void process_field(T& x)
        {
            x.serialize_fields(*d());
        }
    };

    template<typename T>
    struct _FilterAlignedFields
    {
        T* _obj;
        bool _flag;
        void process_field(aligned_buffer& x)
        {
            if (_flag)
                _obj->process_field((buffer&)x);
        }
        void process_field(aligned_iovec_array& x)
        {
            if (_flag)
                _obj->process_field((iovec_array&)x);
        }
        template<typename P>
        void process_field(P& x)
        {
            if (!_flag)
                _obj->process_field(x);
        }
    };

    template<typename T>
    _FilterAlignedFields<T> FilterAlignedFields(T* obj, bool flag)
    {
        return _FilterAlignedFields<T>{obj, flag};
    }

    class SerializerIOV : public ArchiveBase<SerializerIOV>
    {
    public:
        IOVector iov;
        bool iovfull = false;

        using ArchiveBase<SerializerIOV>::process_field;

        void process_field(buffer& x)
        {
            if (iov.back_free_iovcnt() > 0) {
                if (x.size()>0)
                    iov.push_back(x.addr(), x.size());
            } else {
                iovfull = true;
            }
        }

        void process_field(iovec_array& x)
        {
            x.summed_size = 0;
            for (auto& v: x)
            {
                x.summed_size += v.iov_len;
                buffer buf(v.iov_base, v.iov_len);
                d()->process_field(buf);
            }
        }

        template<typename T>
        void serialize(T& x)
        {
            static_assert(
                std::is_base_of<Message, T>::value,
                "only Messages are permitted");

            // serialize aligned fields, non-aligned fields, and the main body
            auto aligned = FilterAlignedFields(this, true);
            x.process_fields(aligned);
            auto non_aligned = FilterAlignedFields(this, false);
            x.process_fields(non_aligned);
            buffer msg(&x, sizeof(x));
            d()->process_field(msg);
            x.add_checksum(&iov);
        }
    };

    class DeserializerIOV : public ArchiveBase<DeserializerIOV>
    {
    public:
        iovector* _iov;
        bool failed = false;

        using ArchiveBase<DeserializerIOV>::process_field;

        void process_field(buffer& x)
        {
            if (x.size()==0)
                return;
            x._ptr = _iov->extract_front_continuous(x.size());
            if (!x._ptr)
                failed = true;
        }

        void process_field(iovec_array& x)
        {
            iovector_view v;
            ssize_t ret = _iov->extract_front(x.summed_size, &v);
            if (ret == (ssize_t)x.summed_size) {
                x.assign(v.iov, v.iovcnt);
            } else {
                failed = true;
            }
        }

        template<typename T>
        T* deserialize(iovector* iov)
        {
            static_assert(
                std::is_base_of<Message, T>::value,
                "only Messages are permitted");

            // deserialize the main body from back
            _iov=iov;
            auto t = iov -> extract_back<T>();
            if (t) {
                if (!t->validate_checksum(iov, t, sizeof(*t))) {
                    failed = true;
                    return nullptr;
                }
                // deserialize aligned fields, and non-aligned fields, from front
                auto aligned = FilterAlignedFields(this, true);
                t->process_fields(aligned);
                auto non_aligned = FilterAlignedFields(this, false);
                t->process_fields(non_aligned);
            } else {
                failed = true;
            }
            return t;
        }
    };
}
}
