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
#include <utility>
#include <photon/thread/thread.h>
#include <photon/thread/awaiter.h>

namespace photon {

template<typename T, typename Context = AutoContext>
class Future {
    static_assert(sizeof(T) > 0, "in the case that T is void, simply use Awaiter<Context> or semaphore instead!");

    T _value;
    Awaiter<Context> _awaiter;
    bool _got = false;

    template<typename P>
    void set_value(P&& rhs) {
        assert(!_got);      // supposed to be assigned only once
        _value = std::forward<P>(rhs);
        _awaiter.resume();
    }

public:
    Future() = default;  // not copy-able or movable, use pointer or shared_ptr instead
    void operator = (const Future&) = delete;

    ~Future() { assert(_got); } // supposed to set_value() before destruction

    class Promise {
    public:
        Future* _fut;

        template<typename P>
        void set_value(P&& value) {
            _fut->set_value(std::forward<P>(value));
        }
    };

    friend Promise;

    Promise get_promise() {
        return {this};
    }
    T& get_value() {
        wait();
        assert(_got);
        return _value;
    }
    T& get() {
        return get_value();
    }
    int wait(Timeout timeout = {}) {
        if (_got) return 0; // already got
        if (_awaiter.suspend() == 0) {
            _got = true;
            return 0;
        }
        return -1;
    }
    int wait_for(Timeout timeout = {}) {
        return wait(timeout);
    }
};

template<typename T>
using Promise = typename Future<T>::Promise;

}
