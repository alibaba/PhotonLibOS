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
