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

/*
 `iovector` is an array of iovecs, which features:
 (1) useful functions and operators;
 (2) intuitive and easy-to-use interface;
 (3) efficient design and implementation;
 (4) fixed-capacity after construction;
 (5) stack-allocation, as well as heap-allocation, for the iovec array itself;
 (6) flexible buffer space memory management for the `iovec::iov_base` part;
 (7) correct buffer space de-allocation, even in cases of:
     (a) modification to iov_bases and iov_lens;
     (b) inserting stack-allocated buffers
*/

#pragma once
#include <cstdlib>
#include <climits>
#include <cassert>
#include <cstring>
#include <new>
#include <memory>
#include <sys/uio.h>

#include <photon/common/callback.h>
#include <photon/common/io-alloc.h>

inline bool operator == (const iovec& a, const iovec& b)
{
    return a.iov_base == b.iov_base && a.iov_len == b.iov_len;
}

// represents the view of iovec[], providing lots of operations
struct iovector_view
{
    iovec* iov;
    int iovcnt;

    constexpr iovector_view() : iov(0), iovcnt(0) { }

    explicit constexpr
    iovector_view(iovec* iov, int iovcnt) : iov(iov), iovcnt(iovcnt) { }

    template<int N> explicit constexpr
    iovector_view(iovec (&iov)[N]) : iov(iov), iovcnt(N) { }

    void assign(iovec* iov, int iovcnt)
    {
        this->iov = iov;
        this->iovcnt = iovcnt;
    }

    template<int N>
    void assign(iovec (&iov)[N])
    {
        this->iov = iov;
        this->iovcnt = N;
    }

    bool empty()     { return iovcnt == 0; }
    iovec* begin()   { return iov; }
    iovec& front()   { return *iov; }
    iovec* end()     { return iov + iovcnt; }
    iovec& back()    { return iov[iovcnt - 1]; }
    void pop_front() { assert(iovcnt > 0); iov++; iovcnt--; }
    void pop_back()  { assert(iovcnt > 0); iovcnt--; }
    iovec& operator[](size_t i)   { return iov[i]; }
    size_t elements_count() const { return iovcnt; }
    const iovec* begin() const    { return iov; }
    const iovec& front() const    { return *iov; }
    const iovec* end() const      { return iov + iovcnt; }
    const iovec& back() const     { return iov[iovcnt - 1]; }
    const iovec& operator[](size_t i) const { return iov[i]; }

    bool operator==(const iovector_view& rhs) const
    {
        return iov == rhs.iov && iovcnt == rhs.iovcnt;
    }

    // returns total bytes summed through iov[0..iovcnt)
//    size_t size() const;
    size_t sum() const;

    // try to extract some bytes from back, to shrink size to `size`
    // return shrinked size, which may be equal to oringinal size
    size_t shrink_to(size_t size);

    // try to decrease iovcnt without modifying content in iov[]
    // to the maximum possible number such that sum() <= `size`
    size_t shrink_less_than(size_t size);

    // try to extract `bytes` bytes from the front
    // return # of bytes actually extracted
    size_t extract_front(size_t bytes);

    // try to extract `bytes` bytes from front elemnt (iov[0])
    // return the pointer if succeeded, or nullptr otherwise
    void* extract_front_continuous(size_t bytes)
    {
        auto& f = front();
        if (empty() || f.iov_len < bytes)
            return nullptr;

        f.iov_len -= bytes;
        auto rst = f.iov_base;
        (char*&)f.iov_base += bytes;
        if (f.iov_len == 0)
            pop_front();
        return rst;
    }

    // try to `extract_front(bytes)` and copy the extracted bytes to `buf`
    // return # of bytes actually extracted
    size_t extract_front(size_t bytes, void* buf);

    // try to extract `bytes` bytes from front in the form of
    // iovector_view stored in `iov`, without copying data
    // return # of bytes actually extracted, or -1 if `iov` has not enough iovs[] space
    ssize_t extract_front(size_t bytes, iovector_view* iov);

    // try to extract `bytes` bytes from the back
    // return # of bytes actually extracted
    size_t extract_back(size_t bytes);

    // try to extract `bytes` bytes from the back elemnt (iov[n-1])
    // return the pointer if succeeded, or nullptr otherwise
    void* extract_back_continuous(size_t bytes)
    {
        auto& b = back();
        if (empty() || b.iov_len < bytes)
            return nullptr;

        b.iov_len -= bytes;
        void* ret = (char*)b.iov_base + b.iov_len;
        if (b.iov_len == 0)
            pop_back();
        return ret;
    }

    // try to `extract_back(bytes)` and copy the extracted bytes to `buf`
    // return # of bytes actually extracted
    size_t extract_back(size_t bytes, void* buf);

    // try to extract `bytes` bytes from the back in the form of
    // iovector_view stored in `iov`, without copying data
    // return # of bytes actually extracted, or -1 if `iov` has not enough iovs[] space
    ssize_t extract_back(size_t bytes, iovector_view* iov);

    // copy data to a buffer of size `size`,
    // return # of bytes actually copied
    size_t memcpy_to(void* buf, size_t size);

    // copy data from a buffer of size `size`,
    // return # of bytes actually copied
    size_t memcpy_from(const void* buf, size_t size);

    // copy data to a iovector_view of size `size`
    // return # of bytes actually copied
    size_t memcpy_to(iovector_view* iov, size_t size=SIZE_MAX) {
        return memcpy_iov(iov, this, size);
    }

    // copy data from a iovector_view of size `size`
    // return # of bytes actually copied
    size_t memcpy_from(iovector_view* iov, size_t size=SIZE_MAX) {
        return memcpy_iov(this, iov, size);
    }

    // copy data to a iovector_view of size `size`
    // `iov` will not change after copy
    // return # of bytes actually copied
    size_t memcpy_to(const iovector_view* iov, size_t size=SIZE_MAX) {
        return iov->memcpy_from(this, size);
    }

    // copy data from a iovector_view of size `size`
    // `iov` will not change after copy
    // return # of bytes actually copied
    size_t memcpy_from(const iovector_view* iov, size_t size=SIZE_MAX) {
        return iov->memcpy_to(this, size);
    }

    // copy data to a iovector_view of size `size`
    // the iovector_view itself will not change after copy
    // return # of bytes actually copied
    size_t memcpy_to(iovector_view* iov, size_t size=SIZE_MAX) const;

    // copy data from a iovector_view of size `size`
    // the iovector_view itself will not change after copy
    // return # of bytes actually copied
    size_t memcpy_from(iovector_view* iov, size_t size=SIZE_MAX) const;

    // copy data to a iovector_view of size `size`
    // both source and target iovectors will not change after copy
    // return # of bytes actually copied
    size_t memcpy_to(const iovector_view* iov, size_t size=SIZE_MAX) const;

    // copy data from a iovector_view of size `size`
    // both source and target iovectors will not change after copy
    // return # of bytes actually copied
    size_t memcpy_from(const iovector_view* iov, size_t size=SIZE_MAX) const;

    // generate iovector_view of partial data
    // generated view shares data field, but has
    // its own iovec array.
    ssize_t slice(size_t count, off_t offset, iovector_view* /*OUT*/ iov) const;

protected:
    // copy data from a iovector_view to another one
    // return # of bytes actually copied
    static size_t memcpy_iov(iovector_view* dest, iovector_view* src, size_t size);

};

#define IF_ASSERT_RETURN(cond, ret) \
    if (cond) { assert(cond); }     \
    else { return (ret); }

class iovector;
inline iovector* new_iovector(uint16_t capacity, uint16_t preserve);
inline void delete_iovector(iovector* ptr);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"

class iovector
{
public:
    struct iovec& front()
    {
        do_assert();
        return *(iovs_ptr() + iov_begin);
    }
    const struct iovec& front() const
    {
        do_assert();
        return *(iovs_ptr() + iov_begin);
    }
    struct iovec& back()
    {
        do_assert();
        return *(iovs_ptr() + (iov_end - 1));
    }
    const struct iovec& back() const
    {
        do_assert();
        return *(iovs_ptr() + (iov_end - 1));
    }
    size_t sum() const
    {
        do_assert();
        return view().sum();
    }
    struct iovec* iovec()
    {
        do_assert();
        return iovs_ptr() + iov_begin;
    }
    const struct iovec* iovec() const
    {
        do_assert();
        return iovs_ptr() + iov_begin;
    }
    uint16_t iovcnt() const
    {
        do_assert();
        return iov_end -  iov_begin;
    }
    // use iovcnt() instead
//    uint16_t size() const
//    {
//        return iovcnt();
//    }
    uint16_t front_free_iovcnt() const
    {
        do_assert();
        return iov_begin;
    }
    uint16_t back_free_iovcnt() const
    {
        do_assert();
        return capacity - iov_end;
    }
    bool empty() const
    {
        do_assert();
        return iov_begin >= iov_end;
    }
    void resize(uint16_t nn)
    {
        iov_end = iov_begin + nn;
        assert(iov_end <= capacity);
    }
    struct iovec* begin()
    {
        do_assert();
        return iovs + iov_begin;
    }
    const struct iovec* begin() const
    {
        do_assert();
        return iovs + iov_begin;
    }
    struct iovec* end()
    {
        do_assert();
        return iovs + iov_end;
    }
    const struct iovec* end() const
    {
        do_assert();
        return iovs + iov_end;
    }
    struct iovec& operator[] (int64_t i)
    {
        do_assert();
        assert(iov_begin + i < iov_end);
        return iovs[iov_begin + i];
    }
    const struct iovec& operator[] (int64_t i) const
    {
        do_assert();
        assert(iov_begin + i < iov_end);
        return iovs[iov_begin + i];
    }

    // push some stuff to the front, and
    // return # of bytes actually pushed
    size_t push_front(struct iovec iov)
    {
        do_assert();
        IF_ASSERT_RETURN(iov_begin > 0, 0);
        iovs[--iov_begin] = iov;
        return iov.iov_len;
    }
    size_t push_front(void* buf, size_t size)
    {
        return push_front({buf, size});
    }
    // alloc an buffer with IOVAllocation, and push the buffer to the front;
    // note that the buffer may be constituted by multiple iovec elements;
    size_t push_front(size_t bytes)
    {
        auto v = new_iovec(bytes);
        IF_ASSERT_RETURN(v.iov_len, 0);
        if (push_front(v) == bytes)
            return bytes;
        return v.iov_len + push_front_more(bytes - v.iov_len);
    }


    // push some stuff to the back, and
    // return # of bytes actually pushed
    size_t push_back(struct iovec iov)
    {
        do_assert();
        IF_ASSERT_RETURN(iov_end < capacity, 0);
        iovs[iov_end++] = iov;
        return iov.iov_len;
    }
    size_t push_back(void* buf, size_t size)
    {
        return push_back({buf, size});
    }
    // alloc an buffer with IOVAllocation, and push the buffer to the back;
    // note that the buffer may constitute multiple iovec elements;
    size_t push_back(size_t bytes)
    {
        auto v = new_iovec(bytes);
        IF_ASSERT_RETURN(v.iov_len, 0);
        if (push_back(v) == bytes)
            return bytes;
        return v.iov_len + push_back_more(bytes - v.iov_len);
    }

    // pop an struct iovec element, and
    // return the # of bytes actually popped
    size_t pop_front()
    {
        do_assert();
        IF_ASSERT_RETURN(!empty(), 0);
        return iovs[iov_begin++].iov_len;
    }
    size_t pop_back()
    {
        do_assert();
        IF_ASSERT_RETURN(!empty(), 0);
        return iovs[--iov_end].iov_len;
    }

    void clear()    // doesn't dispose()
    {
        do_assert();
        iov_end = iov_begin;
    }
    void clear_dispose()
    {
        dispose();
    }

    // try to extract some bytes from back, to shrink size to `size`
    // return shrinked size, which may be equal to oringinal size
    size_t shrink_to(size_t size)
    {
        auto va = view();
        auto ret = va.shrink_to(size);
        if (ret == size)
        {
            iov_end = iov_begin + va.iovcnt;
        }
        return ret;
    }

    // resize the # of bytes, by either poping-back or pushing-back
    size_t truncate(size_t size)
    {
        if (size == sum())
            return size;
        auto ret = shrink_to(size);
        if (ret == size)
            return size;

        return ret + push_back(size - ret);
    }

    // try to extract `bytes` bytes from the front
    // return # of bytes actually extracted
    size_t extract_front(size_t bytes)
    {
        auto va = view();
        auto ret = va.extract_front(bytes);
        iov_begin = iov_end - va.iovcnt;
        return ret;
    }

    // try to `extract_front(bytes)` and copy the extracted bytes to `buf`
    // return # of bytes actually extracted
    size_t extract_front(size_t bytes, void* buf)
    {
        auto va = view();
        auto ret = va.extract_front(bytes, buf);
        iov_begin = iov_end - va.iovcnt;
        return ret;
    }

    // try to extract `bytes` bytes from the front in the form of
    // iovector_view stored in `iov`, without copying data;
    // `iov` will be internally malloced and will be freed if it is empty
    // or will use exists iovec array.
    // return # of bytes actually extracted, or -1 if internal alloc failed
    ssize_t extract_front(size_t bytes, iovector_view* /*OUT*/ iov)
    {
        if (!bytes)
            return 0;
        if (iov->iovcnt == 0) {
            size_t size = iovcnt() * sizeof(struct iovec);
            auto ptr = (struct iovec*)this->do_malloc(size);
            if (!ptr)
                return -1;
            iov->assign(ptr, iovcnt());
        }
        auto va = view();
        auto ret = va.extract_front(bytes, iov);
        iov_begin = iov_end - va.iovcnt;
        return ret;
    }

    // try to extract `bytes` bytes from front in the form of
    // iovector stored in `iov`, without copying data;
    // `iov` will use its own iovec array,
    // return # of bytes actually extracted, or -1 if failed
    ssize_t extract_front(size_t bytes, iovector* iov) {
        if (iov == nullptr)
            return -1;
        if (!bytes)
            return 0;
        iov->resize(iovcnt());
        auto vi = iov->view();
        auto va = view();
        auto ret = va.extract_front(bytes, &vi);
        iov_begin = iov_end - va.iovcnt;
        if (ret >= 0) {
            iov->update(vi);
        }
        return ret;
    }

    // try to extract an object of type `T` from front, possibly copying
    template<typename T>
    T* extract_front()
    {
        return (T*)extract_front_continuous(sizeof(T));
    }

    // try to extract `bytes` bytes from front elemnt (iov[0])
    // return the pointer if succeeded, or nullptr otherwise (alloc
    // failed, or not enough data, etc.)
    void* extract_front_continuous(size_t bytes)
    {
        auto va = view();
        auto ptr = va.extract_front_continuous(bytes);
        if (ptr) {
            update(va);
            return ptr;
        }

        auto buf = do_malloc(bytes);
        auto ret = extract_front(bytes, buf);
        return ret == bytes ?
            buf :
            nullptr;
    }

    // try to extract `bytes` bytes from the back
    // return # of bytes actually extracted
    size_t extract_back(size_t bytes)
    {
        auto va = view();
        auto ret = va.extract_back(bytes);
        iov_end = iov_begin + va.iovcnt;
        return ret;
    }

    // extract some `bytes` from the back without copying the data, in the
    // form of `iovec_array`, which is internally malloced and will be freed
    // when *this destructs, or disposes.
    // return # of bytes actually extracted, may be < `bytes` if not enough in *this
    // return -1 if malloc failed
    size_t extract_back(size_t bytes, void* buf)
    {
        auto va = view();
        auto ret = va.extract_back(bytes, buf);
        iov_end = iov_begin + va.iovcnt;
        return ret;
    }

    // try to extract `bytes` bytes from back in the form of
    // iovector_view stored in `iov`, without copying data;
    // `iov` will be internally malloced and will be freed if it is empty
    // or will use exists iovec array.
    // return # of bytes actually extracted, or -1 if internal alloc failed
    ssize_t extract_back(size_t bytes, iovector_view* iov)
    {
        if (!bytes)
            return 0;
        if (iov->iovcnt == 0) {
            size_t size = iovcnt() * sizeof(struct iovec);
            auto ptr = (struct iovec*)this->do_malloc(size);
            if (!ptr)
                return -1;
            iov->assign(ptr, iovcnt());
        }
        auto va = view();
        auto ret = va.extract_back(bytes, iov);
        iov_end = iov_begin + va.iovcnt;
        return ret;
    }

    // try to extract `bytes` bytes from back in the form of
    // iovector stored in `iov`, without copying data;
    // `iov` will use its own iovec array,
    // return # of bytes actually extracted, or -1 if failed
    ssize_t extract_back(size_t bytes, iovector* iov) {
        if (iov == nullptr)
            return -1;
        if (!bytes)
            return 0;
        iov->resize(iovcnt());
        auto vi = iov->view();
        auto va = view();
        auto ret = va.extract_back(bytes, &vi);
        iov_end = iov_begin + va.iovcnt;
        if (ret >= 0) {
            iov->update(vi);
        }
        return ret;
    }

    // try to extract an object of type `T` from back, possibly copying
    template<typename T>
    T* extract_back()
    {
        return (T*)extract_back_continuous(sizeof(T));
    }

    // `extract_back(bytes)` and return the starting address, guaranteeing it's a
    // continuous buffer, possiblily by memcpying to a internal allocated new buffer
    // return nullptr if not enough data in the iovector
    void* extract_back_continuous(size_t bytes)
    {
        auto va = view();
        auto ptr = va.extract_back_continuous(bytes);
        if (ptr) {
            update(va);
            return ptr;
        }

        auto buf = do_malloc(bytes);
        auto ret = extract_back(bytes, buf);
        return ret == bytes ?
            buf :
            nullptr;
    }

    // copy data to a buffer of size `size`,
    // return # of bytes actually copied
    size_t memcpy_to(void* buf, size_t size)
    {
        return view().memcpy_to(buf, size);
    }

    // copy data from a buffer of size `size`,
    // return # of bytes actually copied
    size_t memcpy_from(const void* buf, size_t size)
    {
        return view().memcpy_from(buf, size);
    }

    // copy data to a iovector_view of size `size`
    // return # of bytes actually copied
    size_t memcpy_to(iovector_view* iov, size_t size=SIZE_MAX)
    {
        return view().memcpy_to(iov, size);
    }

    // copy data from a iovector_view of size `size`
    // return # of bytes actually copied
    size_t memcpy_from(iovector_view* iov, size_t size=SIZE_MAX)
    {
        return view().memcpy_from(iov, size);
    }

    // copy data to a iovector_view of size `size`
    // the iovector_view itself will not change after copy
    // return # of bytes actually copied
    size_t memcpy_to(iovector_view* iov, size_t size=SIZE_MAX) const
    {
        const auto v = view();
        return v.memcpy_to(iov, size);
    }

    // copy data from a iovector_view of size `size`
    // the iovector_view itself will not change after copy
    // return # of bytes actually copied
    size_t memcpy_from(iovector_view* iov, size_t size=SIZE_MAX) const
    {
        const auto v = view();
        return v.memcpy_from(iov, size);
    }

    // copy data to a iovector_view of size `size`
    // `iov` will not change after copy
    // return # of bytes actually copied
    size_t memcpy_to(const iovector_view* iov, size_t size=SIZE_MAX)
    {
        auto v = view();
        return iov->memcpy_from(&v, size);
    }

    // copy data from a iovector_view of size `size`
    // `iov` will not change after copy
    // return # of bytes actually copied
    size_t memcpy_from(const iovector_view* iov, size_t size=SIZE_MAX)
    {
        auto v = view();
        return iov->memcpy_to(&v, size);
    }

    // copy data to a iovector_view of size `size`
    // both source and target iovectors will not change after copy
    // return # of bytes actually copied
    size_t memcpy_to(const iovector_view* iov, size_t size=SIZE_MAX) const
    {
        const auto v = view();
        return v.memcpy_to(iov, size);
    }

    // copy data from a iovector_view of size `size`
    // both source and target iovectors will not change after copy
    // return # of bytes actually copied
    size_t memcpy_from(const iovector_view* iov, size_t size=SIZE_MAX) const
    {
        const auto v = view();
        return v.memcpy_from(iov, size);
    }

    // copy data to a iovector of size `size`
    // return # of bytes actually copied
    size_t memcpy_to(iovector* iov, size_t size=SIZE_MAX) {
        auto v = iov->view();
        return memcpy_to(&v, size);
    }

    // copy data from a iovector of size `size`
    // return # of bytes actually copied
    size_t memcpy_from(iovector* iov, size_t size=SIZE_MAX) {
        auto v = iov->view();
        return memcpy_from(&v, size);
    }

    // copy data to a iovector of size `size`
    // iovector itself will not change
    // return # of bytes actually copied
    size_t memcpy_to(iovector* iov, size_t size=SIZE_MAX) const {
        auto v = iov->view();
        return ((const iovector*)(this))->memcpy_to(&v, size);
    }

    // copy data from a iovector of size `size`
    // iovector itself will not change
    // return # of bytes actually copied
    size_t memcpy_from(iovector* iov, size_t size=SIZE_MAX) const {
        auto v = iov->view();
        return ((const iovector*)(this))->memcpy_from(&v, size);
    }

    // copy data to a iovector of size `size`
    // parameter iovector will not change
    // return # of bytes actually copied
    size_t memcpy_to(const iovector* iov, size_t size=SIZE_MAX) {
        return iov->memcpy_from(this, size);
    }

    // copy data from a iovector of size `size`
    // parameter iovector will not change
    // return # of bytes actually copied
    size_t memcpy_from(const iovector* iov, size_t size=SIZE_MAX) {
        return iov->memcpy_to(this, size);
    }

    // copy data to a iovector of size `size`
    // both iovector will not change
    // return # of bytes actually copied
    size_t memcpy_to(const iovector* iov, size_t size=SIZE_MAX) const {
        auto v = iov->view();
        const auto *p = &v;
        return memcpy_to(p, size);
    }

    // copy data from a iovector of size `size`
    // both iovector will not change
    // return # of bytes actually copied
    size_t memcpy_from(const iovector* iov, size_t size=SIZE_MAX) const {
        auto v = iov->view();
        const auto *p = &v;
        return memcpy_from(p, size);
    }

    void operator = (const iovector& rhs) = delete;
    void operator = (iovector&& rhs)
    {
        if (this == &rhs)
            return;

        do_assert();
        auto end = iov_begin + rhs.iovcnt();
        IF_ASSERT_RETURN(end <= capacity, (void)0);
        clear_dispose();
        iov_end = end;

        memcpy(begin(), rhs.begin(), iovcnt() * sizeof(iovs[0]));
        allocator()->copy(rhs.allocator(), rhs.nbases);
        nbases = rhs.nbases;
        rhs.nbases = 0;
        rhs.clear(); // but not dispose()
    }

    // allocate a buffer with the internal allocator
    void* malloc(size_t size)
    {
        return do_malloc(size);
    }

    iovector_view view() const
    {
        return iovector_view((struct iovec*)iovec(), iovcnt());
    }

    void update(iovector_view va) {
        iov_begin = va.iov - iovs;
        iov_end = iov_begin + va.iovcnt;
    }

    // generate iovector_view of partial data part in the iovector
    // generated view shares data field of this iovector, but has
    // its own iovec array.
    // the iovec array of generated view will be release after iovector
    // destructed, never use them after iovector destructed.
    ssize_t slice(size_t count, off_t offset, iovector_view* /*OUT*/ iov)
    {
        if (count == 0) // empty slice
            return 0;
        if (iov->iovcnt == 0) {
            auto ptr = (struct iovec*)do_malloc(iovcnt() * sizeof(struct iovec));
            if (!ptr) {
                // failed to allocate memory
                return 0;
            }
            iov->iov = ptr;
            iov->iovcnt = iovcnt();
        }
        return view().slice(count, offset, iov);
    }

    IOAlloc* get_allocator()
    {
        return allocator();
    }

    void debug_print();

protected:
    uint16_t capacity;              // total capacity
    uint16_t iov_begin, iov_end;    // [iov_begin, iov_end)
    uint16_t nbases;                // # of allocated pointers
    struct iovec iovs[0];

    friend iovector* new_iovector(uint16_t capacity, uint16_t preserve);
    friend void delete_iovector(iovector* ptr);

    void do_assert() const
    {
        assert(iov_begin <= iov_end);
        assert(iov_end <= capacity);
        assert(nbases <= capacity);
    }

    inline __attribute__((always_inline))
    struct iovec* iovs_ptr() const {
        return const_cast<struct iovec*>(iovs);
    }

    // not allowed to freely construct / destruct
    iovector(uint16_t capacity, uint16_t preserve)
    {
        this->capacity = capacity;
        iov_begin = iov_end = preserve;
        nbases = 0;
    }
    ~iovector()
    {
        dispose();
    }

    size_t push_front_more(size_t bytes);
    size_t push_back_more(size_t bytes);

    struct IOVAllocation_ : public IOAlloc
    {
        void* bases[0];
        void* do_allocate(int size, uint16_t& nbases, uint16_t capacity)
        {
            return do_allocate(size, size, nbases, capacity);
        }
        void* do_allocate(int size_min, int& size_max, uint16_t& nbases, uint16_t capacity)
        {
            assert(allocate);
            if (nbases >= capacity) {
                errno = ENOBUFS;
                return nullptr;
            }
            void* ptr = nullptr;
            int ret = allocate({size_min, size_max}, &ptr);
            size_max = ret;
            void **pbase = bases;
            if (ret >= size_min)
                pbase[nbases++] = ptr;
            return ptr;
        }
        void dispose(uint16_t& nbases)
        {
            void **pbase = bases;
            for (uint16_t i = 0; i < nbases; ++i)
                deallocate(pbase[i]);
            nbases = 0;
        }
        void copy(IOAlloc* rhs, uint16_t nbases)
        {
            auto rhs_ = (IOVAllocation_*)rhs;
            *this = *rhs_;
            memcpy(bases, rhs_->bases, sizeof(bases[0]) * nbases);
        }
    };

    IOVAllocation_* allocator() const
    {   // this makes an assumption of the memory layout as `IOVectorEntity`
        auto addr = (char*)this + sizeof(*this) +sizeof(iovs[0]) * capacity;
        return (IOVAllocation_*)addr;
    }
    void dispose()
    {
        do_assert();
        allocator()->dispose(nbases);
        clear();
    }
    void* do_malloc(size_t size)
    {
        do_assert();
        assert(nbases < capacity);
        assert(size <= INT_MAX);
        return allocator()->do_allocate((int)size, nbases, capacity);
    }
    struct iovec new_iovec(size_t size_)
    {
        do_assert();
        assert(nbases < capacity);
        int size = (size_ <= INT_MAX) ?
            (int)size_ :
            INT_MAX;
        auto ptr = allocator()->do_allocate(1, size, nbases, capacity);
        if (ptr) {
            return {ptr, (size_t)size};
        }
        return {nullptr, 0};
    }
};

#pragma GCC diagnostic pop

// the final class for stack-allocation
template<uint16_t CAPACITY, uint16_t DEF_PRESERVE>
class IOVectorEntity : public iovector
{
public:
    struct iovec iovs[CAPACITY]; // same address with iovector::iovs
    struct IOAlloc allocator;
    void* bases[CAPACITY];       // same address with IOVAllocation::bases

    enum { capacity = CAPACITY };
    enum { default_preserve = DEF_PRESERVE };

    explicit IOVectorEntity(uint16_t preserve = DEF_PRESERVE) :
        iovector(CAPACITY, preserve) { }

//    explicit IOVectorEntity(const void* buf, size_t size,
//                            uint16_t preserve = DEF_PRESERVE) :
//        iovector(CAPACITY, preserve)
//    {
//        push_back({(void*)buf, size});
//    }

    explicit IOVectorEntity(const struct iovec* iov, int iovcnt,
                            uint16_t preserve = DEF_PRESERVE) :
        iovector(CAPACITY, preserve)
    {
        assert(iovcnt >= 0);
        assert(iovcnt + preserve < CAPACITY);
        memcpy(begin(), iov, iovcnt * sizeof(*iov));
        iov_end += iovcnt;
    }

    explicit IOVectorEntity(IOAlloc allocator,
                            uint16_t preserve = DEF_PRESERVE) :
        iovector(CAPACITY, preserve), allocator(allocator)
    {
    }

    IOVectorEntity(const IOVectorEntity& rhs) = delete;
    IOVectorEntity(const iovector& rhs) = delete;
    void operator=(const IOVectorEntity& rhs) = delete;
    void operator=(const iovector& rhs) = delete;

    IOVectorEntity(IOVectorEntity&& rhs) :
        iovector(CAPACITY, DEF_PRESERVE)
    {
        *(iovector*)this = std::move(rhs);
    }
    IOVectorEntity(iovector&& rhs) :
        iovector(CAPACITY, DEF_PRESERVE)
    {
        *(iovector*)this = std::move(rhs);
    }
    void operator = (IOVectorEntity&& rhs)
    {
        if (this != &rhs)
            *(iovector*)this = std::move(rhs);
    }
    void operator = (iovector&& rhs)
    {
        if (this != &rhs)
            *(iovector*)this = std::move(rhs);
    }
};

inline size_t iovector_view::memcpy_to(iovector_view* iov, size_t size) const {
    IOVectorEntity<32, 0> co_iov(this->iov, iovcnt);
    return co_iov.view().memcpy_to(iov, size);
}

inline size_t iovector_view::memcpy_from(iovector_view* iov, size_t size) const {
    IOVectorEntity<32, 0> co_iov(this->iov, iovcnt);
    return co_iov.view().memcpy_from(iov, size);
}

inline size_t iovector_view::memcpy_to(const iovector_view* iov, size_t size) const {
    IOVectorEntity<32, 0> co_iov(this->iov, iovcnt);
    return co_iov.view().memcpy_from(iov, size);
}

inline size_t iovector_view::memcpy_from(const iovector_view* iov, size_t size) const {
    IOVectorEntity<32, 0> co_iov(this->iov, iovcnt);
    return co_iov.view().memcpy_from(iov, size);
}


// to make sure the memory layout assumption
// in iovector::allocator() is correct
inline void do_static_assert()
{
#ifdef __CLANG__
    typedef IOVectorEntity<32, 4> IOV;
    static_assert(offsetof(IOV, iovs) == offsetof(iovector_base, iovs), "offset assumption");
    static_assert(offsetof(IOV, allocator) + offsetof(IOAlloc, bases) ==
                  offsetof(IOV, bases),  "offset assumption");
#endif
}

typedef IOVectorEntity<32, 4> IOVector;

inline iovector* new_iovector(uint16_t capacity, uint16_t preserve)
{
    auto size = sizeof(iovector)      + sizeof(iovec) * capacity +
                sizeof(IOAlloc) + sizeof(void*) * capacity ;
    auto ptr = ::malloc(size);
    return new (ptr) iovector(capacity, preserve);
}

inline void delete_iovector(iovector* ptr)
{
    ptr->dispose();
    free(ptr);
}

// Allocate io-vectors on heap if CAPACITY is not enough,
// otherwise just copy them into array.
template<size_t CAPACITY>
class SmartCloneIOV
{
public:
    iovec iov[CAPACITY];
    iovec* ptr;
    SmartCloneIOV(const iovec* iov, int iovcnt)
    {
        ptr = (iovcnt <= (int)CAPACITY) ?
            this->iov :
            new iovec[iovcnt];
        memcpy(ptr, iov, iovcnt * sizeof(*iov));
    }
    ~SmartCloneIOV()
    {
        if (ptr != iov)
            delete [] ptr;
    }
};


#undef IF_ASSERT_RETURN

