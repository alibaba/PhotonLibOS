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

namespace photon {
#include <photon/thread/thread.h>
#include <photon/thread/awaiter.h>

template<typename T, typename Context = AutoContext>
class Promise {
    static_assert(sizeof(T) > 0, "in the case that T is void, simply use Awaiter<Context> or semaphore instead!");

    T _value;
    Awaiter<Context> _awaiter;
    bool _got = false;

    int wait(Timeout timeout) {
        if (_got) return 0; // already got
        if (_awaiter.suspend() == 0) {
            _got = true;
            return 0;
        }
        return -1;
    }

public:
    Promise() = default;    // not copy-able or movable, use pointer
    void operator = (const Promise&) = delete;

    template<typename P>
    void set_value(P&& rhs) {
        _value = std::forward<P>(rhs);
        _awaiter.resume();
    }

    class Future {
        Promise* _promise;
    public:
        Future(Promise* promise) : _promise(promise) { }
        Future(const Future&) = default;
        Future(Future&&) = default;
        T& get() {
            wait();
            assert(_promise->_got);
            return _promise->_value;
        }
        T& get_value() {
            wait();
            assert(_promise->_got);
            return _promise->_value;
        }
        int wait(Timeout timeout = {}) {
            return _promise->wait(timeout);
        }
        int wait_for(Timeout timeout = {}) {
            return _promise->wait(timeout);
        }
    };

    friend Future;

    Future get_future() {
        return Future(this);
    }
};

}
