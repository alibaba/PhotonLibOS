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

#include <atomic>
#include <memory>
#include <thread>

// Like client_function_test, inspect the private implementation without adding
// a public testing interface to the library.
#include "../dialer.cpp"
#include "../../../test/gtest.h"

using namespace photon;
using namespace photon::net;
using namespace photon::net::http;

namespace {

struct ResolverState {
    std::atomic<int> created{0};
    std::atomic<int> destroyed{0};
    std::atomic<int> wrong_vcpu{0};

    class TrackedResolver : public Resolver {
        ResolverState* state;
        vcpu_base* owner = photon::get_vcpu();
    public:
        explicit TrackedResolver(ResolverState* state) : state(state) {
            ++state->created;
        }
        ~TrackedResolver() override {
            if (owner != photon::get_vcpu()) ++state->wrong_vcpu;
            ++state->destroyed;
        }
        IPAddr resolve(std::string_view) override { return IPAddr("127.0.0.1"); }
        IPAddr resolve_filter(std::string_view host, Delegate<bool, IPAddr>) override {
            return resolve(host);
        }
        void discard_cache(std::string_view, IPAddr) override {}
    };

    Resolver* make() { return new TrackedResolver(this); }
};

class TestSharedResolver : public SharedResolver {
public:
    explicit TestSharedResolver(ResolverState* state)
        : SharedResolver({state, &ResolverState::make}) {}
    bool published() {
        SCOPED_LOCK(m_lock);
        return m_current != nullptr;
    }
};

} // namespace

TEST(shared_resolver, shutdown_waits_for_only_its_generation_past_three_seconds) {
    ResolverState state;
    TestSharedResolver shared(&state);
    ASSERT_EQ(0, photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE));
    DEFER(if (photon::CURRENT) photon::fini());
    photon::semaphore ready(0), start_fini(0), finished(0);
    std::atomic<bool> owner_finished{false};
    std::thread owner([&] {
        EXPECT_EQ(0, photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE));
        { auto lease = shared.borrow(); }
        ready.signal(1);
        start_fini.wait(1);
        EXPECT_EQ(0, photon::fini());
        owner_finished.store(true);
        finished.signal(1);
    });
    EXPECT_EQ(0, ready.wait(1, 5ULL * 1000 * 1000));
    auto old = std::unique_ptr<SharedResolver::Ref>(
        new SharedResolver::Ref(shared.borrow()));
    start_fini.signal(1);
    Timeout unpublish_timeout(5ULL * 1000 * 1000);
    while (shared.published() && !unpublish_timeout.expired())
        photon::thread_usleep(1000);
    EXPECT_FALSE(shared.published());
    {
        auto next = shared.borrow();
        EXPECT_NE(old->operator->(), next.operator->());
        EXPECT_EQ(2, state.created.load());
        photon::thread_usleep(3'200'000);
        EXPECT_FALSE(owner_finished.load());
        EXPECT_EQ(0, state.destroyed.load());
        old.reset();
        EXPECT_EQ(0, finished.wait(1, 5ULL * 1000 * 1000));
        // The new generation remains borrowed, but cannot hold up the old one.
        EXPECT_EQ(1, state.destroyed.load());
        EXPECT_EQ(0, state.wrong_vcpu.load());
    }
    owner.join();
    EXPECT_EQ(0, photon::fini());
    EXPECT_EQ(2, state.destroyed.load());
    EXPECT_EQ(0, state.wrong_vcpu.load());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
