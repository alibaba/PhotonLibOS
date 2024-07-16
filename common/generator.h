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
#include <cassert>
#include <type_traits>

template<typename Derived>
class __CRTP    // the Curiously Recurring Template Pattern
{
protected:
    // static_assert(std::is_base_of<__CRTP, Derived>::value, "...");
    Derived* THIS()
    {
        return (Derived*)this;
    }
    const Derived* THIS() const
    {
        return (const Derived*)this;
    }
};

template<typename Derived, typename ValueType, typename IteratorPayload_ = void>
class Generator : public __CRTP<Derived>
{
public:
    using __CRTP<Derived>::THIS;
    struct IteratorPayloadVoid
    {
        int _;
        IteratorPayloadVoid(const Generator*) { }  // for begin()
        IteratorPayloadVoid() { }                   // for end()
    };
    using IteratorPayload = typename std::conditional<
        std::is_same<IteratorPayload_, void>::value,
        IteratorPayloadVoid, IteratorPayload_>::type;

    struct Iterator : public __CRTP<Iterator>
    {
        using __CRTP<Iterator>::THIS;

        // constructor for begin()
        Iterator(const Derived* generator) :
            _generator(generator), payload(generator)
        {
            if (!_generator->has_next(THIS()))
                _generator = nullptr;
        }

        // constructor for end()
        Iterator() : _generator(nullptr), payload() { }

        void operator++()
        {
            if (_generator->has_next(THIS())) {
                _generator->next(THIS());
                i++;
            } else {
                _generator = nullptr;
            }
        }
        void operator++(int)
        {
            ++(*this);
        }
        ValueType operator*() const
        {
            return _generator->get(THIS());
        }
        bool operator==(const Iterator& rhs)
        {
            if (_generator) {
                return rhs._generator && i == rhs.i;
            } else {
                return !rhs._generator;
            }
        }
        bool operator!=(const Iterator& rhs)
        {
            return !(*this == rhs);
        }

        const Derived* _generator;
        int i = 0;  // assistant for equality test
        IteratorPayload payload;
    };

    // either form of these functions must be overrided in derived classes
    bool has_next(const Iterator* it) const
    {
        return THIS()->payload_has_next(it->payload);
    }
    bool payload_has_next(const IteratorPayload&) const
    {
        assert(false);
        return false;
    }
    void next(Iterator* it) const
    {
        THIS()->payload_next(it->payload);
    }
    void payload_next(IteratorPayload&) const
    {
        assert(false);
    }
    ValueType get(const Iterator* it) const
    {
        return THIS()->payload_get(it->payload);
    }
    ValueType payload_get(const IteratorPayload&) const
    {
        assert(false);
        return ValueType();
    }

    // optionally overrided in derived classes (if Iterator sub-classed)
    Iterator begin() const
    {
        return Iterator(THIS());
    }
    Iterator end() const
    {
        return Iterator();
    }
};

//====================================================================
inline void ___example_of_generator____()
{
    class example_generator1 : public Generator<example_generator1, int>
    {
    public:
        int n;
        example_generator1(int N) : n(N) { }
        bool has_next(const Iterator* it) const
        {
            return it->i < n;
        }
        void next(Iterator* it) const
        {
            assert(has_next(it));
            // it->i++; // i will be increased automatically
        }
        int get(const Iterator* it) const
        {
            return it->i;
        }
    };

    {
        int i = 0; (void)i;
        for (auto x: example_generator1(10))
        {
            assert(x == i); i++;
            (void)x;
        }
    }

    class example_generator2;
    struct IteratorPayload
    {
        int I = 0;
        IteratorPayload(const example_generator2*) { } // for begin()
        IteratorPayload() { }  // for end()
    };

    class example_generator2 : public Generator<example_generator2, int, IteratorPayload>
    {
    public:
        int n;
        example_generator2(int N) : n(N) { }
        bool payload_has_next(const IteratorPayload& payload) const
        {
            return payload.I < n;
        }
        void payload_next(IteratorPayload& payload) const
        {
            assert(payload_has_next(payload));
            payload.I++;
        }
        int payload_get(const IteratorPayload& payload) const
        {
            return payload.I;
        }
    };

    {
        int i = 0; (void)i;
        for (auto x: example_generator2(10))
        {
            assert(x == i); i++;
            (void)x;
        }
    }
}
