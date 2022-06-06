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
#include <cassert>
#include <photon/common/utility.h>

// 本文件主打（文件偏移量）区间的分解操作，支持固定间隔（struct range_split）,
// 以及2的幂次固定间隔（struct range_split_power2）

// 变长间隔在range-split-vi.h中支持(struct range_split_vi)

namespace photon {
namespace fs
{
    struct sub_range
    {
        uint64_t i;         // i-th block
        uint64_t offset;    // offset within the block
        uint64_t length;
        sub_range() {
            assign(0, 0, 0);
        }
        sub_range(const sub_range& rhs) {
            assign(rhs.i, rhs.offset, rhs.length);
        }
        sub_range(uint64_t i, uint64_t offset, uint64_t length)
        {
            assign(i, offset, length);
        }
        void assign(uint64_t i, uint64_t offset, uint64_t length)
        {
            this->i = i;
            this->offset = offset;
            this->length = length;
        }
        uint64_t begin() const { return offset; }
        uint64_t end()   const { return offset + length; }
        operator bool()  const { return length > 0; }
        void clear()    { length = 0; }
    };

    // split a range by a fixed interval
    template<typename Derived>
    struct basic_range_split
    {
        uint64_t begin, end;
        uint64_t abegin, aend;      // aligned begin & end (sub-range index)
        uint64_t apbegin, apend;    // aligned parts begin & end (sub-range index)
        uint64_t begin_remainder, end_remainder;

        // the only one sub-range, which has un-aligned begin and end;
        // may not exist;
        sub_range small_note;

        // the first sub-range that has un-aligned begin and aligned end
        // may not exist; undefined if there's small_note();
        sub_range preface;

        // the `first` is preface if it exists, otherwise the 1st aligned parts
        sub_range first;

        // the last sub-range, which has aligned begin and un-aligned end;
        // may not exist; undefined if there's small_not();
        sub_range postface;

        // whether both offset and length are aligned
        bool is_aligned() const
        {
            return begin_remainder == 0 && end_remainder == 0;
        }

        // whether x is aligned
        bool is_aligned(uint64_t x) const
        {
            uint64_t down, rem, up;
            divide(x, down, rem, up);
            return multiply(down) == x;
        }

        // whether buf is aligned
        bool is_aligned_ptr(const void* buf) const
        {
            return is_aligned((uint64_t)buf);
        }

        uint64_t aligned_begin_offset() const
        {
            return multiply(abegin);
        }

        uint64_t aligned_end_offset() const
        {
            return multiply(aend);
        }

        uint64_t aligned_length() const
        {
            return multiply(aend - abegin);
        }

        #define _this static_cast<const Derived*>(this)
        void divide(uint64_t x, uint64_t& round_down, uint64_t& remainder,
                   uint64_t& round_up) const
        {
            _this->divide(x, round_down, remainder, round_up);
        }
        uint64_t multiply(uint64_t i, uint64_t x = 0) const
        {
            return _this->multiply(i, x);
        }
        uint64_t get_length(uint64_t i) const
        {
            return _this->get_length(i);
        }
        #undef _this

        __attribute__((always_inline))
        void init(uint64_t offset, uint64_t length)
        {
            begin = offset;
            end = offset + length;
            divide(begin, abegin, begin_remainder, apbegin);
            divide(end, apend, end_remainder, aend);

            if (abegin + 1 == aend)
            {
                first.assign(abegin, begin_remainder, length);
                if (abegin != apbegin)
                {
                    if (aend != apend) //end is an offset but aend is an index, totally different
                    {
                        small_note = first;
                        preface.clear();
                        postface.clear();
                    }
                    else //if (aend == epnd)
                    {
                        small_note.clear();
                        preface = first;
                        postface.clear();
                    }
                }
                else //if (abegin == apbegin)
                {
                    if (aend != apend)
                    {
                        small_note.clear();
                        preface.clear();
                        postface = first;
                    }
                    else //if (aend == apend)
                    {
                        small_note.clear();
                        preface.clear();
                        postface.clear();
                    }
                }
            }
            else
            {
                small_note.clear();
                if (abegin == apbegin)
                    preface.clear();
                else
                    preface.assign(abegin, begin_remainder,
                                    get_length(abegin) - begin_remainder);
                if (preface)
                    first = preface;
                else
                    first.assign(apbegin, 0, get_length(apbegin));
                if (aend == apend)
                    postface.clear();
                else
                    postface.assign(apend, 0,
                                     end_remainder);
            }
        }

        struct aligned_parts_t
        {
            const basic_range_split* split;
            struct iterator : sub_range
            {
                const basic_range_split* split;
                iterator(const basic_range_split* split, uint64_t i) :
                    sub_range(i, 0, split->get_length(i)), split(split) { } //prevent initialize order warning
                bool operator == (const iterator& rhs) const
                {
                    return i == rhs.i;
                }
                bool operator != (const iterator& rhs) const
                {
                    return !(*this == rhs);
                }
                iterator& operator++()
                {
                    length = split->get_length(++i);
                    return *this;
                }
                sub_range& operator*()
                {
                    return *this;
                }
                sub_range* operator->() {
                    return this;
                }
            };
            iterator begin() const
            {
                return iterator(split, split->apbegin);
            }
            iterator end() const
            {
                if (split->small_note) // small note have no aligned parts, but apbegin > apend (means empty)
                    return iterator(split, split->apbegin); // therefore, end() should return apbegin for range-based loop
                return iterator(split, split->apend);
            }
        };

        struct aligned_parts_t aligned_parts() const
        {
            return aligned_parts_t{ this };
        }

        struct all_parts_t
        {
            const basic_range_split* split;
            struct iterator : sub_range
            {
                const basic_range_split* split;
                iterator(const basic_range_split* split) : sub_range(split->first), split(split) { }
                bool operator == (const iterator& rhs) const
                {
                    return i == rhs.i;
                }
                bool operator != (const iterator& rhs) const //只实现op==并不能使用range loop；
                {
                    return !(*this == rhs);
                }
                iterator& operator++()
                {
                    offset = 0;
                    ++i;
                    if (i != split->aend) {
                        bool pf = (split->postface && split->postface.i == i);
                        length = pf ? split->postface.length : split->get_length(i);
                    }
                    return *this;
                }
                sub_range& operator*()
                {
                    return *this;
                }
                sub_range* operator->() {
                    return this;
                }
            };
            iterator begin() const
            {
                iterator it(split);
                return it;
            }
            iterator end() const
            {
                iterator it(split);
                it.i = split->aend;
                return it;
            }
        };

        struct all_parts_t all_parts() const
        {
            return all_parts_t{ this };
        }
    };

    struct range_split : public basic_range_split<range_split>
    {
        uint64_t interval;
        range_split(uint64_t offset, uint64_t length, uint64_t interval) : interval(interval)
        {
            init(offset, length);
        }
        void divide(uint64_t x, uint64_t& round_down, uint64_t& remainder,
                    uint64_t& round_up) const
        {
            round_down = x / interval;
            remainder = x % interval;
            round_up = (x + interval - 1) / interval;
        }
        uint64_t multiply(uint64_t i, uint64_t x = 0) const
        {
            return i * interval + x;
        }
        uint64_t get_length(uint64_t) const
        {
            return interval;
        }
    };

    struct range_split_power2 : public basic_range_split<range_split_power2>
    {
        uint64_t interval, interval_shift;
        range_split_power2(uint64_t offset, uint64_t length, uint64_t interval) :
            interval(interval)
        {
            auto x = interval & (interval - 1);  // interval & (interval -1) ==> inverse the lowest 1-bit
            assert(interval > 0 && x == 0);      // assert(interval is 2^n);
            _unused(x);
            interval_shift = __builtin_ffsl(interval)-1;
            init(offset, length);
        }
        void divide(uint64_t x, uint64_t& round_down, uint64_t& remainder,
                    uint64_t& round_up) const
        {
            auto mask = interval - 1;
            round_down = x >> interval_shift;
            remainder = x & mask; // x&mask <--> x % interval
            round_up = (x + mask) >> interval_shift;
        }
        uint64_t multiply(uint64_t i, uint64_t x = 0) const
        {
            return (i << interval_shift) + x;
        }
        uint64_t get_length(uint64_t) const
        {
            return interval;
        }
    };

    inline void ___example_of_range_split___()
    {
        struct process
        {
            process(const sub_range&)
            {
                // do whatever to process the range
            }
        };

        auto rs = range_split(100, 36, 32);

        // process all the sub-ranges in a single loop
        for (auto& x: rs.all_parts())
            process _(x);

        // process the sub-ranges by their category
        if (rs.small_note)
        {
            process _(rs.small_note);
        }
        else
        {
            if (rs.preface)
                process _(rs.preface);
            for (auto& x: rs.aligned_parts())
                process _(x);
            if (rs.postface)
                process _(rs.postface);
        }
    }
}
}
