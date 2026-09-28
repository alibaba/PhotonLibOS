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
// The CONS_MODE_RO_WRITE pair. Two different values, and neither of them zero: the
// child echoes both back, and an echo cannot see a swap of two equal ones, while a
// zero len would stop at the decoder instead of reaching the write.
static constexpr uint64_t RW_OFF = 8192;
static constexpr uint64_t RW_LEN = 4096;

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
    ConsumerIoResult r = consumer_io(path, wbuf.data(), BLK, OFF);
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
    ConsumerIoResult r = consumer_io(path, wbuf.data(), BLK, OFF);
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
    ConsumerIoResult r = consumer_io(path, wbuf.data(), 4096, OFF);
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
    ConsumerIoOpts o;
    o.read_only = true;      // open() on a FIFO with no writer does not return
    o.timeout_us = 1000 * 1000;
    ConsumerIoResult r = consumer_io(path, wbuf.data(), 4096, OFF, o);

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

// ---------------------------------------------------------------------------
// the consumer child's argv encoding
//
// The scalars travel in argv rather than in the channel because argv is
// self-checking: every field is parsed and range-checked on its own, and a
// mis-decode is its own exit code rather than a plausible wrong number. That is
// what makes the encoding safe to change, so these cases pin it -- each row is a
// well-formed argv with exactly one thing wrong, spelled out here independently of
// harness.cpp's builders, because an encoding whose only other copy is the code
// that produces it cannot drift loudly.
// ---------------------------------------------------------------------------

// A well-formed single-IO argv against `path`, in decode_io_argv's order: mode
// tag, channel fd, flags, open_tries, write, off, len, path. argc 10 with argv[0].
static std::vector<std::string> io_argv(const std::string& path) {
    return {CONS_CHILD_ARG, std::to_string((int)CONS_MODE_IO), "3",
            std::to_string(O_RDWR), "200", "1", std::to_string(OFF), "4096", path};
}

// A well-formed stress argv, in decode_stress_argv's order: mode tag, channel fd,
// DISJOINT, 2 threads, 2 iterations, 1 MiB base, 4 MiB span, 64 KiB blocks,
// O_DIRECT, no flush, seed 0, path. argc 14 with argv[0].
static std::vector<std::string> stress_argv(const std::string& path) {
    return {CONS_CHILD_ARG, std::to_string((int)CONS_MODE_STRESS), "3",
            std::to_string((int)StressMode::DISJOINT), "2", "2",
            std::to_string(1ull << 20), std::to_string(4ull << 20),
            std::to_string(64ull << 10), "1", "0", "0", path};
}

// A well-formed read-only-write argv, in decode_ro_write_argv's order: mode tag,
// channel fd, off, len, path. argc 7 with argv[0].
static std::vector<std::string> ro_write_argv(const std::string& path) {
    return {CONS_CHILD_ARG, std::to_string((int)CONS_MODE_RO_WRITE), "3",
            std::to_string(RW_OFF), std::to_string(RW_LEN), path};
}

// `at` is an ARGV index, which is one more than the index into the vectors above:
// consumer_spawn_argv() takes argv[1] onwards.
static std::vector<std::string> patched(std::vector<std::string> a, int at,
                                        const std::string& v) {
    a[at - 1] = v;
    return a;
}
static std::vector<std::string> shortened(std::vector<std::string> a) {
    a.pop_back();   // one under the mode's argc, which is the path it loses
    return a;
}
static std::vector<std::string> over(std::vector<std::string> a) {
    a.push_back("0");   // one over the mode's argc: an exact gate must refuse
                        // this too, and "argc is at least the table" would not
    return a;
}

// The node the malformed rows point at does not exist, deliberately: a row that
// stops being rejected must not turn into a phase doing real IO. It still exits 0
// (a phase that ran and failed every open is not a structural failure), so the
// row goes red on the exit code either way -- just without moving any data.
TEST(ConsumerArgv, an_argv_that_does_not_decode_exits_202) {
    const std::string absent = "/tmp/photon-blk-consumer-argv-absent";
    ::unlink(absent.c_str());
    const auto io = io_argv(absent), st = stress_argv(absent),
               rw = ro_write_argv(absent);
    struct Row { const char* what; std::vector<std::string> argv; };
    const std::vector<Row> rows = {
        // the mode tag selects the rest of the table, so an unknown one is a
        // mis-decode and not a default -- and 3 is the row that pins the top of its
        // accepted range rather than merely flanking it. On the read-only-write argv
        // because that is the mode the top of the range names: on any other base a
        // relaxed bound lands in a half whose own argc gate refuses it, and this row
        // survives the relaxation it exists to catch.
        {"argv[2]: the mode tag is not a ConsumerMode",  patched(io, 2, "7")},
        {"argv[2]: the mode tag is one over its range",  patched(rw, 2, "3")},
        {"argc 3: too short for the tag and the fd",     {CONS_CHILD_ARG, "0"}},
        // the channel fd's range is decode_child_argv's own, so it is checked
        // ahead of either mode's half -- and BOTH bounds need a row: a bound
        // only flanked from one side survives being relaxed on the other
        {"argv[3]: the channel fd is below its range",   patched(io, 3, "2")},
        {"argv[3]: the channel fd is above its range",   patched(io, 3, "65536")},
        // single-IO mode, argc 10
        {"argc 9: one under the single-IO table",        shortened(io)},
        {"argc 11: one over the single-IO table",        over(io)},
        {"argv[5]: open_tries below its range",          patched(io, 5, "0")},
        {"argv[5]: open_tries above its range",          patched(io, 5, "1000001")},
        {"argv[6]: write is neither 0 nor 1",            patched(io, 6, "2")},
        {"argv[7]: off has a tail",                      patched(io, 7, "4096x")},
        {"argv[8]: len is not a number",                 patched(io, 8, "12x")},
        {"argv[8]: len is zero",                         patched(io, 8, "0")},
        // 2^40 + 1: exactly one over the len bound, which the "12x" and "0"
        // rows on either side of it cannot see
        {"argv[8]: len is above its range",              patched(io, 8, "1099511627777")},
        {"argv[9]: the path is empty",                   patched(io, 9, "")},
        // stress mode, argc 14
        {"argc 13: one under the stress table",          shortened(st)},
        {"argc 15: one over the stress table",           over(st)},
        {"argv[4]: the stress mode is not a StressMode", patched(st, 4, "9")},
        {"argv[4]: the stress mode is one over its range", patched(st, 4, "2")},
        {"argv[5]: threads below its range",             patched(st, 5, "0")},
        {"argv[5]: threads above its range",             patched(st, 5, "4097")},
        {"argv[6]: iters below its range",               patched(st, 6, "0")},
        {"argv[6]: iters above its range",               patched(st, 6, "1000001")},
        {"argv[7]: base_off is not 4K-aligned",          patched(st, 7, "1048577")},
        {"argv[8]: span is zero",                        patched(st, 8, "0")},
        {"argv[8]: span is not 4K-aligned",              patched(st, 8, "4194305")},
        {"argv[9]: max_block is zero",                   patched(st, 9, "0")},
        {"argv[10]: direct is neither 0 nor 1",          patched(st, 10, "2")},
        {"argv[11]: flush is not a number",              patched(st, 11, "x")},
        {"argv[11]: flush is neither 0 nor 1",           patched(st, 11, "2")},
        {"argv[12]: seed is over 32 bits",               patched(st, 12, "4294967296")},
        {"argv[13]: the path is empty",                  patched(st, 13, "")},
        // read-only-write mode, argc 7
        {"argc 6: one under the read-only-write table",  shortened(rw)},
        {"argc 8: one over the read-only-write table",   over(rw)},
        {"argv[4]: off has a tail",                      patched(rw, 4, "4096x")},
        {"argv[5]: len is not a number",                 patched(rw, 5, "12x")},
        {"argv[5]: len is zero",                         patched(rw, 5, "0")},
        // 1099511627777 is 2^40 + 1, exactly one over the `len > (1ull << 40)`
        // guard, which the two rows beside it cannot see. 2^40 itself is
        // 1099511627776 and the guard ACCEPTS it, so it is no row of this table --
        // and since len also sizes the channel, no case here can reach that bound
        // from the accepted side.
        {"argv[5]: len is above its range",              patched(rw, 5, "1099511627777")},
        {"argv[6]: the path is empty",                   patched(rw, 6, "")},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.what);
        ConsumerIoResult r = consumer_spawn_argv(row.argv, 10ull * 1000 * 1000);
        // 202 rather than a status: a mis-decode must not be able to read as
        // "nothing to do", and exit_code_stage() is what maps it to a stage.
        EXPECT_EQ(CONS_EXIT_ARGV, r.exit_code);
        EXPECT_EQ(CONS_ARGV, r.stage);
        EXPECT_EQ(EIO, r.status);
        EXPECT_TRUE(r.reaped);
        EXPECT_FALSE(r.hung);
        EXPECT_EQ(nullptr, r.shm);
    }
}

TEST(ConsumerArgv, an_argv_without_the_sentinel_is_refused_not_spawned) {
    // What a child that does not see the sentinel is, is this suite running again
    // inside a child -- so the refusal is the guard, and spawning to find out
    // would be the accident it exists to prevent.
    ConsumerIoResult r = consumer_spawn_argv({"--not-the-sentinel", "0"}, 1000 * 1000);
    EXPECT_EQ(EINVAL, r.status);
    EXPECT_EQ(CONS_ARGV, r.stage);
    EXPECT_EQ(-1, r.exit_code);      // nothing was spawned to have one
    EXPECT_EQ(-1, r.pid);
    EXPECT_EQ(nullptr, r.shm);
    ConsumerIoResult e = consumer_spawn_argv({}, 1000 * 1000);
    EXPECT_EQ(EINVAL, e.status);
}

TEST(ConsumerArgv, a_channel_that_did_not_arrive_exits_203) {
    // The channel arrives on a fixed descriptor and its number also travels in
    // argv, so "the channel did not survive the spawn" is indistinguishable from
    // "argv named a descriptor that is not there" -- which is what this hands
    // over. 65535 is the top of the range decode_child_argv accepts, so the row
    // pins that bound too: being in range says nothing about being open.
    auto a = patched(io_argv("/tmp/photon-blk-consumer-channel-absent"), 3, "65535");
    ConsumerIoResult r = consumer_spawn_argv(a, 10ull * 1000 * 1000);
    EXPECT_EQ(CONS_EXIT_CHANNEL, r.exit_code);
    EXPECT_EQ(CONS_CHANNEL, r.stage);
    EXPECT_EQ(EIO, r.status);
    EXPECT_TRUE(r.reaped);
    EXPECT_EQ(nullptr, r.shm);
}

TEST(ConsumerArgv, an_unmodified_stress_argv_runs_to_completion) {
    // The positive control for stress_argv(), which the table above only ever
    // hands over in refused forms: were its arity itself wrong, every stress row
    // would stop at the argc gate and return 202, and the table would look green
    // while testing nothing. This spawns the helper UNMODIFIED against a real
    // file, so every field of it is proven to decode. Safe here: the stress
    // channel is the report page alone, exactly what consumer_spawn_argv
    // allocates, and the O_DIRECT this helper spells falls back to buffered on
    // tmpfs through stress_open's own retry.
    const char* path = "/tmp/photon-blk-consumer-stress-control";
    int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(0, ::ftruncate(fd, (off_t)(8ull << 20)));   // holds base_off + span
    ::close(fd);
    DEFER(::unlink(path));
    ConsumerIoResult r = consumer_spawn_argv(stress_argv(path), 60ull * 1000 * 1000);
    EXPECT_EQ(0, r.status);
    EXPECT_EQ(0, r.exit_code);          // the phase ran: 0 is what a stress
    EXPECT_EQ(CONS_DONE, r.stage);      // child exits whenever it got that far
    EXPECT_TRUE(r.reaped);
    EXPECT_EQ(nullptr, r.shm);
}

// ---------------------------------------------------------------------------
// a write a node had to refuse, in a consumer child
//
// The suites export read-only nodes and assert that a write to one is rejected.
// What nothing there can say is whether the check would NOTICE a node that took
// the write, since every node those suites export refuses it -- so this is the
// other half, and it doubles as CONS_MODE_RO_WRITE's positive control: it goes
// through expect_write_rejected()'s own builder, its own spawn and its own channel
// sizing, so a mode that could not run at all fails here rather than only in the
// two suites that export a read-only node.
//
// consumer_spawn_argv() cannot stand in for that control. It hands over the report
// page alone whatever the argv says, and this mode sizes a payload region from the
// len inside it, so the child would map more than the page it was given.
// ---------------------------------------------------------------------------

TEST(RoWriteChild, a_node_that_takes_the_write_is_reported_not_passed_over) {
    // A regular file: open_node() is a bare open() with an ENXIO retry and has
    // nothing to say about the node being a device, so this needs no root and no
    // export. A regular file takes the write, which is the verdict under test.
    const char* path = "/tmp/photon-blk-consumer-ro-write";
    int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    // RW_OFF + RW_LEN, so the swapped pair (RW_LEN, RW_OFF) ends at the same byte
    // and still fits: a swap of those two argv slots then changes nothing the child
    // can notice -- the write is still taken -- and only the echo sees it.
    ASSERT_EQ(0, ::ftruncate(fd, (off_t)(RW_OFF + RW_LEN)));
    ::close(fd);
    DEFER(::unlink(path));
    // This process holds descriptors on the very file the child is about to use, so
    // the child's census of 0 proves its drop ran -- with an empty parent table the
    // census passes either way.
    int held[3];
    for (int& h : held)
        h = ::open(path, O_RDONLY);
    DEFER(for (int h : held) ::close(h));
    for (int h : held)
        ASSERT_EQ(0, ::fcntl(h, F_GETFD));   // the parent really is holding them

    ConsumerIoResult rep;
    EXPECT_EQ(EILSEQ, expect_write_rejected(path, RW_OFF, RW_LEN, &rep));
    // The stage is why this enumerator exists: CONS_WRITE names a pwrite that
    // FAILED in every other mode, and the report carries no mode, so a reader handed
    // CONS_WRITE here could not tell which of the two opposites happened.
    EXPECT_EQ(CONS_RO_ACCEPTED, rep.stage);
    EXPECT_EQ(EILSEQ, rep.status);
    EXPECT_EQ(EILSEQ, rep.exit_code);   // the exit code mirrors the status
    EXPECT_EQ(0, rep.child_errno);      // no syscall failed; the write is the finding
    // ... and it happened in a CHILD, born with a clean fd table even though this
    // process is holding four descriptors on the same file.
    EXPECT_GT(rep.pid, 0);
    EXPECT_NE((pid_t)::getpid(), rep.pid);
    EXPECT_EQ(0u, rep.fds);
    EXPECT_TRUE(rep.reaped);
    EXPECT_FALSE(rep.hung);
    EXPECT_EQ(nullptr, rep.shm);
    for (int h : held)
        EXPECT_EQ(0, ::fcntl(h, F_GETFD));   // the drop was the child's only
}

TEST(RoWriteChild, a_node_that_is_not_there_comes_back_as_the_childs_errno) {
    // The open's errno is the child's and reaches the caller through the same report
    // a verdict would, so a node that never appeared is not mistaken for one that
    // refused nothing. It also puts the echo comparison on a report whose stage is
    // not the happy one: this body ran, so it echoed, and off and len still have to
    // come back as sent.
    const char* absent = "/tmp/photon-blk-consumer-ro-write-absent";
    ::unlink(absent);
    ConsumerIoResult rep;
    EXPECT_EQ(ENOENT, expect_write_rejected(absent, RW_OFF, RW_LEN, &rep));
    EXPECT_EQ(CONS_OPEN, rep.stage);
    EXPECT_EQ(ENOENT, rep.child_errno);
    EXPECT_EQ(ENOENT, rep.exit_code);
    EXPECT_TRUE(rep.reaped);
    EXPECT_FALSE(rep.hung);
    EXPECT_EQ(nullptr, rep.shm);
}

// ---------------------------------------------------------------------------
// a stress phase in a consumer child
//
// The counters and the first error are all a phase can say, and they cross a
// process boundary in a fixed-width POD at an offset both sides derive from the
// same function. A parent that read them where the child did not write them would
// read zeros -- and zero failures is a pass, so these cases assert the values, not
// merely that they are nonzero.
// ---------------------------------------------------------------------------

TEST(StressChild, a_phase_runs_in_the_child_and_its_counters_come_back) {
    const char* path = "/tmp/photon-blk-stress-child";
    constexpr uint64_t SIZE = 8ull << 20;
    int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(0, ::ftruncate(fd, (off_t)SIZE));
    DEFER(::close(fd));
    DEFER(::unlink(path));
    // The same shape as the single-IO inheritance case above: the parent holds
    // extra descriptors on the very file the phase is about to use, so the
    // child's census of 0 proves its drop ran -- with an empty parent table the
    // census would pass either way.
    int held[3];
    for (int& h : held)
        h = ::open(path, O_RDONLY);
    DEFER(for (int h : held) ::close(h));
    for (int h : held)
        ASSERT_EQ(0, ::fcntl(h, F_GETFD));   // the parent really is holding them

    StressCfg c;
    c.node = path;
    c.size = SIZE;
    c.threads = 2;
    c.iters = 2;
    c.flush = true;        // direct and flush must not carry the same value: an
                           // echo of two equal scalars cannot see their slots
                           // swapped. (i % 8) == 7 never fires at 2 iters, so
                           // this changes what is encoded and nothing else.
    c.max_block = 64 << 10;
    c.direct = false;      // a regular file on tmpfs rejects O_DIRECT

    StressResult d = stress_run(c);
    EXPECT_EQ(0, d.failures) << d.first_error;
    // The first half of this case's name: the phase ran in a CHILD, not in this
    // process -- its pid is not ours, and it was born with a clean fd table even
    // though this process is holding four descriptors on the same file.
    EXPECT_GT(d.child_pid, 0);
    EXPECT_NE((pid_t)::getpid(), d.child_pid);
    EXPECT_EQ(0u, d.child_fds);
    EXPECT_EQ(4u, d.ios);                 // every worker completed every iteration
    EXPECT_GT(d.bytes, 0u);               // the total is whatever the seeded RNG
                                          // picks, deliberately not pinned:
                                          // recomputing it here would be a second
                                          // copy of the driver's own block-size
                                          // logic, free to drift. A phase that
                                          // moved no IO drives this to 0, and
                                          // that is caught
    EXPECT_TRUE(d.first_error.empty());
    EXPECT_GT(d.elapsed_us, 0u);

    c.mode = StressMode::SHARED;
    StressResult s = stress_run(c);
    EXPECT_EQ(0, s.failures) << s.first_error;
    EXPECT_NE((pid_t)::getpid(), s.child_pid);
    EXPECT_EQ(4u, s.ios);
    EXPECT_EQ(4u * 64u * 1024u, s.bytes);   // SHARED's slot is fixed, so this is exact
    EXPECT_TRUE(s.first_error.empty());
}

TEST(StressChild, a_real_failure_reaches_the_parent_verbatim) {
    // errbuf is a fixed 320 bytes across the boundary and becomes a std::string
    // on this side, so a truncation or a dropped copy would show up here as a
    // failure with no stated reason -- which no suite asserts on and no reader
    // could diagnose.
    const char* absent = "/tmp/photon-blk-stress-child-absent";
    ::unlink(absent);
    StressCfg c;
    c.node = absent;
    c.size = 8ull << 20;   // the span check is the parent's and it passes: that
    c.threads = 2;         // the node is absent is the child's to discover
    c.iters = 1;
    c.max_block = 64 << 10;
    c.direct = false;
    StressResult r = stress_run(c);
    EXPECT_EQ(2, r.failures);            // one per worker
    EXPECT_EQ(0u, r.ios);
    EXPECT_NE(std::string::npos, r.first_error.find("open failed")) << r.first_error;
    EXPECT_NE(std::string::npos, r.first_error.find(strerror(ENOENT))) << r.first_error;
}

TEST(StressChild, a_config_the_child_rejects_is_named_not_silently_zero) {
    // threads one over the [1, 4096] decode_stress_argv accepts: the child is
    // stopped at its argv before it spawns a single worker, and the parent has
    // to NAME that -- the zeros a channel is born with would otherwise read as
    // "a clean phase that moved no IO". This is also the real builder's witness
    // against the decoder's ranges: the table above spells its argv by hand,
    // while this config travels through stress_in_child's own encoding.
    const char* absent = "/tmp/photon-blk-stress-child-rejected";
    ::unlink(absent);
    StressCfg c;
    c.node = absent;
    c.size = 8ull << 20;   // the span check is the parent's and it passes: the
    c.threads = 4097;      // rejection is the child's, at decode time
    c.iters = 1;
    c.max_block = 64 << 10;
    c.direct = false;
    StressResult r = stress_run(c);
    EXPECT_EQ(1, r.failures);
    EXPECT_EQ(0u, r.ios);
    EXPECT_NE(std::string::npos, r.first_error.find("never reported a stress phase"))
        << r.first_error;
    EXPECT_NE(std::string::npos, r.first_error.find("decoding its argv")) << r.first_error;
    EXPECT_GT(r.child_pid, 0);   // spawned and reaped, though the phase never ran
}

TEST(StressChild, a_span_that_does_not_fit_is_caught_before_any_spawn) {
    // A configuration check, not IO: it needs no descriptor, so it stays in the
    // parent. The node below does not exist, which is what makes the two
    // distinguishable -- had this spawned, the child's own open failure would be
    // in first_error instead.
    StressCfg c;
    c.node = "/tmp/photon-blk-stress-child-unspawned";
    c.size = 4096;
    c.base_off = 1ull << 20;   // leaves no span at all
    StressResult r = stress_run(c);
    EXPECT_EQ(1, r.failures);
    EXPECT_EQ(0u, r.ios);
    EXPECT_NE(std::string::npos, r.first_error.find("the span does not fit the device"))
        << r.first_error;
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
