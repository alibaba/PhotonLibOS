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

// Unit checks for the shared blk test helpers themselves (harness.h/.cpp): the
// self-describing stress block format, its validator, the failure classifier,
// pattern(), and the consumer-side process isolation.
//
// A stress test that cannot fail is worthless, and the suites' concurrent_stress
// cases only ever exercise the HAPPY path -- so this is what locks in the
// detector's sensitivity: every corruption class the driver claims to attribute
// (misrouted, torn, lost, corrupted) must actually be caught, and the SHARED
// mode's canonical-block invariant must hold, since the whole shared-region
// verification model rests on it.
//
// The consumer checks below hold the isolation to the same standard. No device
// node and no root, deliberately: what has to be true for every transport is the
// plumbing (the child's verdict reaching the caller), the child dropping the fd
// table it inherited, and the caller keeping a deadline of its own instead of
// waiting on a consumer that may never come back.
//
// Pure logic: no root, no device node, no photon runtime, every platform.

#include "harness.h"

#include "../../test/gtest.h"

#include <photon/common/utility.h>   // DEFER

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace photon {
namespace blk {
namespace test {

static constexpr size_t BLK = 64 << 10;
static constexpr uint64_t OFF = 1ull << 20;

// rewrite just the header of a formatted block, leaving the payload alone
static void patch_hdr(std::vector<char>& b, const StressBlockHdr& h) {
    memcpy(b.data(), &h, sizeof(h));
}
static StressBlockHdr hdr_of(const std::vector<char>& b) {
    StressBlockHdr h;
    memcpy(&h, b.data(), sizeof(h));
    return h;
}

TEST(StressBlock, clean_block_validates_and_reports_its_owner) {
    std::vector<char> b(BLK);
    stress_format(b.data(), BLK, OFF, 7, 3);
    EXPECT_EQ(0, stress_validate(b.data(), BLK, OFF));
    uint32_t tid = 0, seq = 0;
    ASSERT_EQ(0, stress_validate(b.data(), BLK, OFF, &tid, &seq));
    EXPECT_EQ(7u, tid);
    EXPECT_EQ(3u, seq);
}

TEST(StressBlock, misrouted_torn_and_corrupted_blocks_are_caught) {
    std::vector<char> b(BLK);
    stress_format(b.data(), BLK, OFF, 7, 3);

    // the same bytes claimed at another offset / length: a misrouted IO
    EXPECT_EQ(EILSEQ, stress_validate(b.data(), BLK, OFF + 4096));
    EXPECT_EQ(EILSEQ, stress_validate(b.data(), BLK - 4096, OFF));

    // one flipped payload byte: a torn IO
    b[BLK / 2] ^= 0x55;
    EXPECT_EQ(EILSEQ, stress_validate(b.data(), BLK, OFF));

    // a header whose length disagrees with the request
    stress_format(b.data(), BLK, OFF, 7, 3);
    auto h = hdr_of(b);
    h.len -= 4096;
    patch_hdr(b, h);
    EXPECT_EQ(EILSEQ, stress_validate(b.data(), BLK, OFF));

    // a header whose fill disagrees with its own (tid, seq)
    stress_format(b.data(), BLK, OFF, 7, 3);
    h = hdr_of(b);
    h.fill ^= 0x02;
    patch_hdr(b, h);
    EXPECT_EQ(EILSEQ, stress_validate(b.data(), BLK, OFF));
}

TEST(StressBlock, a_block_too_small_for_a_header_is_rejected) {
    std::vector<char> b(sizeof(StressBlockHdr) + 4, 0);
    EXPECT_EQ(EINVAL, stress_validate(b.data(), b.size(), OFF));
}

TEST(StressBlock, canonical_blocks_are_determined_by_the_slot_alone) {
    // the SHARED-mode invariant: two writers of one slot produce identical
    // bytes (so any interleaving is valid), and different slots differ
    std::vector<char> a(BLK), b(BLK);
    stress_format_canonical(a.data(), BLK, OFF);
    stress_format_canonical(b.data(), BLK, OFF);
    EXPECT_EQ(0, memcmp(a.data(), b.data(), BLK));
    stress_format_canonical(b.data(), BLK, OFF + 4096);
    EXPECT_NE(0, memcmp(a.data(), b.data(), BLK));
    // and the length is part of the slot's identity
    stress_format_canonical(b.data(), BLK - 4096, OFF);
    EXPECT_NE(0, memcmp(a.data(), b.data(), BLK - 4096));
}

TEST(StressBlock, poison_differs_from_every_canonical_block) {
    // a lost FIRST write to a slot is only detectable if the pre-fill cannot be
    // mistaken for content
    std::vector<char> p(BLK), c(BLK);
    stress_format_poison(p.data(), BLK);
    stress_format_canonical(c.data(), BLK, OFF);
    EXPECT_NE(0, memcmp(p.data(), c.data(), BLK));
    EXPECT_NE(0, stress_validate(p.data(), BLK, OFF));
}

TEST(StressBlock, diagnose_names_the_failure_class) {
    std::vector<char> want(BLK), got(BLK);
    stress_format_canonical(want.data(), BLK, OFF);

    stress_format_poison(got.data(), BLK);
    EXPECT_NE(std::string::npos, stress_diagnose(got.data(), BLK, OFF).find("lost"))
        << stress_diagnose(got.data(), BLK, OFF);

    stress_format_canonical(got.data(), BLK, OFF + 4096);   // another slot's block
    EXPECT_NE(std::string::npos, stress_diagnose(got.data(), BLK, OFF).find("misrouted"))
        << stress_diagnose(got.data(), BLK, OFF);

    got = want;
    auto h = hdr_of(got);
    h.fill ^= 0x02;                                         // header vs its own fill
    patch_hdr(got, h);
    EXPECT_NE(std::string::npos, stress_diagnose(got.data(), BLK, OFF).find("corrupted"))
        << stress_diagnose(got.data(), BLK, OFF);

    got = want;
    got[BLK / 3] ^= 0x77;                                   // payload only
    EXPECT_NE(std::string::npos, stress_diagnose(got.data(), BLK, OFF).find("torn"))
        << stress_diagnose(got.data(), BLK, OFF);
}

TEST(Pattern, pattern_is_deterministic_and_seed_sensitive) {
    // every suite verifies device IO against pattern(), so a collision between
    // two seeds would silently weaken those checks
    EXPECT_EQ(pattern(0x5a, 4096), pattern(0x5a, 4096));
    EXPECT_NE(pattern(0x5a, 4096), pattern(0xa5, 4096));
    EXPECT_EQ(4096u, pattern(0x5a, 4096).size());
}

TEST(ConsumerIo, the_child_reports_success_and_a_clean_fd_table) {
    const char* path = "/tmp/photon-blk-consumer-file";
    int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(0, ::ftruncate(fd, (off_t)(OFF + BLK)));
    ::close(fd);
    DEFER(::unlink(path));

    auto wbuf = pattern(0x5a, BLK);
    std::vector<char> rbuf(BLK);
    ConsumerIoResult r = consumer_io(path, wbuf.data(), rbuf.data(), BLK, OFF);
    EXPECT_EQ(0, r.status);
    EXPECT_EQ(CONS_DONE, r.stage);
    EXPECT_TRUE(r.reaped);
    EXPECT_FALSE(r.hung);
    EXPECT_EQ(0, r.exit_code);
    EXPECT_EQ(nullptr, r.shm);
    EXPECT_EQ(0u, r.fds);   // see the inheritance case below for what this proves
}

TEST(ConsumerIo, the_child_does_not_inherit_the_callers_descriptors) {
    // A wedged consumer that still holds the descriptors of the process serving
    // the device holds the one reference whose release a re-attach needs, so the
    // drop is the load-bearing step of the isolation -- and the child censuses
    // its own table afterwards rather than trusting it. Both halves need a
    // caller that HAS descriptors to mean anything: with an empty parent table
    // the census passes whether or not the drop ran, and the guard is decoration.
    const char* path = "/tmp/photon-blk-consumer-inherit";
    int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(0, ::ftruncate(fd, (off_t)(OFF + BLK)));
    DEFER(::close(fd));
    DEFER(::unlink(path));
    int held[3];
    for (int& h : held)
        h = ::open(path, O_RDONLY);
    DEFER(for (int h : held) ::close(h));
    for (int h : held)
        ASSERT_EQ(0, ::fcntl(h, F_GETFD));   // the parent really is holding them

    auto wbuf = pattern(0x5a, BLK);
    std::vector<char> rbuf(BLK);
    ConsumerIoResult r = consumer_io(path, wbuf.data(), rbuf.data(), BLK, OFF);
    EXPECT_EQ(0, r.status);                     // the IO still ran
    EXPECT_EQ(0u, r.fds);                       // with none of them inherited
    EXPECT_NE((int)CONS_EXIT_FDLEAK, r.exit_code);
    // ...and the drop is the CHILD's only: a parent-side close here would look
    // identical to the child from the report's point of view
    EXPECT_EQ(0, ::fcntl(fd, F_GETFD));
    for (int h : held)
        EXPECT_EQ(0, ::fcntl(h, F_GETFD));
}

TEST(ConsumerIo, a_failure_carries_its_stage_and_errno_back) {
    const char* path = "/tmp/photon-blk-consumer-absent";
    ::unlink(path);
    auto wbuf = pattern(0x5a, 4096);
    std::vector<char> rbuf(4096);
    ConsumerIoResult r = consumer_io(path, wbuf.data(), rbuf.data(), 4096, OFF);
    // the child cannot log, so the verdict has to survive the exit status: the
    // stage says which syscall, the errno says why, and the exit code mirrors
    // the status so that even a lost report page still names the failure
    EXPECT_EQ(ENOENT, r.status);
    EXPECT_EQ(CONS_OPEN, r.stage);
    EXPECT_EQ(ENOENT, r.child_errno);
    EXPECT_EQ(ENOENT, r.exit_code);
    EXPECT_TRUE(r.reaped);
    EXPECT_EQ(nullptr, r.shm);
}

TEST(ConsumerIo, a_consumer_that_never_returns_costs_the_caller_a_deadline) {
    const char* path = "/tmp/photon-blk-consumer-fifo";
    ::unlink(path);
    ASSERT_EQ(0, ::mkfifo(path, 0600));
    DEFER(::unlink(path));

    auto wbuf = pattern(0x5a, 4096);
    std::vector<char> rbuf(4096);
    ConsumerIoOpts o;
    o.read_only = true;      // open() on a FIFO with no writer does not return
    o.timeout_us = 1000 * 1000;
    ConsumerIoResult r = consumer_io(path, wbuf.data(), rbuf.data(), 4096, OFF, o);

    // The caller is back and the consumer is not, which is the reason the
    // consumer is a child rather than a thread: a thread here takes the caller
    // with it, and with the caller goes everything it still had to do -- for a
    // suite that also serves the device under test, that is the device.
    EXPECT_EQ(ETIMEDOUT, r.status);
    EXPECT_TRUE(r.hung);
    EXPECT_FALSE(r.reaped);
    EXPECT_EQ(CONS_TIMEOUT, r.stage);
    EXPECT_GT(r.pid, 0);
    EXPECT_GE(r.elapsed_us, o.timeout_us);
    EXPECT_NE(nullptr, r.shm);

    // This one CAN be collected, and that is the difference between it and the
    // device case: a FIFO open is an interruptible wait. The device case is not,
    // which is why consumer_reap() waits and never signals.
    EXPECT_EQ(0, ::kill(r.pid, SIGKILL));
    // Through the harness's bounded primitive rather than a raw waitpid(): the
    // parent never waits on a consumer child without a deadline of its own,
    // however sure it is that this particular one will die.
    EXPECT_TRUE(consumer_reap(r, 5ull * 1000 * 1000));
    EXPECT_TRUE(r.reaped);
    EXPECT_FALSE(r.hung);
    // consumer_decode's signal branch: a signal is what ended this child, and the
    // signal number is what comes back as its exit code
    EXPECT_EQ(CONS_EXIT, r.stage);
    EXPECT_EQ(EIO, r.status);
    EXPECT_EQ(SIGKILL, r.exit_code);
    EXPECT_EQ(nullptr, r.shm);   // consumer_reap released the channel
}

}  // namespace test
}  // namespace blk
}  // namespace photon

int main(int argc, char** argv) {
    // A consumer child is this binary re-executed with a sentinel in argv[1]:
    // dispatch it before gtest sees that argument. There is no photon::init()
    // here at all -- nothing in this suite touches the runtime, which is the
    // point: the harness's verification logic must be checkable anywhere.
    int cons = photon::blk::test::consumer_child_main(argc, argv);
    if (cons != photon::blk::test::CONS_NOT_A_CHILD)
        return cons;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
