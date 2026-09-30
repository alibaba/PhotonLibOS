/*
Copyright 2026 The Photon Authors

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

#include <atomic>
#include <thread>
#include <photon/photon.h>
#include <photon/thread/vcpu-local.h>
#include "../../test/gtest.h"

namespace {

struct FailureGate {
    photon::semaphore owner_ready{0}, start_fini{0}, fini_waiting{0};
    std::atomic<int> helper_calls{0}, destroyed{0}, wrong_owner{0};
    bool race_fini = false;
};

FailureGate* gate = nullptr;

struct Value {
    photon::vcpu_base* owner = photon::get_vcpu();
    ~Value() {
        if (owner != photon::get_vcpu()) ++gate->wrong_owner;
        ++gate->destroyed;
    }
};

} // namespace

namespace photon {

static thread* fail_destroy_helper(thread_entry, void*) {
    ++gate->helper_calls;
    if (gate->race_fini) {
        gate->start_fini.signal(1);
        // The slot is already claimed and pins its owning vCPU. Let that
        // vCPU's fini hook reach the empty-list handoff wait before failing.
        EXPECT_EQ(0, gate->fini_waiting.wait(1, 5ULL * 1000 * 1000));
    }
    errno = ENOMEM;
    return nullptr;
}

static int observe_fini_wait(uint64_t duration) {
    gate->fini_waiting.signal(1);
    return photon::thread_usleep(duration);
}

} // namespace photon

// Include the private implementation to control allocation failure and the
// handoff interleaving without adding hooks to the public VCPULocal API.
#define thread_create fail_destroy_helper
#define thread_usleep observe_fini_wait
#include "../vcpu-local.cpp"
#undef thread_usleep
#undef thread_create

static void check_helper_failure(bool race_fini) {
    ASSERT_EQ(0, photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE));
    DEFER(photon::fini());
    FailureGate state;
    state.race_fini = race_fini;
    gate = &state;
    struct Local final : photon::VCPULocal<Value> {};
    auto local = new Local;
    std::thread owner([&] {
        EXPECT_EQ(0, photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE));
        EXPECT_NE(nullptr, local->get());
        state.owner_ready.signal(1);
        EXPECT_EQ(0, state.start_fini.wait(1, 5ULL * 1000 * 1000));
        EXPECT_EQ(0, photon::fini());
    });
    EXPECT_EQ(0, state.owner_ready.wait(1, 5ULL * 1000 * 1000));
    delete local;
    EXPECT_EQ(1, state.helper_calls.load());
    if (!race_fini) {
        EXPECT_EQ(0, state.destroyed.load());
        state.start_fini.signal(1);
    }
    owner.join();
    EXPECT_EQ(1, state.destroyed.load());
    EXPECT_EQ(0, state.wrong_owner.load());
    gate = nullptr;
}

TEST(vcpu_local_failure, helper_failure_before_fini) {
    check_helper_failure(false);
}

TEST(vcpu_local_failure, helper_failure_after_fini_waits_for_handoff) {
    check_helper_failure(true);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
