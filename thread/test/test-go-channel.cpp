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

#include <photon/thread/go.h>
#include <photon/thread/thread.h>
#include <photon/photon.h>
#include <photon/common/alog.h>

#include "../../test/gtest.h"

#include <atomic>
#include <vector>
#include <string>
#include <chrono>

using namespace photon;

class GoChannelTest : public ::testing::Test {
protected:
    void SetUp() override {
        photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    }

    void TearDown() override {
        photon::fini();
    }
};

// =============================================================================
// go() tests
// =============================================================================

TEST_F(GoChannelTest, GoBasicLambda) {
    std::atomic<int> counter{0};

    go([&counter] {
        counter++;
    });

    thread_yield();
    photon::thread_usleep(1000);

    EXPECT_EQ(counter.load(), 1);
}

TEST_F(GoChannelTest, GoWithArguments) {
    std::atomic<int> result{0};

    go([&result](int a, int b) {
        result = a + b;
    }, 10, 20);

    thread_yield();
    photon::thread_usleep(1000);

    EXPECT_EQ(result.load(), 30);
}

TEST_F(GoChannelTest, GoMultipleThreads) {
    const int N = 100;
    std::atomic<int> counter{0};

    for (int i = 0; i < N; i++) {
        go([&counter] {
            counter++;
        });
    }

    // Wait for all threads to complete
    for (int i = 0; i < 100 && counter < N; i++) {
        photon::thread_usleep(1000);
    }

    EXPECT_EQ(counter.load(), N);
}

TEST_F(GoChannelTest, GoWithCustomStackSize) {
    std::atomic<bool> executed{false};

    go(256 * 1024, [&executed] {
        executed = true;
    });

    thread_yield();
    photon::thread_usleep(1000);

    EXPECT_TRUE(executed.load());
}

static int static_function(int x, int y) {
    return x * y;
}

TEST_F(GoChannelTest, GoWithStaticFunction) {
    std::atomic<int> result{0};

    go([&result] {
        result = static_function(6, 7);
    });

    thread_yield();
    photon::thread_usleep(1000);

    EXPECT_EQ(result.load(), 42);
}

// =============================================================================
// Buffered channel tests
// =============================================================================

TEST_F(GoChannelTest, BufferedChannelBasic) {
    channel<int> ch(5);

    EXPECT_TRUE(ch.send(1));
    EXPECT_TRUE(ch.send(2));
    EXPECT_TRUE(ch.send(3));

    EXPECT_EQ(ch.size(), 3u);

    int val;
    EXPECT_TRUE(ch.recv(val));
    EXPECT_EQ(val, 1);
    EXPECT_TRUE(ch.recv(val));
    EXPECT_EQ(val, 2);
    EXPECT_TRUE(ch.recv(val));
    EXPECT_EQ(val, 3);

    EXPECT_EQ(ch.size(), 0u);
}

TEST_F(GoChannelTest, BufferedChannelOperatorSyntax) {
    channel<int> ch(5);

    ch << 10 << 20 << 30;

    int a, b, c;
    ch >> a >> b >> c;

    EXPECT_EQ(a, 10);
    EXPECT_EQ(b, 20);
    EXPECT_EQ(c, 30);
}

TEST_F(GoChannelTest, BufferedChannelFull) {
    channel<int> ch(2);

    EXPECT_TRUE(ch.send(1));
    EXPECT_TRUE(ch.send(2));

    // Channel is full, try_send should fail
    EXPECT_FALSE(ch.try_send(3));

    // Receive one to make room
    int val;
    EXPECT_TRUE(ch.recv(val));
    EXPECT_EQ(val, 1);

    // Now we can send again
    EXPECT_TRUE(ch.try_send(3));
}

TEST_F(GoChannelTest, BufferedChannelClose) {
    channel<int> ch(5);

    ch.send(1);
    ch.send(2);
    ch.close();

    // Should still be able to receive buffered values
    int val;
    EXPECT_TRUE(ch.recv(val));
    EXPECT_EQ(val, 1);
    EXPECT_TRUE(ch.recv(val));
    EXPECT_EQ(val, 2);

    // No more values, channel is closed
    EXPECT_FALSE(ch.recv(val));

    // Sending to closed channel should fail
    EXPECT_FALSE(ch.send(3));
}

TEST_F(GoChannelTest, BufferedChannelIterator) {
    channel<int> ch(5);

    ch << 1 << 2 << 3 << 4 << 5;
    ch.close();

    std::vector<int> received;
    for (auto val : ch) {
        received.push_back(val);
    }

    EXPECT_EQ(received.size(), 5u);
    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(received[i], i + 1);
    }
}

TEST_F(GoChannelTest, BufferedChannelPairRecv) {
    channel<int> ch(2);
    ch << 42;
    ch.close();

    auto result1 = ch.recv();
    EXPECT_TRUE(result1.second);
    EXPECT_EQ(result1.first, 42);

    auto result2 = ch.recv();
    EXPECT_FALSE(result2.second);
}

// =============================================================================
// Unbuffered channel tests
// =============================================================================

TEST_F(GoChannelTest, UnbufferedChannelSendRecv) {
    channel<int> ch;
    std::atomic<int> received{0};

    // Start a receiver
    go([&ch, &received] {
        int val;
        if (ch.recv(val)) {
            received = val;
        }
    });

    thread_yield();

    // Send a value
    EXPECT_TRUE(ch.send(42));

    photon::thread_usleep(10000);

    EXPECT_EQ(received.load(), 42);
}

TEST_F(GoChannelTest, UnbufferedChannelMultipleSendRecv) {
    channel<int> ch;
    std::atomic<int> sum{0};
    const int N = 10;

    // Start a receiver
    go([&ch, &sum] {
        for (int i = 0; i < N; i++) {
            int val;
            if (ch.recv(val)) {
                sum += val;
            }
        }
    });

    thread_yield();

    // Send values
    for (int i = 1; i <= N; i++) {
        ch.send(i);
    }

    photon::thread_usleep(100000);

    EXPECT_EQ(sum.load(), N * (N + 1) / 2);
}

TEST_F(GoChannelTest, UnbufferedChannelTrySendFails) {
    channel<int> ch;

    // No receiver waiting, try_send should fail
    EXPECT_FALSE(ch.try_send(42));
}

TEST_F(GoChannelTest, UnbufferedChannelClose) {
    channel<int> ch;
    std::atomic<bool> recv_failed{false};

    go([&ch, &recv_failed] {
        int val;
        if (!ch.recv(val)) {
            recv_failed = true;
        }
    });

    thread_yield();
    photon::thread_usleep(1000);

    ch.close();

    photon::thread_usleep(10000);

    EXPECT_TRUE(recv_failed.load());
}

// =============================================================================
// Channel with complex types
// =============================================================================

TEST_F(GoChannelTest, ChannelWithString) {
    channel<std::string> ch(3);

    ch << "hello" << "world" << "!";

    std::string s1, s2, s3;
    ch >> s1 >> s2 >> s3;

    EXPECT_EQ(s1, "hello");
    EXPECT_EQ(s2, "world");
    EXPECT_EQ(s3, "!");
}

struct TestStruct {
    int x;
    std::string name;
    bool operator==(const TestStruct& other) const {
        return x == other.x && name == other.name;
    }
};

TEST_F(GoChannelTest, ChannelWithStruct) {
    channel<TestStruct> ch(2);

    ch.send({42, "test"});
    ch.send({100, "another"});

    TestStruct s1, s2;
    ch.recv(s1);
    ch.recv(s2);

    EXPECT_EQ(s1.x, 42);
    EXPECT_EQ(s1.name, "test");
    EXPECT_EQ(s2.x, 100);
    EXPECT_EQ(s2.name, "another");
}

// =============================================================================
// Concurrent access tests
// =============================================================================

TEST_F(GoChannelTest, ConcurrentProducerConsumer) {
    channel<int> ch(10);
    const int N = 1000;
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<long long> sum_produced{0};
    std::atomic<long long> sum_consumed{0};

    // Producer
    go([&ch, &produced, &sum_produced] {
        for (int i = 0; i < N; i++) {
            ch.send(i);
            produced++;
            sum_produced += i;
        }
    });

    // Consumer
    go([&ch, &consumed, &sum_consumed] {
        for (int i = 0; i < N; i++) {
            int val;
            if (ch.recv(val)) {
                consumed++;
                sum_consumed += val;
            }
        }
    });

    // Wait for completion
    for (int i = 0; i < 1000 && (produced < N || consumed < N); i++) {
        photon::thread_usleep(10000);
    }

    EXPECT_EQ(produced.load(), N);
    EXPECT_EQ(consumed.load(), N);
    EXPECT_EQ(sum_produced.load(), sum_consumed.load());
}

TEST_F(GoChannelTest, MultipleProducersConsumers) {
    channel<int> ch(20);
    const int NUM_PRODUCERS = 4;
    const int NUM_CONSUMERS = 4;
    const int ITEMS_PER_PRODUCER = 100;

    std::atomic<int> total_produced{0};
    std::atomic<int> total_consumed{0};

    // Start producers
    for (int p = 0; p < NUM_PRODUCERS; p++) {
        go([&ch, &total_produced] {
            for (int i = 0; i < ITEMS_PER_PRODUCER; i++) {
                ch.send(1);
                total_produced++;
            }
        });
    }

    // Start consumers
    for (int c = 0; c < NUM_CONSUMERS; c++) {
        go([&ch, &total_consumed] {
            while (total_consumed < NUM_PRODUCERS * ITEMS_PER_PRODUCER) {
                int val;
                if (ch.try_recv(val)) {
                    total_consumed++;
                } else {
                    photon::thread_yield();
                }
            }
        });
    }

    // Wait for completion
    for (int i = 0; i < 1000 &&
         total_consumed < NUM_PRODUCERS * ITEMS_PER_PRODUCER; i++) {
        photon::thread_usleep(10000);
    }

    EXPECT_EQ(total_produced.load(), NUM_PRODUCERS * ITEMS_PER_PRODUCER);
    EXPECT_EQ(total_consumed.load(), NUM_PRODUCERS * ITEMS_PER_PRODUCER);
}

// =============================================================================
// Timeout tests
// =============================================================================

TEST_F(GoChannelTest, RecvTimeout) {
    channel<int> ch(1);

    int val;
    // Should timeout waiting for data
    auto start = photon::now;
    bool result = ch.recv(val, Timeout(50000)); // 50ms timeout
    auto elapsed = photon::now - start;

    EXPECT_FALSE(result);
    EXPECT_GE(elapsed, 40000ULL); // At least 40ms
}

TEST_F(GoChannelTest, SendTimeoutOnFull) {
    channel<int> ch(1);
    ch.send(1); // Fill the channel

    // Should timeout trying to send to full channel
    auto start = photon::now;
    bool result = ch.send(2, Timeout(50000)); // 50ms timeout
    auto elapsed = photon::now - start;

    EXPECT_FALSE(result);
    EXPECT_GE(elapsed, 40000ULL);
}

// =============================================================================
// make_channel helper test
// =============================================================================

TEST_F(GoChannelTest, MakeChannel) {
    auto ch = make_channel<int>(5);

    ch << 1 << 2 << 3;

    EXPECT_EQ(ch.size(), 3u);
    EXPECT_EQ(ch.capacity(), 5u);
}

// =============================================================================
// Edge cases
// =============================================================================

TEST_F(GoChannelTest, EmptyChannelProperties) {
    channel<int> ch(5);

    EXPECT_TRUE(ch.empty());
    EXPECT_EQ(ch.size(), 0u);
    EXPECT_FALSE(ch.is_closed());

    ch.send(1);
    EXPECT_FALSE(ch.empty());
    EXPECT_EQ(ch.size(), 1u);

    ch.close();
    EXPECT_TRUE(ch.is_closed());
}

TEST_F(GoChannelTest, DoubleClose) {
    channel<int> ch(5);

    ch.close();
    ch.close(); // Should not crash

    EXPECT_TRUE(ch.is_closed());
}

TEST_F(GoChannelTest, RecvFromClosedEmptyChannel) {
    channel<int> ch(5);
    ch.close();

    int val = -1;
    EXPECT_FALSE(ch.recv(val));
    EXPECT_EQ(val, -1); // Should not modify val
}

// =============================================================================
// Integration test: Go-style ping-pong
// =============================================================================

TEST_F(GoChannelTest, PingPong) {
    channel<int> ping(1);
    channel<int> pong(1);

    std::atomic<int> ping_count{0};
    std::atomic<int> pong_count{0};
    const int ROUNDS = 10;

    // Ping player
    go([&ping, &pong, &ping_count] {
        for (int i = 0; i < ROUNDS; i++) {
            int val;
            if (ping.recv(val)) {
                ping_count++;
                pong.send(val + 1);
            }
        }
    });

    // Pong player
    go([&ping, &pong, &pong_count] {
        for (int i = 0; i < ROUNDS; i++) {
            int val;
            if (pong.recv(val)) {
                pong_count++;
                if (i < ROUNDS - 1) {
                    ping.send(val + 1);
                }
            }
        }
    });

    // Start the game
    ping.send(0);

    // Wait for completion
    for (int i = 0; i < 1000 &&
         (ping_count < ROUNDS || pong_count < ROUNDS); i++) {
        photon::thread_usleep(10000);
    }

    EXPECT_EQ(ping_count.load(), ROUNDS);
    EXPECT_EQ(pong_count.load(), ROUNDS);
}

// =============================================================================
// Move semantics test
// =============================================================================

TEST_F(GoChannelTest, MoveOnlyType) {
    struct MoveOnly {
        int value;
        MoveOnly() : value(0) {}
        explicit MoveOnly(int v) : value(v) {}
        MoveOnly(const MoveOnly&) = delete;
        MoveOnly& operator=(const MoveOnly&) = delete;
        MoveOnly(MoveOnly&& other) noexcept : value(other.value) {
            other.value = -1;
        }
        MoveOnly& operator=(MoveOnly&& other) noexcept {
            value = other.value;
            other.value = -1;
            return *this;
        }
    };

    channel<MoveOnly> ch(2);

    ch.send(MoveOnly(42));
    ch.send(MoveOnly(100));

    MoveOnly m1, m2;
    ch.recv(m1);
    ch.recv(m2);

    EXPECT_EQ(m1.value, 42);
    EXPECT_EQ(m2.value, 100);
}

// =============================================================================
// Select tests
// =============================================================================

TEST_F(GoChannelTest, SelectNonblockRecvReady) {
    channel<int> ch1(1);
    channel<int> ch2(1);

    ch1.send(42);  // ch1 has data ready

    int received = 0;
    bool ch1_selected = false;
    bool ch2_selected = false;

    int result = select_nonblock(
        recv_case_of(ch1, [&](int val, bool ok) {
            if (ok) { received = val; ch1_selected = true; }
        }),
        recv_case_of(ch2, [&](int val, bool ok) {
            if (ok) { received = val; ch2_selected = true; }
        })
    );

    EXPECT_EQ(result, 0);  // ch1 was selected (index 0)
    EXPECT_TRUE(ch1_selected);
    EXPECT_FALSE(ch2_selected);
    EXPECT_EQ(received, 42);
}

TEST_F(GoChannelTest, SelectNonblockSendReady) {
    channel<int> ch1(1);  // Has space
    channel<int> ch2(1);
    ch2.send(99);  // ch2 is full

    bool ch1_selected = false;
    bool ch2_selected = false;

    int result = select_nonblock(
        send_case_of(ch1, 42, [&]() { ch1_selected = true; }),
        send_case_of(ch2, 100, [&]() { ch2_selected = true; })
    );

    EXPECT_EQ(result, 0);  // ch1 was selected (has space)
    EXPECT_TRUE(ch1_selected);
    EXPECT_FALSE(ch2_selected);

    // Verify the value was sent
    int val;
    EXPECT_TRUE(ch1.recv(val));
    EXPECT_EQ(val, 42);
}

TEST_F(GoChannelTest, SelectNonblockNoneReady) {
    channel<int> ch1(1);
    channel<int> ch2(1);

    // Both channels empty, no recv possible
    int result = select_nonblock(
        recv_case_of(ch1, [](int, bool) {}),
        recv_case_of(ch2, [](int, bool) {})
    );

    EXPECT_EQ(result, -1);  // None ready, like default case
}

TEST_F(GoChannelTest, SelectBlockingRecv) {
    channel<int> ch1(1);
    channel<int> ch2(1);

    std::atomic<int> received{0};
    std::atomic<bool> done{false};

    // Start a thread that will select on both channels
    go([&ch1, &ch2, &received, &done] {
        select(
            recv_case_of(ch1, [&](int val, bool ok) {
                if (ok) received = val;
            }),
            recv_case_of(ch2, [&](int val, bool ok) {
                if (ok) received = val;
            })
        );
        done = true;
    });

    photon::thread_usleep(10000);  // Give select time to start

    // Send to ch2
    ch2.send(123);

    // Wait for completion
    for (int i = 0; i < 100 && !done; i++) {
        photon::thread_usleep(10000);
    }

    EXPECT_TRUE(done.load());
    EXPECT_EQ(received.load(), 123);
}

TEST_F(GoChannelTest, SelectWithTimeout) {
    channel<int> ch1(1);
    channel<int> ch2(1);

    // Both channels empty, select should timeout
    auto start = photon::now;
    int result = select(
        Timeout(50000),  // 50ms timeout
        recv_case_of(ch1, [](int, bool) {}),
        recv_case_of(ch2, [](int, bool) {})
    );
    auto elapsed = photon::now - start;

    EXPECT_EQ(result, -1);  // Timeout
    EXPECT_GE(elapsed, 40000ULL);
}

TEST_F(GoChannelTest, SelectMixedSendRecv) {
    channel<int> ch_send(1);  // For sending
    channel<int> ch_recv(1);  // For receiving

    ch_recv.send(42);  // Pre-fill recv channel

    bool sent = false;
    int received = 0;

    // Try to either send to ch_send or recv from ch_recv
    int result = select_nonblock(
        send_case_of(ch_send, 100, [&]() { sent = true; }),
        recv_case_of(ch_recv, [&](int val, bool ok) {
            if (ok) received = val;
        })
    );

    // Both should be ready, but one will be selected
    EXPECT_GE(result, 0);
    EXPECT_TRUE(sent || received == 42);
}

TEST_F(GoChannelTest, SelectFanIn) {
    // Fan-in pattern: receive from multiple channels into one
    channel<int> ch1(5);
    channel<int> ch2(5);
    channel<int> ch3(5);
    channel<int> out(15);

    // Send data to input channels
    for (int i = 0; i < 5; i++) {
        ch1.send(i);
        ch2.send(i + 10);
        ch3.send(i + 20);
    }
    ch1.close();
    ch2.close();
    ch3.close();

    std::atomic<int> count{0};

    // Fan-in loop
    go([&] {
        while (count < 15) {
            int result = select_nonblock(
                recv_case_of(ch1, [&](int val, bool ok) {
                    if (ok) { out.send(val); count++; }
                }),
                recv_case_of(ch2, [&](int val, bool ok) {
                    if (ok) { out.send(val); count++; }
                }),
                recv_case_of(ch3, [&](int val, bool ok) {
                    if (ok) { out.send(val); count++; }
                })
            );
            if (result == -1) {
                // All channels exhausted
                break;
            }
        }
    });

    // Wait for fan-in to complete
    for (int i = 0; i < 100 && count < 15; i++) {
        photon::thread_usleep(10000);
    }

    EXPECT_EQ(count.load(), 15);
    EXPECT_EQ(out.size(), 15u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
