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
#include <cstddef>
#include <cstdint>
#include <string>
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
// single-shot device IO (the suites' workhorse) and a deterministic pattern
// ---------------------------------------------------------------------------

std::vector<char> pattern(uint8_t seed, size_t n);

struct DeviceIoOpts {
    fs::IFile* backend = nullptr;   // also verify the range in the backend file
    bool read_only = false;         // skip the write+fsync, just read the range
    bool direct = false;            // O_DIRECT
};

// Write + fsync + read back `len` bytes at `off` through the device node,
// entirely off the photon vcpu. Returns 0, an errno, or EILSEQ when the data
// read back (or the backend's copy) does not match what was written.
int device_io(const std::string& node, const void* wbuf, size_t len,
              uint64_t off, const DeviceIoOpts& o = {});

// A read-only export: open(O_RDWR) on the node still SUCCEEDS (verified against
// a read-only loop device) and the RO enforcement happens at write time, so a
// write must fail. Returns 0 when it was rejected as expected, EILSEQ when it
// went through, or an errno from the open. Runs off the vcpu.
int expect_write_rejected(const std::string& node, uint64_t off, size_t len);

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
    uint64_t elapsed_us = 0;

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

// Runs on the CALLING OS THREAD (it spawns and joins its own workers); from a
// coroutine use stress_off_vcpu() below.
StressResult stress_run(const StressCfg& c);

// Coroutine-safe wrapper: the driver runs entirely on OS threads while this
// vcpu keeps serving the device under test.
StressResult stress_off_vcpu(const StressCfg& c);

// Both phases the kernel-node suites run: DISJOINT at the default concurrency
// (fsync every 8th IO), then SHARED at `shared_threads` on one slot grid.
// Reports each phase, logs the first failure, and returns the total failure
// count so a suite can assert EXPECT_EQ(0, ...).
int stress_node_both_modes(const std::string& node, uint64_t size, const char* label,
                           int shared_threads = 16);

// ---------------------------------------------------------------------------
// the restart-window writer
// ---------------------------------------------------------------------------

// A continuous writer on its own OS thread, for the tests that drop and
// re-start (or hand over) the daemon while IO is in flight. It must NEVER be
// joined on the photon vcpu while it may hold IO: join blocks the whole vcpu,
// serving stops, and the in-flight IO then only returns through the kernel's
// timeout paths (tcmu's cmd_time_out/EH cycles are 30s quanta). stop() therefore
// polls a done flag coroutine-side and joins only once the thread is out of its
// IO -- and it is idempotent, so a test can stop it explicitly at the right
// point (before the device shutdown, which would otherwise EBUSY against the
// still-open node) and still leave a DEFER as the safety net.
class BackgroundWriter {
public:
    struct Opts {
        uint64_t off;                 // first (or, with advance=false, only) offset
        uint64_t wrap_at = 0;         // advance: wrap back to `off` once a block
                                      // would start past this; 0 = never wrap
        size_t block = 64 << 10;
        bool direct = true;
        bool advance = true;          // false = hammer one offset
        bool verify = false;          // read each block back and compare
    };

    int start(const std::string& node, const Opts& o);   // 0, or errno
    void stop();                                         // idempotent
    ~BackgroundWriter() { stop(); }

    int errors() const { return bad.load(); }
    uint64_t iters() const { return iter_count.load(); }
    // wait until at least `n` iterations completed; false on timeout
    bool wait_iters(uint64_t n, uint64_t timeout_us = 30ull * 1000 * 1000);

private:
    std::thread th;
    std::atomic<bool> stop_flag{false}, done{false};
    std::atomic<int> bad{0};
    std::atomic<uint64_t> iter_count{0};
    bool running = false;
};

}  // namespace test
}  // namespace blk
}  // namespace photon
