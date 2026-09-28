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

// Shared helpers for the blk/ test suites: off-vcpu primitives, single-shot
// device IO, and the high-concurrency stress driver. Test-local (not installed,
// not part of the library): the suites include this header and compile
// harness.cpp alongside their own source.

#include <photon/common/callback.h>     // TempDelegate
#include <photon/fs/filesystem.h>       // fs::IFile
#include <photon/thread/thread.h>       // vcpu_base, get_vcpu, mutex
#include <photon/thread/workerpool.h>   // WorkPool

#include <atomic>
#include <cerrno>                 // EIO, ETIMEDOUT in the defaults below
#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>            // pid_t
#include <thread>
#include <vector>

namespace photon {
namespace blk {
namespace test {

// ---------------------------------------------------------------------------
// off-vcpu primitives
//
// Blocking device IO must never run on the photon vcpu that serves the device
// under test: the vcpu stalls, the IO cannot be served, and it only returns
// through the kernel's timeout paths (tcmu cmd_time_out, nbd timeout, ublk
// quiesce) -- tens of seconds, or a hang.
// ---------------------------------------------------------------------------

// Run a blocking callable on a fresh OS thread and yield the vcpu until it
// returns: blk::run_off_vcpu (utils.h) for a callable with nothing to report.
// Synchronous, so a temporary lambda is safe (the TempDelegate contract).
// A caller with no photon thread runs it inline: it has no vcpu to yield, and
// the hand-back needs one.
void run_off_vcpu(TempDelegate<void> fn);

// Open a device node, retrying the not-ready race (ENXIO while the driver and
// udev settle). CALL FROM OFF THE VCPU (e.g. inside run_off_vcpu).
int open_node(const std::string& node, int mode, int tries = 200);

// Run a shell command off the vcpu and capture its stdout. Tools like `vdpa`
// block in netlink calls that converse with OUR OWN daemon's message loop, so
// running them on the vcpu would deadlock it.
std::string sh_off_vcpu(const std::string& cmd, int* rc = nullptr);

// ---------------------------------------------------------------------------
// fixture scaffolding
// ---------------------------------------------------------------------------

// The backend image a suite exports: a localfs file created fresh at `path` and
// sized to `size`, removed when this goes out of scope. Every suite's SetUp and
// TearDown used to hand-roll exactly this.
struct TestImage {
    fs::IFileSystem* lfs = nullptr;
    fs::IFile* file = nullptr;
    std::string path;

    // 0 on success, errno on failure (logged). Requires photon::init().
    int create(const char* p, uint64_t size);
    // drop the handles but keep the file on disk: TearDown does this before the
    // suite's sweep (which may still open the image); the destructor unlinks it
    void release();
    ~TestImage();
};

// ---------------------------------------------------------------------------
// vcpu placement observation
//
// BlkConfig::pool's whole contract is WHERE the serving coroutines run, and
// nothing in a transport's public API reports that. All five transports take an
// fs::IFile* and all five perform the backend IO from the coroutine that serves
// the request, so a pass-through file that records photon::get_vcpu() on each
// positioned IO answers the question for every transport at once -- and the
// answer is the ground truth, not a proxy for it.
// ---------------------------------------------------------------------------
class RecordingFile : public fs::IFile {
public:
    // does NOT own f; f must outlive this (declare it before the device, so the
    // device's shutdown DEFER -- and therefore its last IO -- happens first)
    explicit RecordingFile(fs::IFile* f) : m_file(f) {}

    // distinct vcpus that ran a backend IO, in first-seen order
    std::vector<photon::vcpu_base*> vcpus();
    size_t vcpu_count();
    bool ran_on(photon::vcpu_base* v);
    void reset();
    fs::IFile* underlying() { return m_file; }

    // Hold every backend IO inside record() until release_gate(), or until a
    // teardown interrupts it -- see record() for that one exit. The purpose is to
    // PIN a device state that is otherwise transient: with all of a virtqueue's
    // dispatched requests parked here at once, in_flight stays at the dispatch cap
    // for as long as the gate is shut, so the avail entries behind the cap stay
    // unconsumed INDEFINITELY rather than for one KICK_FALLBACK_US interval. That is
    // what makes an orderly teardown's "drain the backlog, THEN stop the queue"
    // order assertable at all -- ungated, the window the drain has to win is a few
    // microseconds wide and an assertion on it would be a timing guess. Off by
    // default, so an ungated RecordingFile stays exactly the placement probe above.
    bool gated = false;
    // Resume every parked IO at once. `n` only has to be at least the number still
    // parked, which a teardown's interrupt can make fewer than reached the gate;
    // whatever is left over stays in the count and is harmless.
    // Clears `gated` FIRST: an IO arriving after the release must not park on a
    // gate nobody is going to open again.
    void release_gate(uint64_t n = 1) {
        gated = false;
        gate.signal(n);
    }

    // The 17 pure virtuals of IStream + IFile, each a one-line forward, plus the
    // two non-pure virtuals the transports do reach (see below). The IO entry
    // points call record() first; the metadata ones do not, because they are not
    // what a serving coroutine does with a request.
    photon::fs::IFileSystem* filesystem() override;
    ssize_t pread (void* buf, size_t count, off_t offset) override;
    ssize_t preadv(const struct iovec* iov, int iovcnt, off_t offset) override;
    ssize_t pwrite(const void* buf, size_t count, off_t offset) override;
    ssize_t pwritev(const struct iovec* iov, int iovcnt, off_t offset) override;
    // nbd, tcmu and ublk all issue an FUA write as pwritev2(..., RWF_DSYNC). The
    // base default discards `flags` and calls pwritev, which would still record
    // the placement but silently drop the durability request -- and a placement
    // probe that quietly changes what the backend was asked to do is a trap for
    // whoever asserts on it next.
    ssize_t pwritev2(const struct iovec* iov, int iovcnt, off_t offset, int flags) override;
    ssize_t read  (void* buf, size_t count) override;
    ssize_t readv (const struct iovec* iov, int iovcnt) override;
    ssize_t write (const void* buf, size_t count) override;
    ssize_t writev(const struct iovec* iov, int iovcnt) override;
    off_t lseek(off_t offset, int whence) override;
    int fsync() override;
    int fdatasync() override;
    int fchmod(mode_t mode) override;
    int fchown(uid_t owner, gid_t group) override;
    int fstat(struct stat* buf) override;
    int ftruncate(off_t length) override;
    // Also not pure virtual, and also reached: IFile::trim() and
    // IFile::zero_range() are plain methods that call the VIRTUAL fallocate
    // (fs/virtual-file.cpp), and every transport uses them (nbd TRIM /
    // WRITE_ZEROES, tcmu and ublk UNMAP / WRITE_ZEROES, the shared virtio path
    // in utils.cpp). The inherited UNIMPLEMENTED default answers ENOSYS, which
    // nbd maps to NBD_ENOTSUP -- so TRIM would fail and WRITE_ZEROES would not
    // even take its EOPNOTSUPP fallback.
    int fallocate(int mode, off_t offset, off_t len) override;
    int close() override;

private:
    void record();
    fs::IFile* m_file;
    // NSDMI, not {}: semaphore's ctor is explicit. In-order (the ctor's default),
    // deliberately: that is the mode in which an interrupted wait_interruptible
    // wakes the waiters queued behind it to take the count it did not, while
    // out-of-order leaves that count for the next signal() to hand over.
    photon::semaphore gate{0};
    photon::mutex m_lock;
    std::vector<photon::vcpu_base*> m_vcpus;

    RecordingFile(const RecordingFile&) = delete;
    RecordingFile& operator=(const RecordingFile&) = delete;
};

// A WorkPool whose vcpus are initialized with exactly the engines the calling
// vcpu has, which is what check_pool_engines() requires. Spelling an engine
// name at the call sites instead would encode today's recommended_order (epoll
// ahead of iouring) as if it were a contract, and would be wrong on macOS,
// where the caller's engine is kqueue or select.
struct TestPool {
    photon::WorkPool* pool;
    explicit TestPool(size_t n);
    ~TestPool();
    photon::WorkPool* operator->() const { return pool; }
    operator photon::WorkPool*() const { return pool; }

    TestPool(const TestPool&) = delete;
    TestPool& operator=(const TestPool&) = delete;
};

// The kernel's own answer to "how many hardware queues does this device have":
// blk-mq creates /sys/block/<name>/mq/<hctx index>/ for each one. Counting those
// directories is an oracle that does not read anything our code wrote, which is
// the property a queue-count assertion needs -- reading VIRTIO_BLK_F_MQ or
// virtio_blk_config::num_queues back from our own device would only prove we are
// consistent with ourselves.
//
// `name` is the BARE kernel name ("ublkb0", "vda"), not a /dev path and not a
// sysfs path; the /sys/block/<name>/mq prefix is built here. Returns -1 when that
// directory does not exist, which is itself a useful signal: it means the driver
// never bound, not that the queue count is zero.
int count_mq_dirs(const std::string& name);

// ---------------------------------------------------------------------------
// consumer-side process isolation
//
// A consumer IO on an exported node has no completion bound of its own: if the
// serving side stops while a request is in flight, only a daemon that attaches
// again can complete it. Doing that IO inside the test process is what turned
// "stuck" into "unrecoverable", measurably and once already: the process has ONE
// fd table and the daemon shares it, so a consumer thread parked in an
// uninterruptible sleep kept the daemon's control device referenced for as long
// as the process existed, while releasing it is a precondition of the re-attach
// that alone could complete the IO. That process was unkillable for two days.
//
// So the consumer is another process -- and a SPAWNED one: the child re-executes
// this same binary with a sentinel argument that every suite's main() dispatches
// on before photon::init() and before gtest ever sees argv.
//
// Why a spawn and not a fork, which is the smaller change: a fork copies the
// address space, and every daemon here holds MAP_SHARED mappings -- of its
// control device, or of memory a peer handed it. A shared mapping holds the
// opened file referenced exactly as an fd does, so a forked child that had
// dropped every descriptor still kept the control device referenced, the
// release a re-attach needs never ran, and the device stayed unrecoverable.
// That was measured on a child whose fd table had just been verified empty.
// exec drops the address space, which makes the isolation structural instead of
// enumerative: no filter over "which mappings count" can be complete, and an
// incomplete one fails silently in exactly the way above.
//
// The fd half is structural only as far as O_CLOEXEC reaches, and not everything
// this process holds sets it. So the child still drops the whole table itself
// and then censuses what is left, expecting exactly {0,1,2}: the census is what
// turns "every descriptor the daemon opens is CLOEXEC" from an assumption about
// code elsewhere into a property this code checks.
//
// One constraint a spawn removes: between fork and exec a child of a
// multi-threaded process may only call async-signal-safe functions, because a
// lock another thread held at the fork instant stays held forever. This child is
// past the exec and single-threaded, so it may allocate, log and use photon --
// which is what lets a consumer path reuse the harness code it already calls.
//
// Measured, on a consumer wedged on a real device: the child held nothing of the
// daemon's. Its descriptor table was the three it was born with plus the node it
// had opened, its address space had no device mapping in it, and the daemon's
// control device showed no holder but the daemon. The recovery the isolation
// exists to make possible is measured too, and by the daemon that stopped
// serving: it took the registration back, destroyed it while that consumer was
// still wedged on the node, and the consumer then drained on its own and reported
// the IO it was doing when the serving side vanished as successful.
//
// That last step has a precondition worth stating, because it is a property of the
// probe and not of the transport: stopping has to wake whatever is parked inside
// the backend, so a backend that parks must park interruptibly. RecordingFile's
// gate does. A backend that cannot be woken does not merely fail a case -- the
// stop that is waiting for it has no deadline of its own.
// ---------------------------------------------------------------------------

// The child's record of what it did, at offset 0 of the result channel below.
struct ConsumerReport {            // POD, shared with the child
    uint32_t magic;                // CONSUMER_MAGIC once `status` is final
    int32_t status;                // 0, an errno, or EILSEQ
    int32_t stage;                 // which step produced it (ConsumerStage)
    int32_t child_errno;           // errno at that step
    uint32_t fds;                  // the child's own census after the drop
};

// A stress phase's counters, plus what a child echoes its argv back as, in the
// same report page as the ConsumerReport above it and written by the same child.
// Kept out of ConsumerReport because that one is what EVERY consumer writes and
// what the single-IO cases assert on: 320 bytes of error text folded into it would
// be carried by every child to report a phase it never ran. A mode with no counters
// of its own still has echoes, and this POD is the only room the page has for them.
struct ConsumerStressReport {      // POD, shared with the child
    uint64_t ios;                  // completed write+read-back pairs
    uint64_t bytes;                // bytes written (== bytes read back)
    int32_t failures;              // syscall + validation errors
    char errbuf[320];              // the first error. The width is
                                   // StressCounters::errbuf's, not a new one: it
                                   // is the buffer this one is copied out of.
    // What the child decoded its config argv into, for the parent to compare
    // against what it sent. Range checks alone cannot witness a swap of two
    // fields whose accepted ranges are the same -- a direct that lands in
    // flush's slot still decodes cleanly -- so every scalar is echoed back and
    // a slot disagreement becomes a failure that names the field. The path is
    // not echoed: it is the argv's only non-numeric element, so any slot swap
    // involving it fails a parse instead of decoding silently.
    uint64_t echo_base_off;
    uint64_t echo_span;
    uint64_t echo_max_block;
    uint32_t echo_mode;            // StressMode
    uint32_t echo_threads;
    uint32_t echo_iters;
    uint32_t echo_direct;
    uint32_t echo_flush;
    uint32_t echo_seed;
    // CONS_MODE_RO_WRITE's, which is the whole of what its argv carries. Here and
    // not in a POD of its own: this one is already in the page and has room, and an
    // offset is a pair of numbers the two sides have to derive identically, so
    // there is a cost to each new one. The writer's counters below are the
    // exception, and what makes them one is that they are read while the child is
    // still running -- which no field of this POD is, every reader of these having
    // collected the child that wrote them first.
    uint64_t echo_off;
    uint64_t echo_len;
};

// The writer's counters, in the same report page as the two PODs above it and at an
// offset both sides derive from the same function. Unlike those two, this one is
// read WHILE the child writing it is still running: a caller synchronizes on a
// writer's progress from the outside (wait_iters), which is the only way to say
// "the writer has got going" about IO this process is serving. So none of these
// fields are published behind ConsumerReport::magic -- the magic is written last
// precisely because it says the rest is final, and a writer's counters are not
// final until it is stopped.
//
// std::atomic rather than photon::semaphore here is NOT a departure from the
// project's rule that completion between threads uses a semaphore: that rule is
// about threads of one process, and a semaphore's contract does not cross a process
// boundary at all, so an atomic in a shared mapping is the only thing available.
//
// A cross-process std::atomic is safe only when it is lock-free: a non-lock-free
// one falls back to a lock table that belongs to the process, so the two sides
// would lock different tables and the counter would have no mutual exclusion at
// all. Hence one assert per member below, each taking the type it checks from the
// member itself: an assert that named a width instead would keep passing over a
// member that had stopped being an atomic at all. They are about these members, not
// about atomics in general, and they are the reason the members are the widths they
// are.
struct ConsumerWriterReport {      // shared with the child; constructed, not cleared
    std::atomic<uint64_t> iters{0};      // blocks completed, published as they go
    std::atomic<int32_t> errors{0};      // the writer's own failures, plus the
                                         // parent's finding about its argv (below)
    std::atomic<int32_t> stop_flag{0};   // the parent's: leave the loop
    // No "the loop is behind me" flag: a writer's exit IS that fact, and the parent
    // learns of the exit through consumer_reap()'s own bounded wait, which is what
    // makes the counters and the report final together. A second copy of the same
    // fact is a second thing to get out of order.
    // What the child decoded its config argv into, for the parent to compare
    // against what it sent -- the same mechanism and the same reason as the two
    // PODs above: six fields whose accepted ranges overlap pairwise, so a swap of
    // two of them decodes cleanly and would otherwise be invisible.
    uint64_t echo_off;
    uint64_t echo_wrap_at;
    uint64_t echo_block;
    uint32_t echo_direct;
    uint32_t echo_advance;
    uint32_t echo_verify;
};
static_assert(decltype(ConsumerWriterReport::iters)::is_always_lock_free,
              "ConsumerWriterReport::iters crosses a process boundary, so a "
              "non-lock-free atomic would leave it guarded by a per-process lock");
static_assert(decltype(ConsumerWriterReport::errors)::is_always_lock_free,
              "ConsumerWriterReport::errors crosses a process boundary, so a "
              "non-lock-free atomic would leave it guarded by a per-process lock");
static_assert(decltype(ConsumerWriterReport::stop_flag)::is_always_lock_free,
              "ConsumerWriterReport::stop_flag crosses a process boundary, so a "
              "non-lock-free atomic would leave it guarded by a per-process lock");

// The result channel: ONE fd-backed shared object, mapped by both sides, handed
// to the child as a descriptor. Not an anonymous mapping -- exec replaces the
// whole address space, so only what the child is handed as an fd survives into
// it. Not a pipe either -- the child drops every descriptor it was handed, and
// the verdict has to outlive that, which a mapping does and a pipe cannot.
//
// Layout, derived by BOTH sides from (mode, len) through the same function
// (channel_layout(), harness.cpp): the parent from the mode and length it is
// handing over, the child from the mode and length in its argv. Never worked out
// once per side, because two copies of these offsets can drift, and a parent that
// reads its report where the child did not write one reads zeros -- and zero is
// "nothing failed", so the case stays green and lies.
//   [0, page_align(all three report PODs))  ConsumerReport, then the other two
//   [.., + page_align(len))             the write payload, the caller's
//   [.., + page_align(len))             the read-back, the child's
// Page-aligned so an O_DIRECT consumer can use both buffers as they are. Two modes
// carry no payload because they allocate their own IO buffers -- a stress child's
// workers each posix_memalign a pair, and a writer posix_memaligns the block it
// hammers -- so for those the channel is the report page alone.
//
// The child's argv, which is how the mode and the length above reach it. argv[0]
// is the suite's own binary, argv[1] the sentinel consumer_child_main() dispatches
// on, argv[2] the mode tag, argv[3] the descriptor the channel arrived on, then
// the mode's own fields, then the node path LAST -- argv elements pass through
// verbatim, so a path needs no quoting, while anything packed into one element
// would need a separator the path could contain.
//
// Every field is a decimal integer, range-checked on its own, and argc is per
// mode: a mis-decode has to be loud, because the alternative is a field read out
// of the wrong slot yielding a plausible number and a silent wrong IO. That is
// also why the scalars travel here rather than in the channel, whose fields
// nothing checks -- a wrong offset there is just a wrong number.
//
//   mode tag              argc  fields after argv[3], with their accepted range
//   CONS_MODE_IO            10  flags [0, INT_MAX], open_tries [1, 1e6],
//                               write [0, 1], off, len [1, 2^40], path
//   CONS_MODE_STRESS        14  stress_mode [DISJOINT, SHARED], threads [1, 4096],
//                               iters [1, 1e6], base_off (4K-aligned), span
//                               (4K-aligned, nonzero), max_block (nonzero),
//                               direct [0, 1], flush [0, 1], seed [0, 2^32-1],
//                               path
//   CONS_MODE_RO_WRITE      7   off, len [1, 2^40], path. Three of the single-IO
//                               table's fields dropped -- flags, open_tries and
//                               write: this mode is always O_RDWR, always writes,
//                               and the retry budget it needs is the one
//                               open_node() already defaults to.
//   CONS_MODE_WRITER        11  off, wrap_at (0 = never wrap), block [4096, 2^40],
//                               direct [0, 1], advance [0, 1], verify [0, 1], path.
//                               Six fields because BackgroundWriter::Opts has six
//                               and every one of them changes what the child does,
//                               so every one has to arrive in the slot it was sent
//                               in. `block`'s lower bound is start()'s own check;
//                               its upper bound is the child's, and is the same
//                               2^40 as `len` above for the same reason -- it is a
//                               size this side has to allocate.
enum ConsumerMode : int32_t {
    CONS_MODE_IO       = 0,   // one write+fsync+read-back: consumer_io()
    CONS_MODE_STRESS   = 1,   // a whole stress phase: stress_run()
    CONS_MODE_RO_WRITE = 2,   // a write a read-only node must refuse: expect_write_rejected()
    // A writer that hammers one region until it is told to stop:
    // BackgroundWriter. The only mode whose child does not end on its own, which is
    // why its spawn is the one that does not wait and its caller has a bounded
    // stop() of its own.
    CONS_MODE_WRITER   = 3,
};

// Where a consumer IO ended. Also how the caller's own failure to even start
// one is reported, so that a single `stage` names the culprit in every case.
enum ConsumerStage : int32_t {
    CONS_DONE = 0,   // nothing failed
    CONS_OPEN,       // the child's open() on the node
    CONS_WRITE,      // its pwrite()
    CONS_SYNC,       // its fsync()
    CONS_READ,       // its pread()
    CONS_VERIFY,     // the read-back comparison
    // CONS_MODE_RO_WRITE alone: the node took a write it had to refuse. Its own
    // value and not CONS_WRITE, which in every other mode names a pwrite that
    // FAILED -- and ConsumerReport carries no mode, so one enumerator cannot say
    // both without the reader having to guess which one it meant.
    CONS_RO_ACCEPTED,
    CONS_DROP_FDS,   // it could not drop the inherited fd table
    CONS_FDLEAK,     // its census found fds that survived the drop
    CONS_ARGV,       // its argv did not decode
    CONS_CHANNEL,    // it could not map the result channel
    CONS_EXEC,       // the exec never happened, so the child never ran
    CONS_EXIT,       // it ended without writing a report
    CONS_SPAWN,      // the caller's posix_spawn()
    CONS_MAPPING,    // the caller's result channel
    CONS_TIMEOUT,    // the deadline passed with the child still running
};
const char* consumer_stage_name(ConsumerStage s);

// The child's exit codes above the status range. `_exit(status)` carries 0 or
// an errno, so these sit where no errno can reach them -- otherwise a structural
// failure of the isolation itself would read as an IO error.
enum ConsumerExit : int32_t {
    CONS_EXIT_DROP_FDS = 200,
    CONS_EXIT_FDLEAK   = 201,
    CONS_EXIT_ARGV     = 202,
    CONS_EXIT_CHANNEL  = 203,
};

// Returned by consumer_child_main() when argv is not the sentinel, i.e. when this
// process is a suite and not a consumer child. Negative, so no exit code -- which
// is 0..255 -- can be mistaken for it.
constexpr int CONS_NOT_A_CHILD = -1;

// argv[1] of a consumer child, the sentinel consumer_child_main() dispatches on.
// Exposed because a case that spawns a child of its own has to spell it exactly:
// a child that does not see it is not a consumer that failed, it is this suite
// running again inside a child -- and every consumer case in that suite spawning
// another one.
extern const char CONS_CHILD_ARG[];

// The consumer child's entire program. Every blk suite's main() must call this
// FIRST, ahead of photon::init() and of InitGoogleTest(): the child needs no
// vcpu, and gtest must never be handed the sentinel argument it was spawned
// with. Returns the child's exit code, or CONS_NOT_A_CHILD.
int consumer_child_main(int argc, char** argv);

struct ConsumerIoOpts {
    bool read_only = false;      // skip the write+fsync, just read the range
    bool direct = false;         // O_DIRECT
    int open_tries = 200;        // ENXIO retry budget for the not-ready race
    // How long the caller waits. On expiry the child is ABANDONED -- never
    // killed, an uninterruptible sleeper ignores signals -- and the call
    // reports ETIMEDOUT with `hung` set. 0 selects the default below, which is
    // deliberately far above a served IO: that one completes in milliseconds.
    uint64_t timeout_us = 0;
};
constexpr uint64_t CONSUMER_TIMEOUT_US = 60ull * 1000 * 1000;

// What a consumer's fd census reads as when there was no census. The child only
// ever writes one into the channel beside a final report, so on the paths that
// produce no report -- an abandoned child, a structural exit -- the field in the
// page is one of the zeros the channel was born with, and copying that out would
// read as "censused, and clean" when nothing was censused. A caller handed the
// child can still get the real number: consumer_reap() decodes it.
constexpr uint32_t CONS_FDS_UNMEASURED = UINT32_MAX;

struct ConsumerIoResult {
    int status = EIO;            // 0, an errno, EILSEQ, or ETIMEDOUT if abandoned
    ConsumerStage stage = CONS_DONE;
    int child_errno = 0;         // the child's errno at `stage`
    int exit_code = -1;          // raw, or -1 while the child has not exited
    pid_t pid = -1;
    bool hung = false;           // the deadline passed with the child running
    bool reaped = false;         // waitpid() collected it and read its report
    uint32_t fds = CONS_FDS_UNMEASURED;   // its post-drop census: any other
                                          // nonzero is a bug
    uint64_t elapsed_us = 0;
    std::string node;            // for the log lines, including consumer_reap's
    // The result channel, held mapped while the child may still write into it --
    // which an abandoned one does. consumer_reap() reads it and releases it; a
    // caller that gives up on the child must release it itself.
    void* shm = nullptr;
    size_t shm_size = 0;         // the channel's, which depends on `len`
};

// Run one write+fsync+read-back (or, with read_only, one read) against `node`
// in a spawned child process, and report what it did. `wbuf` is `len` bytes and
// is copied into the result channel before the spawn. The read-back stays in the
// channel and the comparison happens IN the child: it is the only side that sees
// both the payload and what the device returned, and it is the side that
// survives the caller giving up.
//
// CALL FROM OFF THE VCPU (e.g. inside run_off_vcpu): the wait is a bounded
// WNOHANG/nanosleep poll, and running that on the vcpu would stall the very
// coroutines that have to serve the IO being waited on.
//
// A result with `hung` set owns a child that is still running and a channel that
// is still mapped: finish with consumer_reap(), or give up on it and call
// consumer_release().
ConsumerIoResult consumer_io(const std::string& node, const void* wbuf, size_t len,
                             uint64_t off, const ConsumerIoOpts& o = {});

// Wait out a child that consumer_io() abandoned, then finish reading its
// report. Returns true once the child is collected, which sets `reaped`; false
// if `timeout_us` (0 = CONSUMER_TIMEOUT_US) passed again -- the child is still
// running and still not killed, an uninterruptible sleeper cannot be, which is
// why the consumer is a child rather than a thread in the first place -- or if
// it turned out to be gone without a status to read, which leaves `reaped`
// clear and `hung` as it was. Runs its poll off the vcpu, so it is safe to call
// from a coroutine.
//
// The two `false` returns a cleanup path can get -- the timeout, and the "no
// result channel left" refusal -- leave different things behind, and that
// decides whether it retries: only the timeout keeps the channel mapped, so it
// is the only one worth calling again. A call that collected the child sets
// `reaped` and releases the channel before it returns, so a second call answers
// `true` from the `reaped` guard without reading anything -- it does not fail.
// That refusal is reached the other way round: a caller that gave up on the
// child, released the channel itself (see consumer_release), and then called
// this anyway.
bool consumer_reap(ConsumerIoResult& r, uint64_t timeout_us = 0);

// Unmap the result channel of a consumer that will not be reaped. Idempotent.
void consumer_release(ConsumerIoResult& r);

// Spawn the consumer child with EXACTLY this argv and report what came back
// through the same posix_spawn / bounded wait / consumer_decode path consumer_io()
// takes. `argv` is argv[1] onwards and must start with CONS_CHILD_ARG -- argv[0]
// (this binary) and the terminator are supplied here, and anything else is
// refused without spawning (see CONS_CHILD_ARG for what a child that does not see
// the sentinel turns out to be). The channel is the report page alone, which is
// all a child that never reaches its IO can touch. Whether one whose argv DOES
// decode stays inside it turns on its mode: a mode with a payload region sizes
// that region from its argv, so the child maps more than the one page handed
// here, and its first touch past the end of the backing file would kill it --
// the second reason such an argv must name a node that does not exist, failing
// its open before any payload. channel_io_bytes() returns 0 for two modes, whose
// children allocate their own IO buffers instead of taking a payload region from
// their argv: CONS_MODE_STRESS, each of whose workers posix_memaligns a pair, and
// CONS_MODE_WRITER, which posix_memaligns the block it hammers. For those the page
// handed here is all the child maps, so the reason above does not reach them -- but
// the two are not alike in what a decoded argv handed here then does. A stress
// child ends on its own, so a stress argv may name a real file and run to
// completion. A writer child loops until its parent stops it, so one whose argv
// decoded would spend the caller's whole deadline here and then leave the child
// running. Hence a row in the table of argvs that must not decode may be a writer's
// only in a form that is refused, and the writer's positive control is a
// BackgroundWriter, whose start() is the builder those rows pin.
//
// Test-only, and the reason it exists: the structural exit codes above are the
// loud half of the isolation, and the only way to hear them is to hand the child
// an argv that cannot decode. A case that spawned the child itself would witness
// libc's exit codes rather than this mapping of them -- and would say nothing
// about the path the suites actually take.
ConsumerIoResult consumer_spawn_argv(const std::vector<std::string>& argv,
                                     uint64_t timeout_us = 0);

// ---------------------------------------------------------------------------
// single-shot device IO (the suites' workhorse) and a deterministic pattern
// ---------------------------------------------------------------------------

std::vector<char> pattern(uint8_t seed, size_t n);

struct DeviceIoOpts {
    fs::IFile* backend = nullptr;   // also verify the range in the backend file
    bool read_only = false;         // skip the write+fsync, just read the range
    bool direct = false;            // O_DIRECT
    uint64_t timeout_us = 0;        // 0 = CONSUMER_TIMEOUT_US
    ConsumerIoResult* report = nullptr;   // optional: what the consumer child did
};

// Write + fsync + read back `len` bytes at `off` through the device node, from a
// spawned consumer child (see above) and entirely off the photon vcpu. Returns 0,
// an errno, EILSEQ when the data read back (or the backend's copy) does not
// match what was written, or ETIMEDOUT when the child had to be abandoned --
// which leaves it running. Ask for `report` to keep the handle on it (its pid
// and its result channel, for a later consumer_reap()); without one the channel
// is released here and the child is left to finish or not on its own.
int device_io(const std::string& node, const void* wbuf, size_t len,
              uint64_t off, const DeviceIoOpts& o = {});

// A read-only export: open(O_RDWR) on the node still SUCCEEDS (verified against
// a read-only loop device) and the RO enforcement happens at write time, so a
// write must fail. The open, the read that has to come back and the write that
// must not all run in a spawned consumer child (CONS_MODE_RO_WRITE), off the vcpu:
// an IO against an exported node has no completion bound, and a caller in this
// process would hold the descriptors of whatever is serving that node.
//
// Returns 0 when the write was rejected as expected, EILSEQ when it went through,
// EBADMSG when the child echoed an off or a len other than the one sent -- its
// verdict is then about an IO nobody asked for, so it is not handed back as one --
// an errno from the child's open or read, the structural ones included, or
// ETIMEDOUT when the child had to be abandoned, which leaves it running: without a
// `report` its channel is released here and nothing keeps its pid. Ask for `report`
// to keep the child's own account of it (see consumer_io above).
int expect_write_rejected(const std::string& node, uint64_t off, size_t len,
                          ConsumerIoResult* report = nullptr);

// ---------------------------------------------------------------------------
// block formats
//
// Every block the driver writes is SELF-DESCRIBING: the header carries the
// offset and length it was written at, so a reader can tell a misrouted IO
// (another slot's block), a torn IO (payload inconsistent with the header) and
// a lost IO (the slot's previous content) apart.
// ---------------------------------------------------------------------------

struct StressBlockHdr {
    uint64_t off;
    uint32_t len;
    uint32_t tid;
    uint32_t seq;
    uint32_t fill;
};

// DISJOINT mode: content owned by one thread, so a read-back must match the
// reader's own (tid, seq) exactly.
void stress_format(void* buf, size_t len, uint64_t off, uint32_t tid, uint32_t seq);

// 0 on success, errno on a structural problem, EILSEQ when the block does not
// validate. *tid/*seq report whose block it was (DISJOINT mode asserts they
// are the reader's own).
int stress_validate(const void* buf, size_t len, uint64_t off,
                    uint32_t* tid = nullptr, uint32_t* seq = nullptr);

// SHARED mode: content is a function of the slot ONLY, never of the writer, so
// concurrent writes to one slot are byte-identical and ANY interleaving of
// them is a valid outcome. Without that, reading back another writer's block
// (or a mix of two writers' blocks) is legitimate block-layer behavior and
// would be indistinguishable from corruption.
void stress_format_canonical(void* buf, size_t len, uint64_t off);

// the slot grid's pre-fill: content no canonical block can equal, so a lost
// FIRST write to a slot is still detectable
void stress_format_poison(void* buf, size_t len);

// classify a read-back that does not match what was written
std::string stress_diagnose(const void* got, size_t len, uint64_t off);

// ---------------------------------------------------------------------------
// the stress driver
// ---------------------------------------------------------------------------

enum class StressMode : uint8_t {
    DISJOINT,   // per-thread regions, mixed block sizes: a read-back must be
                // exactly what this thread wrote (strictest attribution)
    SHARED,     // every thread hammers one grid of fixed-size slots: maximal
                // contention, validated against the slot's canonical content
};

struct StressCfg {
    std::string node;
    uint64_t size = 0;                    // device size in bytes
    int threads = 32;                     // concurrent OS threads on the node
    int iters = 32;                       // write+read-back pairs per thread
    uint64_t base_off = 1ull << 20;       // 4K-aligned; keeps offset 0 out of
                                          // it (partition/superblock probes)
    uint64_t span = 0;                    // 0 = size - base_off
    size_t max_block = 256 << 10;         // DISJOINT: the largest of the mixed
                                          // sizes; SHARED: the fixed slot size
    StressMode mode = StressMode::DISJOINT;
    bool direct = true;                   // O_DIRECT: every IO really reaches
                                          // the daemon, not the page cache
    bool flush = false;                   // fsync every 8th iteration
    uint32_t seed = 0;                    // 0 = derive from the thread id
};

struct StressResult {
    uint64_t ios = 0;                     // completed write+read-back pairs
    uint64_t bytes = 0;                   // bytes written (== bytes read back)
    int failures = 0;                     // syscall + validation errors
    std::string first_error;
    uint64_t elapsed_us = 0;              // measured by stress_run(), around the
                                          // spawn of the child that ran the phase,
                                          // so it includes the spawn and the exec
    // The child that ran the phase. Surfaced because "the phase ran in a child
    // at all" is the property the isolation exists for, and counters alone
    // cannot witness it: an in-process phase reports the same numbers. -1 until
    // a spawn happened, so a phase that never got that far stays distinguishable.
    pid_t child_pid = -1;
    // Its post-drop census, the child's own, or CONS_FDS_UNMEASURED when the child
    // never wrote a final report to read one out of. Not 0 in that case: a phase
    // that was abandoned is exactly the one a reader most wants to ask about its
    // descriptors, and 0 would answer "clean" to a question nobody asked.
    uint32_t child_fds = CONS_FDS_UNMEASURED;

    explicit operator bool() const { return failures == 0 && ios > 0; }
    void report(const char* what, const StressCfg& c) const;
};

// xorshift64* -- the offsets must be reproducible from the seed, and
// std::mt19937 per thread costs more than the IO it schedules
struct StressRnd {
    uint64_t s;
    explicit StressRnd(uint64_t seed) : s(seed ? seed : 0x9e3779b97f4a7c15ull) {}
    uint64_t next() {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 2685821657736338717ull;
    }
    uint64_t below(uint64_t n) { return n ? next() % n : 0; }
};

// Runs the phase in a SPAWNED CHILD and returns that child's counters. One child
// per phase, not per IO: the property being protected is who holds the node, and
// every worker of a phase opens it, so a phase is the smallest unit that can be
// wedged on it -- and per-IO children would be tens of thousands of execs.
// Runs on the CALLING OS THREAD: it spawns that child and then polls for it with
// a deadline of its own. From a coroutine use stress_off_vcpu() below. `abandoned`,
// when given, receives this phase's result whether or not the child had to be given
// up on: a clean phase hands over the pid it spawned with `shm` null, and only a
// child the deadline gave up on comes with its result channel still mapped, for a
// later consumer_reap(). So `shm`, not `pid`, is what says there is a child still
// to reap. Without `abandoned` the channel is released there and the child is left
// to finish or not on its own.
StressResult stress_run(const StressCfg& c, ConsumerIoResult* abandoned = nullptr);

// Coroutine-safe wrapper: the spawn and the bounded wait both run on an OS thread
// while this vcpu keeps serving the device under test. The wait is a
// WNOHANG/nanosleep poll, so on the vcpu it would stall the very coroutines that
// have to serve the IO being waited on.
StressResult stress_off_vcpu(const StressCfg& c, ConsumerIoResult* abandoned = nullptr);

// Both phases the kernel-node suites run: DISJOINT at the default concurrency
// (fsync every 8th IO), then SHARED at `shared_threads` on one slot grid.
// Reports each phase, logs the first failure, and returns the total failure
// count so a suite can assert EXPECT_EQ(0, ...).
int stress_node_both_modes(const std::string& node, uint64_t size, const char* label,
                           int shared_threads = 16);

// ---------------------------------------------------------------------------
// the restart-window writer
// ---------------------------------------------------------------------------

// A continuous writer in a SPAWNED CHILD, for the tests that drop and re-start (or
// hand over) the daemon while IO is in flight. A child and not a thread of this
// process: a writer is the consumer most likely to end up wedged on the node, and a
// wedged thread here holds this process's descriptors and shares its address space,
// in which every transport has the device mapped MAP_SHARED -- so the recovery path,
// which begins by letting go of the control device, could never run. Measured once:
// 47 hours unkillable, device never recovered.
//
// stop() is BOUNDED and never signals the child: an uninterruptible sleeper cannot
// be killed, so a signal would only queue up beside the wedge while this process
// stays stuck. On expiry it gives up, says so loudly with the pid, and returns
// false -- so a writer that was abandoned is a failure a case can assert on and not
// something that looks exactly like a clean stop. That is also why the release of a
// gate the writer is parked behind still has to come first: with a deadline the
// wrong order no longer hangs, it just goes red, and red is the correct answer.
//
// Its counters are LIVE across the process boundary: iters() and wait_iters() read
// what the child has published so far while it is still running, which is what lets
// a case synchronize on it (see ConsumerWriterReport for why that is an atomic in a
// shared mapping and not a semaphore). Both of those, and stop()'s wait, need a
// photon runtime on the calling thread -- they yield the vcpu rather than block it,
// which is the point: the IO a writer is inside is served by this very process.
//
// Idempotent, so a case can stop it explicitly at the right point (before the
// device shutdown, which would otherwise EBUSY against the still-open node) and
// still leave a DEFER as the safety net. A second stop() answers true: a writer
// already stopped was not abandoned by the call that found it stopped. The
// destructor calls stop() too and nobody reads what it returns, so a case that
// cares whether its writer was collected has to call stop() itself and assert on it.
class BackgroundWriter {
public:
    // Spelled because the deleted copy below is itself a user-declared constructor,
    // and declaring one suppresses the implicit default.
    BackgroundWriter() = default;

    struct Opts {
        uint64_t off = 0;              // first (or, with advance=false, only) offset
        uint64_t wrap_at = 0;         // advance: wrap back to `off` once a block
                                      // would start past this; 0 = never wrap
        size_t block = 64 << 10;
        bool direct = true;
        bool advance = true;          // false = hammer one offset
        bool verify = false;          // read each block back and compare
    };

    int start(const std::string& node, const Opts& o);   // 0, or errno

    // Wait out the writer with a deadline of its own (0 = CONSUMER_TIMEOUT_US) and
    // collect it. true once it is collected; false when the deadline passed with it
    // still running -- it is then abandoned, not killed, and logged here with its
    // pid, because the destructor's call has nobody to tell. Ask for `abandoned` to
    // keep the handle on it (its pid and its result channel, for a later
    // consumer_reap()); without one the channel is released here and the child is
    // left to finish or not on its own. Idempotent.
    bool stop(ConsumerIoResult* abandoned = nullptr, uint64_t timeout_us = 0);
    ~BackgroundWriter() { stop(); }

    // The child's own IO failures, plus one from this side if what the child
    // decoded its argv into disagreed with what was sent to it -- the same folding
    // stress_in_child() does, and for the same reason: a writer that ran a
    // configuration nobody asked for is not a writer that ran cleanly.
    int errors() const;
    uint64_t iters() const;
    // wait until at least `n` iterations completed; false on timeout
    bool wait_iters(uint64_t n, uint64_t timeout_us = 30ull * 1000 * 1000);

private:
    ConsumerIoResult r;                 // the child: its pid, and the channel
    ConsumerWriterReport* w = nullptr;  // inside r.shm; null once that is released
    // What was sent, kept because what the child says it decoded is compared
    // against it in stop() -- which is after the argv it was built from is gone.
    Opts sent{};
    // What iters() and errors() answer with afterwards: both are read after stop()
    // returns, and by then the channel they came out of is gone.
    uint64_t final_iters = 0;
    int final_errors = 0;
    bool running = false;

    // It owns a mapping and a child: a copy would release the one twice and leave
    // two objects each believing the other's pid was theirs to reap.
    BackgroundWriter(const BackgroundWriter&) = delete;
    BackgroundWriter& operator=(const BackgroundWriter&) = delete;
};

}  // namespace test
}  // namespace blk
}  // namespace photon
