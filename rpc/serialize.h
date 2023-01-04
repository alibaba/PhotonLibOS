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
#include <algorithm>
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
        T* begin() const { return (T*)_ptr; }
        T* end() const { return begin() + size(); }
        const T* cbegin() const { return begin(); }
        const T* cend() const { return end(); }
        const T& operator[](long i) const { return ((char*)_ptr)[i]; }
        const T& front() const    { return (*this)[0]; }
        const T& back() const     { return (*this)[(long)size() - 1]; }
        void assign(const T* x, size_t size) { buffer::assign(x, sizeof(*x) * size); }
        void assign(const std::vector<T>& vec) { assign(&vec[0], vec.size()); }

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
        string(const char* s, size_t len) : base(s, len) { }
        string(const std::string& s) { assign(s); }
        string() : base(nullptr, 0) { }
        const char* c_str() const { return cbegin(); }
        std::string_view sv() const { return {c_str(), size()}; }
        bool operator==(const string& rhs) const { return sv() == rhs.sv(); }
        bool operator!=(const string& rhs) const { return !(*this == rhs); }
        bool operator<(const string& rhs) const { return sv() < rhs.sv(); }
        bool operator>(const string& rhs) const { return !(*this < rhs); }
        void assign(const char* s) {
            base::assign(s, strlen(s) + 1);
        }
        void assign(const std::string& s)
        {
            array<char>::assign(s.c_str(), s.size()+1);
        }
    };

    template<typename T1, typename T2>
    struct pair {
        pair() = default;
        pair(T1 t1, T2 t2) : first(t1), second(t2) {}

        T1 first;
        T2 second;
    };

    // slice can be anchored to a base_buffer to form a new string
    class slice {
    public:
        slice() = default;

        slice(off_t off, size_t len) : offset(off), length(len) {}

        string anchor(const buffer& base_buffer) const {
            assert(offset + length <= base_buffer.size());
            return {(char*) base_buffer.addr() + offset, length};
        }

        off_t offset;
        size_t length;
    };

    inline string operator|(slice s, const buffer& base) {
        return s.anchor(base);
    }

    inline string operator|(string s, const buffer& base) {
        return s;
    }

    inline string operator|(const char* s, const buffer& base) {
        return {s, strlen(s)};
    }

    inline string operator|(std::string_view s, const buffer& base) {
        return {s.data(), s.size()};
    }

    inline string operator|(std::string s, const buffer& base) {
        return {s.data(), s.size()};
    }

    inline string operator|(pair<slice, slice> p, const buffer& base) {
        return p.first.anchor(base);
    }

    // A function object to compare rpc::slice, rpc::string, std::string, const char*, and so on.
    // Some of them may need to be anchored to a base_buffer
    class base_buffer_cmp {
    public:
        explicit base_buffer_cmp(buffer* base_buffer) : m_base_buffer(base_buffer) {}

        template<typename A, typename B>
        bool operator()(const A& a, const B& b) {
            assert(m_base_buffer);
            return (a | *m_base_buffer) < (b | *m_base_buffer);
        }

    private:
        buffer* m_base_buffer;
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

    template<typename K, typename V>
    struct sorted_map;

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

        template<typename K, typename V>
        void process_field(sorted_map<K, V>& x) {
            d()->process_field(x.index);
            d()->process_field(x.base_buffer);
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

    // The actual protocol struct sent in an RPC message
    template<typename K, typename V>
    struct sorted_map {
        using KeyType = slice;
        using ValueType = pair<slice, slice>;
        using MappedType = slice;
        using KeyCompare = base_buffer_cmp;
        using Index = array<ValueType>;

        struct Iterator {
            using iterator_category = std::forward_iterator_tag;
            using difference_type   = std::ptrdiff_t;

            Iterator(ValueType* ptr, buffer* base_buffer) : m_ptr(ptr), m_base_buffer(base_buffer) {}

            pair<K, V>& operator*() { deserialize(); return m_pair; }
            pair<K, V>* operator->() { deserialize(); return &m_pair; }
            Iterator& operator++() { m_ptr++; return *this; }
            Iterator operator++(int) { Iterator tmp = *this; ++(*this); return tmp; }
            friend bool operator== (const Iterator& a, const Iterator& b) { return a.m_ptr == b.m_ptr; };
            friend bool operator!= (const Iterator& a, const Iterator& b) { return a.m_ptr != b.m_ptr; };

        private:
            void deserialize() {
                photon::rpc::string s = m_ptr->second | *m_base_buffer;
                IOVector iov;
                iov.push_back(s.addr(), s.size());
                DeserializerIOV deserializer;
                auto* t = deserializer.deserialize<V>(&iov);

                m_pair.first = m_ptr->first | *m_base_buffer;
                m_pair.second = *t;
            }

            ValueType* m_ptr;
            buffer* m_base_buffer;
            pair<K, V> m_pair;
        };

        Iterator begin() { return Iterator(index.begin(), &base_buffer); }

        Iterator end() { return Iterator(index.end(), &base_buffer); }

        Iterator find(const K& k) {
            auto iter = std::lower_bound(index.begin(), index.end(), k, KeyCompare(&base_buffer));
            return Iterator(iter, &base_buffer);
        }

        Index index;
        buffer base_buffer;
    };

    template<typename K, typename V>
    class sorted_map_factory {
    public:
        using SortedMap = sorted_map<K, V>;

        ~sorted_map_factory() {
            if (m_flat_buffer)
                free(m_flat_buffer);
        }

        // Append key and value into a temp iov, and append the index
        void append(const K& k, const V& v) {
            static_assert(std::is_base_of<photon::rpc::string, K>::value, "Only support rpc::string as key for now");
            static_assert(std::is_base_of<Message, V>::value, "The value must be a RPC message");

            off_t offset = m_iov.sum();
            m_iov.push_back(k.addr(), k.size() + 1);
            typename SortedMap::KeyType key_slice(offset, k.size());

            offset = m_iov.sum();
            SerializerIOV serializer;
            serializer.serialize((V&) v);
            size_t value_size = serializer.iov.sum();
            for (auto each : serializer.iov) {
                m_iov.push_back(each.iov_base, each.iov_len);
            }
            typename SortedMap::MappedType val_slice(offset, value_size);

            m_index.emplace_back(key_slice, val_slice);
        }

        // Flatten the iov to a buffer, and bind the external sorted_map with this buffer.
        // Should be used in send side.
        void assign_to(SortedMap* sm) {
            if (m_index.empty())
                return;
            m_flat_buffer = malloc(m_iov.sum());
            m_iov.memcpy_to(m_flat_buffer, m_iov.sum());
            sm->base_buffer.assign(m_flat_buffer, m_iov.sum());
            std::sort(m_index.begin(), m_index.end(), typename SortedMap::KeyCompare(&sm->base_buffer));
            sm->index.assign(m_index);
        }

    protected:
        std::vector<typename SortedMap::ValueType> m_index;
        IOVector m_iov;
        void* m_flat_buffer = nullptr;
    };

}
}
