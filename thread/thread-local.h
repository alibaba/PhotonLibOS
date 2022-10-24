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

#include <memory>
#include <functional>
#include <photon/thread/thread-key.h>
#include <photon/common/callback.h>
#include <photon/common/tuple-assistance.h>
#include <photon/common/utility.h>

namespace photon {

template<typename T, typename...ARGS>
class thread_local_ptr {
public:
    explicit thread_local_ptr(ARGS&& ...args) : m_args(std::forward<ARGS>(args)...) {
        int ret = photon::thread_key_create(&m_key, &dtor);
        if (ret != 0)
            abort();
    }

    // If every thread_local_ptr has been declared as static, its lifecycle will remain
    // to the end of the program. So we don't even need to delete the key.
    ~thread_local_ptr() = default;

    T& operator*() {
        return *get();
    }

    T* operator->() const {
        return get();
    }

    thread_local_ptr(const thread_local_ptr& l) = delete;
    thread_local_ptr& operator=(const thread_local_ptr& l) = delete;
    thread_local_ptr(thread_local_ptr&& rhs) noexcept = delete;
    thread_local_ptr& operator=(thread_local_ptr&& rhs) noexcept = delete;

private:
    T* get() const __attribute__ ((const)) {
        void* data = photon::thread_getspecific(m_key);
        if (!data) {
            struct ctor {
                T* operator()(const ARGS& ...args) {
                    return new T(args...);
                }
            };
            data = tuple_assistance::apply(ctor(), m_args);
            assert(data != nullptr);    // operator new should not fail
            if (photon::thread_setspecific(m_key, data) != 0)
                abort();
        }
        return (T*) data;
    }

    static void dtor(void* data) {
        delete (T*) data;
    }

    std::tuple<ARGS...> m_args;
    photon::thread_key_t m_key = std::numeric_limits<photon::thread_key_t>::max();
};

}