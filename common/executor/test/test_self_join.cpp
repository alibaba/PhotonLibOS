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

// Verify ~ExecutorImpl won't self-join (EDEADLK → terminate) when
// destroyed from its own worker thread. Related: #967, #969.

#include <photon/common/executor/executor.h>
#include <photon/common/alog.h>
#include <photon/photon.h>
#include "../../../test/gtest.h"

#include <memory>
#include <atomic>
#include <future>

using namespace photon;

// Task releases the last shared_ptr ref on the worker thread,
// triggering ~ExecutorImpl from within → must detach, not join.
TEST(executor, destroy_from_worker_thread) {
    std::promise<void> done;
    auto fut = done.get_future();

    {
        auto exec = std::make_shared<Executor>(
            photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);

        exec->async_perform(new auto([&exec, &done] {
            exec.reset();       // ~Executor on worker thread
            done.set_value();   // reachable only if no crash
        }));
    }

    fut.wait();
    LOG_INFO("executor destroyed from worker thread without crash");
}
