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

template<typename T>
class Promise {
    semaphore _sem;
    T _value;

    int wait(Timeout timeout) {
        if (_sem.count() > 0) return 0;  // already got
        return _sem.wait(1, timeout);
    }

public:
    Promise() = default;    // not copy-able or movable, use pointer
    void operator=(const Promise&) = delete;

    template<typename P>
    void set_value(P&& rhs) {
        _value = std::forward<P>(rhs);
        _sem.signal(2);
    }

    class Future {
        Promise* _promise;
    public:
        Future(Promise* promise) : _promise(promise) { }
        Future(const Future&) = default;
        Future(Future&&) = default;
        T& get() const {
            return _promise->_value;
        }
        T& get_value() const {
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
