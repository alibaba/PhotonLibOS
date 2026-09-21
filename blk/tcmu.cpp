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

// TCMU transport (LIO target_core_user backstore). Exports a photon IFile as a
// kernel SCSI disk: start() creates the configfs backstore, enables it (the
// kernel registers a /dev/uioN command ring), mmaps and serves that ring with a
// photon coroutine per SCSI command, then attaches a tcm_loop LUN so a local
// /dev/sdX appears. detach() stops serving but keeps the registration + LUN so a
// later start() (possibly another process) takes over and harvests the backlog;
// shutdown() also removes the LUN and the registration.
//
// Self-contained: the kernel UAPI <linux/target_core_user.h> is C++-hostile
// (tcmu_hdr_get_op() does an invalid enum conversion and <linux/uio.h> clashes
// with glibc's struct iovec), so the ring ABI is hand-defined below and verified
// byte-identical to the kernel struct (sizeof/offsetof cross-checked). No
// libtcmu, no libnl3. Only the SCSI opcodes a Linux tcm_loop initiator sends are
// emulated. All serving coroutines run on the single vcpu that called start(),
// so the bookkeeping needs no cross-vcpu locking.

#include "blk.h"
#include "utils.h"

#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/estring.h>
#include <photon/common/iovector.h>
#include <photon/common/utility.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef RWF_DSYNC
#define RWF_DSYNC 0x00000002
#endif

namespace photon {
namespace blk {

// ----------------------------------------------------------------------------
// TCMU ring ABI (kernel <linux/target_core_user.h>, mailbox version 2),
// hand-defined: that header is a C header whose inline tcmu_hdr_get_op()
// converts an integer to an enum, which C++ rejects outright. Only the mailbox
// and the entry header are packed, as in the kernel; req/rsp/entry use NATURAL
// alignment, which is what places cdb_off at entry offset 24 and the iov array
// at 48 -- the static_asserts below pin every offset. glibc's struct iovec
// {void*,size_t} is layout-identical to the kernel's, so req's iov array feeds
// IFile::preadv/pwritev directly.
// ----------------------------------------------------------------------------

static constexpr uint16_t TCMU_MAILBOX_VERSION = 2;
static constexpr uint32_t TCMU_SENSE_BUFFERSIZE = 96;
static constexpr uint32_t TCMU_OP_MASK = 0x7;
enum : uint32_t { TCMU_OP_PAD = 0, TCMU_OP_CMD = 1, TCMU_OP_TMR = 2 };

struct tcmu_mailbox {
    uint16_t version;
    uint16_t flags;
    uint32_t cmdr_off;
    uint32_t cmdr_size;
    uint32_t cmd_head;
    uint32_t cmd_tail __attribute__((__aligned__(64)));  // own cacheline
} __attribute__((packed));
static_assert(sizeof(tcmu_mailbox) == 128, "tcmu mailbox layout");
static_assert(offsetof(tcmu_mailbox, cmd_tail) == 64, "tcmu cmd_tail offset");

// The two cursors are shared with the kernel and each has exactly one writer:
// cmd_head is published by the kernel with smp_store_release (its UPDATE_HEAD),
// cmd_tail by us. Pair them. A plain load of cmd_head can be reordered before
// the loads of the entry it announces -- the entry address comes from our own
// parse cursor, so there is no address dependency to stop it -- which is what
// broke tcmu-runner on aarch64 (issue #688: "cmd_id N not found, ring is
// broken" under fio QD=128); its libtcmu now uses this same acquire load. Our
// release on cmd_tail publishes the rsp fields written into the same shared
// entry just before it. The other mailbox fields are written once by the kernel
// before the device is exposed, so plain reads after mmap() need no ordering.
static inline uint32_t mb_head(tcmu_mailbox* mb) {
    return __atomic_load_n(&mb->cmd_head, __ATOMIC_ACQUIRE);
}
static inline uint32_t mb_tail(tcmu_mailbox* mb) {
    return __atomic_load_n(&mb->cmd_tail, __ATOMIC_ACQUIRE);
}
static inline void mb_set_tail(tcmu_mailbox* mb, uint32_t tail) {
    __atomic_store_n(&mb->cmd_tail, tail, __ATOMIC_RELEASE);
}

struct tcmu_cmd_entry_hdr {
    uint32_t len_op;     // low 3 bits = opcode, rest = entry length (8-aligned)
    uint16_t cmd_id;
    uint8_t  kflags;
    uint8_t  uflags;
} __attribute__((packed));
static_assert(sizeof(tcmu_cmd_entry_hdr) == 8, "tcmu entry header layout");

struct tcmu_cmd_req {    // natural alignment: cdb_off lands at req offset 16
    uint32_t iov_cnt;
    uint32_t iov_bidi_cnt;
    uint32_t iov_dif_cnt;
    uint64_t cdb_off;    // offset into the mmap of the CDB
    uint64_t __pad1;
    uint64_t __pad2;
    // the iov array of the request that follows inside the SAME ring entry;
    // iov_base holds an OFFSET into the mmap, not a pointer. The kernel uapi
    // spells it __DECLARE_FLEX_ARRAY(struct iovec, iov), which expands to
    // exactly this zero-size array under __cplusplus.
    struct iovec iov[0];
};
static_assert(sizeof(tcmu_cmd_req) == 40, "tcmu req layout");   // iov[0] adds nothing

struct tcmu_cmd_rsp {
    uint8_t  scsi_status;
    uint8_t  __pad1;
    uint16_t __pad2;
    uint32_t read_len;
    char     sense_buffer[TCMU_SENSE_BUFFERSIZE];
};
static_assert(sizeof(tcmu_cmd_rsp) == 104, "tcmu rsp layout");

// Not packed, unlike the kernel's: packing is layout-neutral here (hdr is 8 and
// the union is 104 with 8-byte alignment, so sizeof is 112 either way, pinned
// below), and leaving it off is what lets `e->req.iov` compile -- taking the
// address of a member of a packed struct trips -Waddress-of-packed-member, which
// is -Werror here. Safe because ring entries are 8-aligned by construction
// (cmdr_off is 128 and hdr.len is 8-aligned).
struct tcmu_cmd_entry {
    tcmu_cmd_entry_hdr hdr;
    union {
        tcmu_cmd_req req;
        tcmu_cmd_rsp rsp;
    };
};
static_assert(sizeof(tcmu_cmd_entry) == 112, "tcmu entry layout");
static_assert(offsetof(tcmu_cmd_entry, req) == 8, "tcmu req offset");
static_assert(offsetof(tcmu_cmd_entry, req.cdb_off) == 24, "tcmu cdb_off offset");
static_assert(offsetof(tcmu_cmd_entry, rsp.scsi_status) == 8, "tcmu status offset");
static_assert(offsetof(tcmu_cmd_entry, rsp.sense_buffer) == 16, "tcmu sense offset");
// the iov array sits right after req's fixed part, INSIDE hdr.len (48 + 16*n)
static_assert(offsetof(tcmu_cmd_entry, req) + sizeof(tcmu_cmd_req) == 48, "tcmu iov offset");

static inline struct iovec* tcmu_entry_iov(tcmu_cmd_entry* e) {
    return e->req.iov;
}

// ----------------------------------------------------------------------------
// SCSI constants (SAM status, sense keys, ASC/ASCQ, opcodes)
//
// Hand-defined and NOT cross-checkable the way the ring ABI above is. The only
// exported candidate, <scsi/scsi.h>, is SCSI-2 era: it has none of the SBC-3
// opcodes used below (READ_16 / WRITE_16 / UNMAP / REPORT_LUNS /
// SERVICE_ACTION_IN), and its CHECK_CONDITION is 0x01 where SAM says 0x02
// (0x01 is RECOVERED_ERROR). The kernel's correct SAM_STAT_* live in
// <scsi/scsi_proto.h>, which is not exported to /usr/include. So these are
// checked empirically instead -- the unknown_opcode case in test-tcmu.cpp puts
// SAM_CHECK_CONDITION and our own sense bytes through the real kernel and LIO
// and asserts what comes back on the wire.
// ----------------------------------------------------------------------------

static constexpr uint8_t SAM_GOOD            = 0x00;
static constexpr uint8_t SAM_CHECK_CONDITION = 0x02;

static constexpr uint8_t SK_NO_SENSE        = 0x0;
static constexpr uint8_t SK_MEDIUM_ERROR    = 0x3;
static constexpr uint8_t SK_ILLEGAL_REQUEST = 0x5;
static constexpr uint8_t SK_UNIT_ATTENTION  = 0x6;
static constexpr uint8_t SK_DATA_PROTECT    = 0x7;

static constexpr uint8_t ASC_INVALID_OPCODE   = 0x20;
static constexpr uint8_t ASC_INVALID_FIELD    = 0x24;
static constexpr uint8_t ASC_LBA_OUT_OF_RANGE = 0x21;
static constexpr uint8_t ASC_WRITE_PROTECTED  = 0x27;
static constexpr uint8_t ASC_CAPACITY_CHANGED = 0x2a;   // ASCQ 09: capacity data has changed
static constexpr uint8_t ASC_READ_ERROR       = 0x11;
static constexpr uint8_t ASC_WRITE_ERROR      = 0x0c;

static constexpr uint8_t OPC_TEST_UNIT_READY       = 0x00;
static constexpr uint8_t OPC_REQUEST_SENSE         = 0x03;
static constexpr uint8_t OPC_READ_6                = 0x08;
static constexpr uint8_t OPC_WRITE_6               = 0x0a;
static constexpr uint8_t OPC_INQUIRY               = 0x12;
static constexpr uint8_t OPC_MODE_SELECT_6         = 0x15;
static constexpr uint8_t OPC_MODE_SENSE_6          = 0x1a;
static constexpr uint8_t OPC_START_STOP_UNIT       = 0x1b;
static constexpr uint8_t OPC_PREVENT_ALLOW_REMOVAL = 0x1e;
static constexpr uint8_t OPC_READ_CAPACITY_10      = 0x25;
static constexpr uint8_t OPC_READ_10               = 0x28;
static constexpr uint8_t OPC_WRITE_10              = 0x2a;
static constexpr uint8_t OPC_SYNC_CACHE_10         = 0x35;
static constexpr uint8_t OPC_WRITE_SAME_10         = 0x41;
static constexpr uint8_t OPC_UNMAP                 = 0x42;
static constexpr uint8_t OPC_MODE_SELECT_10        = 0x55;
static constexpr uint8_t OPC_MODE_SENSE_10         = 0x5a;
static constexpr uint8_t OPC_READ_16               = 0x88;
static constexpr uint8_t OPC_WRITE_16              = 0x8a;
static constexpr uint8_t OPC_SYNC_CACHE_16         = 0x91;
static constexpr uint8_t OPC_WRITE_SAME_16         = 0x93;
static constexpr uint8_t OPC_SERVICE_ACTION_IN_16  = 0x9e;
static constexpr uint8_t OPC_REPORT_LUNS           = 0xa0;
static constexpr uint8_t OPC_READ_12               = 0xa8;
static constexpr uint8_t OPC_WRITE_12              = 0xaa;
static constexpr uint8_t SAI_READ_CAPACITY_16      = 0x10;

static constexpr uint32_t DEFAULT_QUEUE_DEPTH = 64;

// tcmu genetlink ABI names (uapi <linux/target_core_user.h>; stable literals,
// hand-defined because that header is C++-hostile -- same reason as the ring).
// The family and multicast-group names are not in that header at all, and are
// resolved against the real kernel on every suite run instead.
static const char TCMU_GENL_FAMILY[] = "TCM-USER";
static const char TCMU_MCGRP_CONFIG[] = "config";
enum : uint8_t {
    TCMU_CMD_ADDED_DEVICE          = 1,
    TCMU_CMD_REMOVED_DEVICE        = 2,
    TCMU_CMD_RECONFIG_DEVICE       = 3,
    TCMU_CMD_ADDED_DEVICE_DONE     = 4,
    TCMU_CMD_REMOVED_DEVICE_DONE   = 5,
    TCMU_CMD_RECONFIG_DEVICE_DONE  = 6,
    TCMU_CMD_SET_FEATURES          = 7,
};
enum : uint16_t {
    TCMU_ATTR_DEVICE               = 1,
    TCMU_ATTR_MINOR                = 2,
    TCMU_ATTR_DEV_CFG              = 4,
    TCMU_ATTR_DEV_SIZE             = 5,
    TCMU_ATTR_CMD_STATUS           = 7,
    TCMU_ATTR_DEVICE_ID            = 8,
    TCMU_ATTR_SUPP_KERN_CMD_REPLY  = 9,
};

// ----------------------------------------------------------------------------
// big-endian CDB field access (SCSI is big-endian on the wire; convert with
// the __builtin_bswap family -- little-endian hosts only, same as nbd.cpp)
// ----------------------------------------------------------------------------

static inline uint16_t cdb_be16(const uint8_t* p) { return __builtin_bswap16(*(const uint16_t*)p); }
static inline uint32_t cdb_be32(const uint8_t* p) { return __builtin_bswap32(*(const uint32_t*)p); }
static inline uint64_t cdb_be64(const uint8_t* p) { return __builtin_bswap64(*(const uint64_t*)p); }
static inline void put_be16(uint8_t* p, uint16_t v) { *(uint16_t*)p = __builtin_bswap16(v); }
static inline void put_be32(uint8_t* p, uint32_t v) { *(uint32_t*)p = __builtin_bswap32(v); }
static inline void put_be64(uint8_t* p, uint64_t v) { *(uint64_t*)p = __builtin_bswap64(v); }

// ----------------------------------------------------------------------------
// configfs / sysfs helpers
// ----------------------------------------------------------------------------

static int cfg_write(const char* path, const char* val) {
    int fd = ::open(path, O_WRONLY);
    if (fd < 0)
        LOG_ERRNO_RETURN(0, -1, "failed to open configfs ` for write", path);
    size_t len = strlen(val);
    ssize_t n = ::write(fd, val, len);
    ::close(fd);
    if (n != (ssize_t)len)
        LOG_ERROR_RETURN(EIO, -1, "short write to configfs `, wrote ` of `", path, n, len);
    return 0;
}
// Best-effort configfs write for teardown paths that tolerate failure (the
// caller ignores the result), so it logs at DEBUG rather than ERROR. Used to
// disable a backstore the kernel may have already torn down; the subsequent
// rmdir is the real cleanup.
static void cfg_write_best(const char* path, const char* val) {
    int fd = ::open(path, O_WRONLY);
    if (fd < 0) return;
    size_t len = strlen(val);
    ssize_t n = ::write(fd, val, len);
    ::close(fd);
    if (n != (ssize_t)len)
        LOG_DEBUG("best-effort configfs write to ` wrote ` of ` (tolerated)", path, n, len);
}
static int cfg_write_u64(const char* path, uint64_t v) {
    char b[24];
    snprintf(b, sizeof(b), "%llu", (unsigned long long)v);
    return cfg_write(path, b);
}

static int cfg_read(const char* path, char* buf, size_t n) {
    int fd = ::open(path, O_RDONLY);
    if (fd < 0)
        LOG_ERRNO_RETURN(0, -1, "failed to open ` for read", path);
    ssize_t r = ::read(fd, buf, n - 1);
    ::close(fd);
    if (r < 0)
        LOG_ERRNO_RETURN(0, -1, "failed to read `", path);
    if ((size_t)r >= n)
        LOG_ERROR_RETURN(ENOBUFS, -1, "no enough buffer");
    // strip a trailing newline
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == '\r')) --r;
    buf[r] = '\0';
    return (int)r;
}
// reads an integer attribute (base auto: handles the hex map0/size and decimal attribs)
static uint64_t cfg_read_u64(const char* path, uint64_t dflt) {
    char b[64];
    if (cfg_read(path, b, sizeof(b)) < 0) return dflt;
    return strtoull(b, nullptr, 0);
}
static int cfg_mkdir(const char* path) {
    if (::mkdir(path, 0755) == 0 || errno == EEXIST) return 0;
    LOG_ERRNO_RETURN(0, -1, "failed to mkdir `", path);
}
static bool path_exists(const char* path) { return ::access(path, F_OK) == 0; }

// ----------------------------------------------------------------------------
// identity helpers: a stable hash drives the tcm_loop WWN and the SCSI device id,
// both derived deterministically from cfg.info.identity so they survive restarts
// ----------------------------------------------------------------------------

static uint64_t fnv1a64(const char* s) {
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ull; }
    return h;
}
// configfs names and the lock file allow only a conservative charset
static void sanitize(char* out, size_t cap, const char* s) {
    size_t n = 0;
    for (; n + 1 < cap && n < 64 && s[n]; n++) {
        char c = s[n];
        out[n] = (isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-') ? c : '_';
    }
    if (n == 0 && cap > 4) {   // nothing usable: fall back to a fixed name
        memcpy(out, "dev", 3);
        n = 3;
    }
    if (cap) out[n] = '\0';
}
// tcm_loop WWN: "naa." + 16 hex digits, the leading 5 = NAA registered IEEE
static void derive_wwn(char* out, size_t cap, const char* identity) {
    uint64_t h = fnv1a64(identity);
    snprintf(out, cap, "naa.5%015llx", (unsigned long long)(h & 0x0fffffffffffffffull));
}

// ----------------------------------------------------------------------------
// TcmuUio: owns the /dev/uioN fd, the mmap, the mailbox, the command-ring
// cursors and the completion bitmap. No SCSI knowledge -- pure ring mechanics.
// ----------------------------------------------------------------------------

struct TcmuUio {
    int fd = -1;
    void* map = nullptr;          // mmap base == mailbox base
    size_t map_size = 0;
    tcmu_mailbox* mb = nullptr;
    char* ring = nullptr;         // map + cmdr_off
    uint32_t cmdr_size = 0;
    uint32_t parse_pos = 0;       // pump's dispatch cursor (runs ahead of cmd_tail)
    uint8_t* done = nullptr;      // per-8-byte-slot completion bitmap, cmdr_size/8

    ~TcmuUio() { close(); }

    // find the /dev/uioN whose sysfs name correlates to backstore <bs_name>
    // under hba number <hbanum>. The uio name is "tcm-user/<hbanum>/<bs>/<cfg>".
    static int find(const char* hbanum, const char* bs_name, char* node, size_t n) {
        DIR* d = opendir("/sys/class/uio");
        if (!d)
            LOG_ERRNO_RETURN(0, -1, "failed to open /sys/class/uio");
        DEFER(closedir(d));
        struct dirent* e;
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, "uio", 3) != 0) continue;
            char buf[512];
            snprintf(buf, sizeof(buf), "/sys/class/uio/%s/name", e->d_name);
            // one buffer serves both: open() consumes the path before read()
            // overwrites buf with the uio name (node is too small for this:
            // callers give 64 bytes, the name can reach ~330)
            if (cfg_read(buf, buf, sizeof(buf)) < 0) continue;
            // buf = "tcm-user/<hbanum>/<bs_name>/<dev_config...>"; only the
            // first three fields matter (dev_config may itself contain '/').
            // `fields` must be named: the iterator points back into it
            auto fields = estring_view(buf).split("/");
            auto it = fields.begin(), end = fields.end();
            if (it == end || *it != "tcm-user") continue;
            if (++it == end || *it != hbanum) continue;
            if (++it == end || *it != bs_name) continue;
            snprintf(node, n, "/dev/%s", e->d_name);
            return 0;
        }
        LOG_ERROR_RETURN(ENOENT, -1, "no uio device for tcmu backstore ` (hba `)", bs_name, hbanum);
    }

    int open(const char* devnode) {
        char sp[256];
        const char* uioname = devnode + 5;  // skip "/dev/"
        // the two map0 attributes share the directory path; write the prefix
        // once, then swap only the leaf
        int plen = snprintf(sp, sizeof(sp), "/sys/class/uio/%s/maps/map0/size", uioname);
        if (plen < 0 || (size_t)plen + 2 >= sizeof(sp))
            LOG_ERROR_RETURN(EINVAL, -1, "name too long");
        uint64_t sz = cfg_read_u64(sp, 0);
        strcpy(sp + plen - 4, "offset");    // trailing "size" --> "offset"
        uint64_t off = cfg_read_u64(sp, 0);
        if (!sz)
            LOG_ERROR_RETURN(EINVAL, -1, "uio map0 size is zero for ", devnode);

        fd = ::open(devnode, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to open ", devnode);
        map = ::mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
        if (map == MAP_FAILED) {
            map = nullptr;
            LOG_ERRNO_RETURN(0, -1, "failed to mmap uio ", devnode);
        }
        map_size = sz;
        mb = (tcmu_mailbox*)map;
        // Version 2 is what every kernel we target publishes: upstream since v4.1,
        // and RHEL/CentOS 7.3+ ship it too (with CAP_OOOC). The ABI-v1 mailbox of
        // upstream v4.0 lays its fields out differently, so this is what turns an
        // unsupported kernel into EINVAL instead of a wild cmdr_off.
        if (mb->version != TCMU_MAILBOX_VERSION)
            LOG_ERROR_RETURN(EINVAL, -1, "tcmu mailbox version `, expected `",
                             (int)mb->version, (int)TCMU_MAILBOX_VERSION);
        cmdr_size = mb->cmdr_size;
        if (!cmdr_size || (uint64_t)mb->cmdr_off + cmdr_size > sz)
            LOG_ERROR_RETURN(EINVAL, -1, "tcmu cmd ring out of range, ", VALUE(cmdr_size));
        ring = (char*)map + mb->cmdr_off;
        done = (uint8_t*)calloc(cmdr_size / 8, 1);
        if (!done)
            LOG_ERROR_RETURN(ENOMEM, -1, "failed to allocate tcmu completion bitmap");
        // resume from the kernel's consumer position: on a fresh device this is
        // 0; on attach it harvests the backlog a previous process left behind
        parse_pos = mb_tail(mb);
        uint32_t h = mb_head(mb), t = parse_pos;   // locals: VALUE() cannot bind a packed field
        LOG_INFO("tcmu ring resume point: ", VALUE(h), VALUE(t));
        return 0;
    }

    void close() {
        if (done) { free(done); done = nullptr; }
        if (map) { ::munmap(map, map_size); map = nullptr; mb = nullptr; ring = nullptr; }
        if (fd >= 0) { ::close(fd); fd = -1; }
        cmdr_size = 0;
        parse_pos = 0;
    }

    bool has_work() { return parse_pos != mb_head(mb); }

    // clear the uio interrupt (non-blocking; EAGAIN when none is pending)
    void clear_interrupt() {
        int32_t b;
        ssize_t r = ::read(fd, &b, sizeof(b));
        (void)r;
    }
    // notify the kernel that cmd_tail advanced (process completions)
    void ring_doorbell() {
        int32_t b = 0;
        ssize_t r = ::write(fd, &b, sizeof(b));
        (void)r;
    }

    // advance cmd_tail over the contiguous run of completed entries in
    // [cmd_tail, parse_pos). The kernel only ever sees in-order completions, so
    // out-of-order coroutine completion needs no CAP_OOOC. Returns true if moved.
    bool advance_tail() {
        bool moved = false;
        // we are cmd_tail's only writer, so track it locally and publish each
        // step with a release store: the rsp fields of that entry must reach the
        // kernel before the cursor that exposes them
        uint32_t tail = mb_tail(mb);
        while (tail != parse_pos && done[tail >> 3]) {
            auto ent = (tcmu_cmd_entry*)(ring + tail);
            uint32_t len = ent->hdr.len_op & ~TCMU_OP_MASK;
            if (len == 0) {
                LOG_ERROR("corrupt tcmu entry length 0 at offset `", tail);
                break;
            }
            done[tail >> 3] = false;  // reset for the next ring cycle
            tail = (tail + len) % cmdr_size;
            mb_set_tail(mb, tail);
            moved = true;
        }
        return moved;
    }
};

// ----------------------------------------------------------------------------
// TcmuServer: serves one device's ring. Owns the backend geometry, the pump
// and the SCSI emulation. Coroutine-per-command via plain thread_create;
// dispatch depth is bounded by a counting semaphore (queue_depth).
// ----------------------------------------------------------------------------

struct TcmuServer {
    // Field order is padding-driven, do not tidy it: the 8-wide members and the
    // 256-byte identity pack from offset 0, and every narrower one (vcpu_state,
    // the four uint32_t counters, the bools, PollPolicy) trails them.
    // Interleaving them left 29 bytes of holes -- block_size after identity,
    // pending_capacity_ua and read_only before features, poll before spin_us,
    // stopping before pump_th, dedicated before vcpu_thread, the two stop flags
    // before the handshake semaphores. 496 bytes vs 520.
    TcmuUio uio;
    fs::IFile* backend = nullptr;
    char identity[256] = {};    // capped well below this at registration time
    std::atomic<uint64_t> num_lbas{0};   // atomics: resize() can run on another
    std::atomic<uint64_t> dev_size{0};   // thread while the pump serves (dedicated vcpu)
    uint64_t features = 0;      // FEATURE_* the device advertises (gates LBPME/VPD 0xB0)
    uint64_t spin_us = 0;
    photon::thread* pump_th = nullptr;
    // One slot per in-flight command. photon::semaphore has no reset, but every
    // serve_stop() drains it back to full (each handle_cmd returns its token in
    // a DEFER, and stop waits for in_flight == 0), so it is seeded once at the
    // first start and reused across restarts -- the nbd.cpp idiom. Handlers
    // signal on completion (single vcpu: the in_flight-- and the signal execute
    // without an intervening schedule point).
    photon::semaphore slots;
    // the serving vcpu's two handshakes, one signal each per start(): it
    // publishes vcpu_state then signals started, and signals exited as its very
    // last act -- after vcpu_fini, from a plain std thread, which
    // semaphore::signal explicitly supports
    photon::semaphore vcpu_started{0}, vcpu_exited{0};
    // dedicated-vcpu serving (BlkConfig::vcpus >= 2): the pump and command
    // coroutines run on an owned vcpu (a std::thread) instead of the caller's.
    // tcmu has exactly one ring per device, so a single serving vcpu is all
    // that can help.
    bool dedicated = false;
    std::atomic<bool> stop_req{false}, stop_flush{false};
    std::thread vcpu_thread;
    std::atomic<int> vcpu_state{0};              // 0 starting, 1 serving, <0 = -errno
    uint32_t block_size = 512;
    uint32_t in_flight = 0;
    // stack for the per-command coroutines: the device resolves
    // BlkConfig::stack_size into this before start(), which keeps it out of the
    // start()/serve_start()/vcpu_main() parameter chain
    uint32_t stack_size = DEFAULT_REQ_STACK;
    uint32_t pending_wakeups = 0;   // doorbell coalescing counter
    std::atomic<bool> pending_capacity_ua{false};   // one-shot UNIT ATTENTION after resize
    bool read_only = false;
    PollPolicy poll = PollPolicy::SLEEP;
    bool stopping = false;

    struct CmdArg { TcmuServer* srv; tcmu_cmd_entry* ent; };
    static void* trampoline(void* a) {
        auto arg = (CmdArg*)a;
        arg->srv->handle_cmd(arg->ent);
        delete arg;
        return nullptr;
    }

    ~TcmuServer() { stop(false); }

    // `stack` lands in the stack_size member rather than being threaded through
    // serve_start()/vcpu_main() as well; it has no default so that every caller
    // has to say what the per-command coroutines get.
    int start(const char* devnode, uint32_t queue_depth, uint32_t vcpus, uint32_t stack) {
        stack_size = stack;
        if (vcpus < 2)
            return serve_start(devnode, queue_depth);
        dedicated = true;
        vcpu_state = 0;
        stop_req = false;
        vcpu_thread = std::thread([=, this] { vcpu_main(devnode, queue_depth); });
        // photon-side wait: never join a std::thread directly on the vcpu
        vcpu_started.wait(1);
        int st = vcpu_state.load();
        if (st < 0) {
            join_vcpu();
            dedicated = false;
            LOG_ERROR_RETURN(-st, -1, "failed to start the tcmu serving vcpu");
        }
        return 0;
    }

    void stop(bool flush) {
        if (dedicated) {
            stop_flush = flush;
            stop_req = true;
            join_vcpu();
            dedicated = false;
            return;
        }
        serve_stop(flush);
    }

    void join_vcpu() {
        vcpu_exited.wait(1);   // already exited: the join itself is instant
        vcpu_thread.join();
    }

    // the serving vcpu's whole lifetime when dedicated. devnode is borrowed:
    // start() waits for the publish below, which follows serve_start() consuming it
    void vcpu_main(const char* devnode, uint32_t queue_depth) {
        DEFER(vcpu_exited.signal(1));   // registered first, so it runs last
        photon::vcpu_init();
        DEFER(photon::vcpu_fini());
        if (photon::fd_events_init(photon::INIT_EVENT_EPOLL) < 0) {
            int e = errno ? errno : EIO;
            LOG_ERROR("failed to init the serving vcpu's event engine");
            return publish(-e);
        }
        DEFER(photon::fd_events_fini());
        if (serve_start(devnode, queue_depth) < 0) {
            int e = errno ? errno : EIO;
            return publish(-e);
        }
        publish(1);
        while (!stop_req.load(std::memory_order_acquire))
            photon::thread_usleep(1000);
        serve_stop(stop_flush);
    }

    // publish the outcome and wake start(): one signal per start() attempt
    void publish(int st) {
        vcpu_state = st;
        vcpu_started.signal(1);
    }

    // open the uio, seed the dispatch-depth semaphore, spawn the pump -- on
    // the serving vcpu
    int serve_start(const char* devnode, uint32_t queue_depth) {
        if (uio.open(devnode) < 0)
            return -1;
        if (slots.count() == 0)   // seed once; a restart finds it drained full
            slots.signal(queue_depth ? queue_depth : DEFAULT_QUEUE_DEPTH);
        stopping = false;
        in_flight = 0;
        pending_wakeups = 0;
        pump_th = photon::thread_create11(&TcmuServer::pump, this);
        photon::thread_enable_join(pump_th);
        return 0;
    }

    // flush=true: consume the un-dispatched ring backlog before stopping (an
    // orderly handover); either way the already-dispatched commands are drained,
    // since their coroutines write into the mmap and must finish before munmap
    void serve_stop(bool flush) {
        stopping = true;
        if (pump_th) {
            photon::thread_interrupt(pump_th);
            photon::thread_join((photon::join_handle*)pump_th);
            pump_th = nullptr;
        }
        if (flush && uio.mb)        // a never-opened uio has no ring to drain
            drain_ring();           // dispatch whatever the kernel queued
        while (in_flight)
            photon::thread_usleep(1000);
        uio.close();
    }

    // the serving loop: wait for the uio interrupt per the poll policy, then
    // dispatch every pending ring command to a fresh coroutine
    void pump() {
        uint64_t last_work = photon::now;
        while (!stopping) {
            if (uio.has_work()) {
                last_work = photon::now;
                uio.clear_interrupt();
                drain_ring();
                continue;
            }
            if (poll == PollPolicy::SPIN ||
                (poll == PollPolicy::ADAPTIVE && photon::now - last_work < spin_us)) {
                photon::thread_yield();
                continue;
            }
            // SLEEP (or ADAPTIVE that cooled down): block on the uio interrupt
            if (photon::wait_for_fd_readable(uio.fd) < 0) {
                if (stopping) break;
                if (errno != EINTR) {
                    LOG_WARN("tcmu pump wait failed, ", ERRNO());
                    photon::thread_usleep(1000);
                }
            }
        }
    }

    // Dispatch every command in [parse_pos, cmd_head) to a fresh coroutine. The
    // pump gates this with its own while(!stopping) and stop() joins the pump
    // before the flush call, so drain_ring itself must run to completion: the
    // orderly-handover flush (detach/shutdown with wait_pending) relies on it to
    // consume the un-dispatched backlog. A stopping guard here would make that
    // flush a no-op and collapse wait_pending=true into wait_pending=false.
    void drain_ring() {
        uint32_t head = mb_head(uio.mb);  // acquire: the entry loads below are addressed from parse_pos, not from head
        bool marked = false;
        while (uio.parse_pos != head) {
            auto ent = (tcmu_cmd_entry*)(uio.ring + uio.parse_pos);
            uint32_t len = ent->hdr.len_op & ~TCMU_OP_MASK;
            if (len == 0) {
                LOG_ERROR("corrupt tcmu entry length 0 at offset `, stopping drain", uio.parse_pos);
                break;
            }
            uint32_t op = ent->hdr.len_op & TCMU_OP_MASK;
            // take a dispatch slot BEFORE advancing parse_pos: an interruption
            // (stop() interrupts the pump) must leave the entry queued, with
            // parse_pos still pointing at it, for the flush drain to pick up
            if (op == TCMU_OP_CMD && slots.wait_interruptible(1) < 0)
                break;
            uint32_t off = uio.parse_pos;
            uio.parse_pos = (uio.parse_pos + len) % uio.cmdr_size;
            if (op == TCMU_OP_CMD) {
                // in-flight until handle_cmd completes -- set BEFORE the create
                // and left set if the create fails, so advance_tail cannot
                // reclaim the space of an entry nobody answered
                uio.done[off >> 3] = false;
                // handle_cmd's DEFER is the only thing that decrements in_flight
                // and returns the dispatch slot, so commit in_flight only once
                // the create succeeded: a nullptr return (photon's stack
                // allocation failed) would wedge serve_stop's `while (in_flight)`
                // forever and lose the slot. The entry then stays pending for
                // the kernel's cmd_time_out, which beats completing it with no
                // response written.
                auto* arg = new CmdArg{this, ent};
                if (photon::thread_create(&TcmuServer::trampoline, arg, stack_size)) {
                    in_flight++;
                } else {
                    delete arg;
                    slots.signal(1);
                    LOG_ERROR("tcmu: cannot create the command coroutine at ring offset `, leaving it pending", off);
                }
            } else {
                // PAD needs no response; TMR cannot appear (tmr_notification=0).
                // Mark done so advance_tail reclaims the ring space.
                uio.done[off >> 3] = true;
                marked = true;
            }
        }
        if (marked && uio.advance_tail())
            coalesced_doorbell();
    }

    void handle_cmd(tcmu_cmd_entry* ent) {
        DEFER({ in_flight--; slots.signal(1); });
        uint8_t* cdb = (uint8_t*)((char*)uio.map + ent->req.cdb_off);
        uint32_t iov_cnt = ent->req.iov_cnt;
        struct iovec* iov = tcmu_entry_iov(ent);
        // the ring stores iov_base as an OFFSET into the mmap; convert in place
        // (req and rsp share a union, so this must precede writing the response)
        for (uint32_t i = 0; i < iov_cnt; i++)
            iov[i].iov_base = (char*)uio.map + (uintptr_t)iov[i].iov_base;
        size_t data_len = iovector_view(iov, iov_cnt).sum();

        emulate(cdb, iov, iov_cnt, data_len, ent);

        uint32_t off = (char*)ent - uio.ring;
        uio.done[off >> 3] = true;
        if (uio.advance_tail())
            coalesced_doorbell();
    }

    // overlaybd's aio_pending_wakeups idiom: the first completer in a batch owns
    // the doorbell, rings it, yields, and re-rings if more completed meanwhile,
    // so N completions cost 1-2 uio writes instead of N
    void coalesced_doorbell() {
        bool wake = (++pending_wakeups == 1);
        while (wake) {
            uio.ring_doorbell();
            photon::thread_yield();
            if (pending_wakeups > 1) { pending_wakeups = 1; wake = true; }
            else                     { pending_wakeups = 0; wake = false; }
        }
    }

    // ----- response writers -----

    void set_status(tcmu_cmd_entry* ent, uint8_t st) { ent->rsp.scsi_status = st; }
    void set_sense(tcmu_cmd_entry* ent, uint8_t key, uint8_t asc, uint8_t ascq) {
        ent->rsp.scsi_status = SAM_CHECK_CONDITION;
        auto s = (uint8_t*)ent->rsp.sense_buffer;
        memset(s, 0, 18);
        s[0] = 0x70;          // current, fixed format
        s[2] = key & 0x0f;
        s[7] = 10;            // additional sense length (18 - 8)
        s[12] = asc;
        s[13] = ascq;
    }
    void unknown_op(tcmu_cmd_entry* ent) {
        // Unrecognized SCSI opcode -> CHECK CONDITION with ILLEGAL_REQUEST sense.
        // Do NOT set TCMU_UFLAG_UNKNOWN_OP: we fill the sense buffer ourselves,
        // which the normal completion path copies back to the initiator, so the
        // reply does not depend on what the kernel does with that ring-level flag.
        set_sense(ent, SK_ILLEGAL_REQUEST, ASC_INVALID_OPCODE, 0);
    }

    // ----- the SCSI opcode switch -----

    void emulate(uint8_t* cdb, struct iovec* iov, uint32_t iov_cnt, size_t data_len,
                 tcmu_cmd_entry* ent) {
        photon::thread_yield();  // don't hog the pump (overlaybd idiom)
        // one-shot UNIT ATTENTION "capacity data has changed" after resize();
        // the initiator's retry path re-reads the device (it issues READ
        // CAPACITY again). INQUIRY and REQUEST SENSE must complete normally
        // while a UA is pending (SAM UA-reporting rules).
        if (cdb[0] != OPC_INQUIRY && cdb[0] != OPC_REQUEST_SENSE &&
            pending_capacity_ua.exchange(false)) {
            set_sense(ent, SK_UNIT_ATTENTION, ASC_CAPACITY_CHANGED, 0x09);
            return;
        }
        switch (cdb[0]) {
        case OPC_INQUIRY:               emul_inquiry(cdb, iov, iov_cnt, ent); return;
        case OPC_TEST_UNIT_READY:       set_status(ent, SAM_GOOD); return;
        case OPC_SERVICE_ACTION_IN_16:
            if ((cdb[1] & 0x1f) == SAI_READ_CAPACITY_16) emul_read_capacity(iov, iov_cnt, ent, true);
            else unknown_op(ent);
            return;
        case OPC_READ_CAPACITY_10:      emul_read_capacity(iov, iov_cnt, ent, false); return;
        case OPC_MODE_SENSE_6:          emul_mode_sense(cdb, iov, iov_cnt, ent, false); return;
        case OPC_MODE_SENSE_10:         emul_mode_sense(cdb, iov, iov_cnt, ent, true); return;
        case OPC_MODE_SELECT_6:
        case OPC_MODE_SELECT_10:        set_status(ent, SAM_GOOD); return;  // no-op accept
        case OPC_READ_6: case OPC_READ_10: case OPC_READ_12: case OPC_READ_16:
            emul_read(cdb, iov, iov_cnt, data_len, ent); return;
        case OPC_WRITE_6: case OPC_WRITE_10: case OPC_WRITE_12: case OPC_WRITE_16:
            emul_write(cdb, iov, iov_cnt, data_len, ent); return;
        case OPC_SYNC_CACHE_10: case OPC_SYNC_CACHE_16: emul_sync(ent); return;
        case OPC_UNMAP:                 emul_unmap(iov, iov_cnt, data_len, ent); return;
        case OPC_WRITE_SAME_10:
        case OPC_WRITE_SAME_16:         emul_write_same(cdb, iov, iov_cnt, data_len, ent); return;
        case OPC_START_STOP_UNIT:
        case OPC_PREVENT_ALLOW_REMOVAL: set_status(ent, SAM_GOOD); return;  // no-op
        case OPC_REQUEST_SENSE:         emul_request_sense(iov, iov_cnt, ent); return;
        case OPC_REPORT_LUNS:           emul_report_luns(iov, iov_cnt, ent); return;
        default:                        unknown_op(ent); return;
        }
    }

    // LBA -> byte offset for the read/write/write-same families
    uint64_t cdb_byte_off(uint8_t op, const uint8_t* cdb) {
        uint64_t lba;
        switch (op) {
        case OPC_READ_6: case OPC_WRITE_6:
            lba = ((uint64_t)(cdb[1] & 0x1f) << 16) | ((uint64_t)cdb[2] << 8) | cdb[3];
            break;
        case OPC_READ_16: case OPC_WRITE_16: case OPC_WRITE_SAME_16:
            lba = cdb_be64(cdb + 2);
            break;
        default:  // 10/12-byte forms: 32-bit LBA at cdb[2..5]
            lba = cdb_be32(cdb + 2);
            break;
        }
        return blocks_to_bytes(lba);
    }
    // Multiply by the sector size WITHOUT wrapping. A 16-byte CDB carries a full
    // 64-bit LBA and the UNMAP parameter list is initiator data as well, so the
    // product can overflow -- and a wrapped offset sails through out_of_bounds()
    // and gets served at a bogus place (lba = 1<<55 with a 512-byte sector lands
    // on 0). Saturating to UINT64_MAX is safe: it is never a legitimate product
    // here and out_of_bounds() always rejects it.
    uint64_t blocks_to_bytes(uint64_t n) {
        return n > UINT64_MAX / block_size ? UINT64_MAX : n * block_size;
    }
    bool out_of_bounds(uint64_t off, uint64_t len) {
        return off > dev_size || len > dev_size - off;
    }

    void emul_read(uint8_t* cdb, struct iovec* iov, uint32_t iov_cnt, size_t data_len,
                   tcmu_cmd_entry* ent) {
        if (data_len == 0) { set_status(ent, SAM_GOOD); return; }
        uint64_t off = cdb_byte_off(cdb[0], cdb);
        if (out_of_bounds(off, data_len)) {
            set_sense(ent, SK_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
            return;
        }
        ssize_t r = backend->preadv(iov, iov_cnt, off);
        if (r == (ssize_t)data_len) set_status(ent, SAM_GOOD);
        else                        set_sense(ent, SK_MEDIUM_ERROR, ASC_READ_ERROR, 0);
    }

    void emul_write(uint8_t* cdb, struct iovec* iov, uint32_t iov_cnt, size_t data_len,
                    tcmu_cmd_entry* ent) {
        if (read_only) { set_sense(ent, SK_DATA_PROTECT, ASC_WRITE_PROTECTED, 0); return; }
        if (data_len == 0) { set_status(ent, SAM_GOOD); return; }
        uint8_t op = cdb[0];
        uint64_t off = cdb_byte_off(op, cdb);
        if (out_of_bounds(off, data_len)) {
            set_sense(ent, SK_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
            return;
        }
        bool fua = (op != OPC_WRITE_6) && (cdb[1] & 0x08);
        ssize_t w = fua ? backend->pwritev2(iov, iov_cnt, off, RWF_DSYNC)
                        : backend->pwritev(iov, iov_cnt, off);
        if (w == (ssize_t)data_len)   set_status(ent, SAM_GOOD);
        else if (errno == EROFS)      set_sense(ent, SK_DATA_PROTECT, ASC_WRITE_PROTECTED, 0);
        else                          set_sense(ent, SK_MEDIUM_ERROR, ASC_WRITE_ERROR, 0);
    }

    void emul_sync(tcmu_cmd_entry* ent) {
        if (backend->fdatasync() == 0) set_status(ent, SAM_GOOD);
        else                           set_sense(ent, SK_MEDIUM_ERROR, ASC_WRITE_ERROR, 0);
    }

    void emul_inquiry(uint8_t* cdb, struct iovec* iov, uint32_t iov_cnt, tcmu_cmd_entry* ent) {
        bool evpd = cdb[1] & 0x01;
        uint8_t page = cdb[2];
        uint16_t alloc = cdb_be16(cdb + 3);
        uint8_t buf[256];
        int n;
        // the kernel gathers data_length bytes (the full iov) from a REUSED,
        // non-zeroed ring data area, so any byte a metadata handler does not
        // explicitly write would be stale garbage from a previous command
        iovector_view(iov, iov_cnt).memset(0);
        if (!evpd) {
            memset(buf, 0, 36);
            buf[0] = 0x00;   // peripheral qualifier 0, device type 0 (disk)
            buf[2] = 0x05;   // version: SPC-3
            buf[3] = 0x02;   // response data format: SPC-3
            buf[4] = 31;     // additional length (36 - 5)
            memcpy(buf + 8,  "PHOTON  ", 8);
            memcpy(buf + 16, "PHOTON BLK      ", 16);
            memcpy(buf + 32, "0001", 4);
            n = 36;
        } else if (page == 0x00) {           // supported VPD pages
            memset(buf, 0, 8);
            buf[1] = 0x00;
            buf[4] = 0x00; buf[5] = 0x83;
            n = 6;
            if (features & (FEATURE_DISCARD | FEATURE_WRITE_ZEROES))
                buf[n++] = 0xB0;             // block limits (n becomes 7)
            if (features & FEATURE_DISCARD)
                buf[n++] = 0xB2;             // logical block provisioning
            buf[3] = (uint8_t)(n - 4);       // number of supported page codes
        } else if (page == 0x83) {           // device identification (the WWID)
            n = build_vpd83(buf);
        } else if (page == 0xB0 && (features & (FEATURE_DISCARD | FEATURE_WRITE_ZEROES))) {
            n = build_vpd_b0(buf);           // block limits (unmap / write-same)
        } else if (page == 0xB2 && (features & FEATURE_DISCARD)) {
            n = build_vpd_b2(buf);           // logical block provisioning
        } else {                             // any other page: empty
            memset(buf, 0, 4);
            buf[1] = page;
            n = 4;
        }
        if ((uint16_t)n > alloc) n = alloc;
        iovector_view(iov, iov_cnt).memcpy_from(buf, n);
        set_status(ent, SAM_GOOD);
    }

    // one NAA designator derived deterministically from the identity
    int build_vpd83(uint8_t* buf) {
        uint64_t h = fnv1a64(identity);
        uint8_t naa[8];
        naa[0] = 0x50 | (uint8_t)((h >> 60) & 0x0f);   // NAA = 5 (registered IEEE)
        for (int i = 1; i < 8; i++) naa[i] = (uint8_t)(h >> (60 - 8 * i));
        memset(buf, 0, 16);
        buf[0] = 0x00;   // peripheral
        buf[1] = 0x83;   // page code
        buf[4] = 0x01;   // code set = binary
        buf[5] = 0x03;   // identifier type = NAA
        buf[7] = 8;      // identifier length
        memcpy(buf + 8, naa, 8);
        put_be16(buf + 2, 12);   // page length = descriptor bytes (16 - 4)
        return 16;
    }

    // Block Limits VPD page (0xB0). The Linux SCSI disk driver reads the unmap
    // fields at [20..35] -- which is what turns BLKDISCARD into UNMAP -- only when
    // the page is at least 64 bytes; a shorter 0xB0 page leaves discard
    // unprovisioned (measured). Transfer-length fields [6..15] stay 0 because the
    // driver reads 0 there as "no limit", so existing I/O sizing is untouched.
    // Page length 0x3C -> 64 bytes total is that minimum.
    int build_vpd_b0(uint8_t* buf) {
        memset(buf, 0, 64);
        buf[1] = 0xB0;
        put_be16(buf + 2, 0x3C);             // page length 60 -> total 64
        if (features & FEATURE_DISCARD) {
            put_be32(buf + 20, 0x100000);    // max unmap LBA count
            put_be32(buf + 24, 32);          // max unmap block descriptor count
            put_be32(buf + 28, 1);           // optimal unmap granularity (blocks)
        }
        if (features & FEATURE_WRITE_ZEROES)
            put_be64(buf + 36, 0x100000);    // max write same length
        return 64;
    }

    // Logical Block Provisioning VPD page (0xB2). The Linux SCSI disk driver reads
    // this page only when it is at least 8 bytes (4-byte header + 4 data bytes),
    // and reads byte 5: LBPU(0x80)=UNMAP, LBPWS(0x40)=WRITE SAME(16) w/ UNMAP,
    // LBPWS10(0x20). We advertise LBPU|LBPWS (both paths land in
    // emul_unmap/emul_write_same); threshold exponent 0 = no provisioning
    // threshold reported.
    int build_vpd_b2(uint8_t* buf) {
        memset(buf, 0, 8);
        buf[1] = 0xB2;
        put_be16(buf + 2, 4);                // page length 4 -> total 8
        buf[5] = 0x80 | 0x40;                // LBPU | LBPWS
        return 8;
    }

    void emul_read_capacity(struct iovec* iov, uint32_t iov_cnt, tcmu_cmd_entry* ent, bool v16) {
        // RC16 byte layout the Linux SCSI disk driver parses: [0-7] last LBA,
        // [8-11] block length, [12] protection type + RC basis, [13] low nibble =
        // physical block exponent, [14-15] LBPME/LBPRZ + lowest-aligned LBA. Zero
        // the whole response first so bytes 12+ are 0 (PBE=0 -> physical==logical,
        // no protection, alignment 0) instead of stale ring garbage; byte 14 bits
        // 0x80 (LBPME) + 0x40 (LBPRZ) are set only when the device advertises
        // FEATURE_DISCARD -- LBPME is what makes the driver select its unmap-based
        // discard mode, and LBPRZ claims reads-after-unmap return zeros (true for a
        // punched hole; the backend's trim() must honor that), which makes the
        // driver route BLKZEROOUT to the unmap/write-same fast path instead of
        // writing zeroes itself.
        iovector_view(iov, iov_cnt).memset(0);
        uint64_t last = num_lbas ? num_lbas - 1 : 0;
        if (v16) {
            uint8_t buf[32] = {};
            put_be64(buf, last);
            put_be32(buf + 8, block_size);
            if (features & FEATURE_DISCARD)
                buf[14] = 0xC0;   // LBPME | LBPRZ
            iovector_view(iov, iov_cnt).memcpy_from(buf, 32);
        } else {
            uint8_t buf[8] = {};
            put_be32(buf, last > 0xffffffffull ? 0xffffffffu : (uint32_t)last);
            put_be32(buf + 4, block_size);
            iovector_view(iov, iov_cnt).memcpy_from(buf, 8);
        }
        set_status(ent, SAM_GOOD);
    }

    void emul_mode_sense(uint8_t* cdb, struct iovec* iov, uint32_t iov_cnt, tcmu_cmd_entry* ent,
                         bool ten) {
        uint8_t page = cdb[2] & 0x3f;
        uint8_t buf[64] = {};
        int hdr = ten ? 8 : 4;
        int n = hdr;
        iovector_view(iov, iov_cnt).memset(0);   // kernel gathers data_length bytes; no stale tail
        if (page == 0x08 || page == 0x3f) {   // caching page (also for "all pages")
            buf[n + 0] = 0x08;
            buf[n + 1] = 0x0a;                // page length 10
            // buf[n+2] WCE bit left 0: write cache reported disabled
            n += 12;
        }
        if (ten) {
            buf[3] = read_only ? 0x80 : 0;    // device-specific: write-protect
            put_be16(buf, (uint16_t)(n - 2)); // mode data length
        } else {
            buf[2] = read_only ? 0x80 : 0;
            buf[0] = (uint8_t)(n - 1);
        }
        iovector_view(iov, iov_cnt).memcpy_from(buf, n);
        set_status(ent, SAM_GOOD);
    }

    void emul_request_sense(struct iovec* iov, uint32_t iov_cnt, tcmu_cmd_entry* ent) {
        uint8_t s[18] = {};
        s[0] = 0x70; s[2] = SK_NO_SENSE; s[7] = 10;
        iovector_view(iov, iov_cnt).memset(0);   // kernel gathers data_length bytes; no stale tail
        iovector_view(iov, iov_cnt).memcpy_from(s, 18);
        set_status(ent, SAM_GOOD);
    }

    void emul_report_luns(struct iovec* iov, uint32_t iov_cnt, tcmu_cmd_entry* ent) {
        uint8_t buf[16] = {};
        put_be32(buf, 8);   // LUN list length = 8 (a single LUN 0, all-zero)
        iovector_view(iov, iov_cnt).memset(0);   // kernel gathers data_length bytes; no stale tail
        iovector_view(iov, iov_cnt).memcpy_from(buf, 16);
        set_status(ent, SAM_GOOD);
    }

    // UNMAP: the initiator sends a parameter list (8-byte header + 16-byte
    // descriptors) in the data buffer; trim each range. overlaybd skips UNMAP
    // entirely (fstrim fails there); we honor it.
    void emul_unmap(struct iovec* iov, uint32_t iov_cnt, size_t data_len, tcmu_cmd_entry* ent) {
        if (read_only) { set_sense(ent, SK_DATA_PROTECT, ASC_WRITE_PROTECTED, 0); return; }
        if (data_len < 8 || data_len > (1u << 20)) {
            set_sense(ent, SK_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
            return;
        }
        std::vector<uint8_t> p(data_len);
        iovector_view(iov, iov_cnt).memcpy_to(p.data(), data_len);
        uint16_t list_len = cdb_be16(p.data() + 2);
        if ((size_t)8 + list_len > data_len) list_len = (uint16_t)(data_len - 8);
        int ndesc = list_len / 16;
        for (int i = 0; i < ndesc; i++) {
            const uint8_t* d = p.data() + 8 + i * 16;
            uint64_t off = blocks_to_bytes(cdb_be64(d));
            uint64_t len = blocks_to_bytes(cdb_be32(d + 8));
            if (len == 0 || out_of_bounds(off, len)) continue;
            if (backend->trim(off, len) < 0 && errno != EOPNOTSUPP && errno != ENOSYS) {
                set_sense(ent, SK_MEDIUM_ERROR, ASC_WRITE_ERROR, 0);
                return;
            }
        }
        set_status(ent, SAM_GOOD);
    }

    // WRITE SAME: the UNMAP bit trims; NDOB or an all-zero block zero-fills
    // (the common mkfs case); a non-zero pattern is left to the initiator
    void emul_write_same(uint8_t* cdb, struct iovec* iov, uint32_t iov_cnt, size_t data_len,
                         tcmu_cmd_entry* ent) {
        if (read_only) { set_sense(ent, SK_DATA_PROTECT, ASC_WRITE_PROTECTED, 0); return; }
        bool sixteen = (cdb[0] == OPC_WRITE_SAME_16);
        bool unmap = cdb[1] & 0x08;
        bool ndob = sixteen && (cdb[1] & 0x10);
        uint64_t off = cdb_byte_off(cdb[0], cdb);
        uint32_t nblocks = sixteen ? cdb_be32(cdb + 10) : cdb_be16(cdb + 7);
        uint64_t len = (uint64_t)nblocks * block_size;
        if (len == 0) { set_status(ent, SAM_GOOD); return; }
        if (out_of_bounds(off, len)) {
            set_sense(ent, SK_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
            return;
        }
        if (unmap) {
            if (backend->trim(off, len) < 0 && errno != EOPNOTSUPP && errno != ENOSYS) {
                set_sense(ent, SK_MEDIUM_ERROR, ASC_WRITE_ERROR, 0);
                return;
            }
            set_status(ent, SAM_GOOD);
            return;
        }
        if (ndob || data_len == 0 || iovector_view(iov, iov_cnt).is_zero()) {
            if (zero_fill(backend, off, len) == 0) set_status(ent, SAM_GOOD);
            else                          set_sense(ent, SK_MEDIUM_ERROR, ASC_WRITE_ERROR, 0);
            return;
        }
        // recognized command but an unsupported (non-zero) pattern: report an
        // invalid field, not an unknown opcode; the initiator falls back to
        // plain writes either way
        set_sense(ent, SK_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
    }
};

// ----------------------------------------------------------------------------
// TcmuDeviceImpl: the IBlkDevice. configfs lifecycle + flock + tcm_loop LUN +
// a TcmuServer.
// ----------------------------------------------------------------------------

static const char* const TARGET_ROOT = "/sys/kernel/config/target";
struct TcmuDeviceImpl;   // the device: defined just below

// Device <-> HBA linkage. Written by the HBA's listener vcpu, read
// by the device on whatever vcpu its caller runs on, hence all atomic.
struct TcmuLink {
    TcmuDeviceImpl* dev = nullptr;              // back-pointer, for orphaning on
                                                // HBA teardown
    int fam = 0;                              // TCM-USER genetlink family id
    std::atomic<bool> serving{false};         // the device is serving its ring now
    std::atomic<bool> starting{false};        // start() is in flight on some vcpu. The
                                              // registry entry exists from construction, so
                                              // "a device is registered under this name" is
                                              // weaker than "a ring is being brought up" --
                                              // and only the latter may answer an ADDED
    std::atomic<uint32_t> added_id{0};        // ADDED to answer from start()
    std::atomic<uint32_t> removed_id{0};      // REMOVED to answer once serving stopped
    std::atomic<uint32_t> reconfig_id{0};     // operator RECONFIG(dev_size) awaiting
    std::atomic<uint64_t> pending_size{0};    // ... the size it asked for
    std::atomic<uint64_t> self_size{0};       // our own resize() in flight: the listener
                                              // matches it to answer our own RECONFIG
                                              // instead of queueing it back at us
};

// The HBA's device registry and its ADDED hand-over table. A class of its own,
// defined BEFORE both of its users, because those two point at each other: a
// device joins the registry from its constructor and leaves it from its
// destructor, while the HBA matches kernel events against it and builds devices
// from it -- so neither can be defined first without needing the other's
// complete type. Both vcpus touch this: the HBA's listener matches events here,
// the caller's registers and unregisters.
struct TcmuRegistry {
    static constexpr size_t BS_NAME_BUF = 65;   // backstore name, see TcmuDeviceImpl::validate()

    // A const char* key hashes and compares by ADDRESS by default, so both are
    // spelled out: the registry is keyed by backstore NAME.
    struct CStrHash {
        size_t operator()(const char* s) const { return std::hash<std::string_view>()(s); }
    };
    struct CStrEqual {
        bool operator()(const char* a, const char* b) const { return strcmp(a, b) == 0; }
    };
    // Devices by backstore name. The key BORROWS the device's own bs_name buffer
    // -- no copy, no allocation. Two invariants are therefore load-bearing: a
    // device unregisters before that buffer dies (its dtor does, and the HBA's
    // dtor clears the map), and the buffer's CONTENT is fixed once the ctor
    // derives it -- a hash table cannot tolerate a stored key changing under it.
    photon::spinlock reg_lock;
    std::unordered_map<const char*, TcmuLink*, CStrHash, CStrEqual> devs;

    // ADDED events handed to the caller, awaiting the device that will answer
    // them. A dev_id exists only inside its netlink message -- no configfs attrib
    // reads one back -- and wait_for_event() pops the event, so this table is the
    // only channel by which new_device() learns which reply the device it is about
    // to build owes. Keyed by backstore name, because that is all new_device() can
    // derive (and blk.h therefore requires info.identity to name the backstore).
    // A re-ADDED overwrites: the earlier dev_id is dead once the kernel tears that
    // registration down. Bounded, because an entry the caller neither serves nor
    // denies would otherwise outlive the backstore it names -- and a dev_id can be
    // recycled by the kernel, so a stale one must not be answered.
    static constexpr size_t MAX_PENDING_ADDED = 64;
    struct PendingAdded {
        char bs[BS_NAME_BUF];
        uint32_t dev_id;
    };
    std::vector<PendingAdded> pending_added;   // reg_lock

    TcmuLink* find_link(const char* bs) {
        SCOPED_LOCK(reg_lock);
        auto it = devs.find(bs);
        return it == devs.end() ? nullptr : it->second;
    }

    void register_link(TcmuLink* link, const char* bs_name) {
        if (!bs_name[0])
            return;
        SCOPED_LOCK(reg_lock);
        // The key BORROWS this device's bs_name buffer, and operator[] keeps the key
        // already stored -- so re-inserting under an equal name would leave it
        // pointing at the OTHER device's buffer, which may die first. Erase by name.
        auto it = devs.find(bs_name);
        if (it != devs.end())
            devs.erase(it);
        devs[bs_name] = link;
    }

    void unregister_link(TcmuLink* link) {
        SCOPED_LOCK(reg_lock);
        for (auto it = devs.begin(); it != devs.end(); )
            it = (it->second == link) ? devs.erase(it) : std::next(it);
    }

    // ----- the ADDED hand-over: the listener vcpu remembers, the caller's claims -----
    // Nothing below may log while holding reg_lock: it is a spinlock shared with
    // the caller's vcpu.

    // false = not remembered. The caller can still deny() the event (which
    // carries its own dev_id), but no device will be able to answer it.
    bool remember_added(const char* bs, uint32_t dev_id) {
        if (!dev_id)
            return false;   // a synthesized ADDED owes nothing
        if (strlen(bs) >= BS_NAME_BUF) {
            LOG_WARN("tcmu backstore ` is longer than the ` chars a device can serve, so it can only be denied",
                     bs, BS_NAME_BUF - 1);
            return false;
        }
        char evicted[BS_NAME_BUF] = {};
        {
            SCOPED_LOCK(reg_lock);
            for (auto& p : pending_added)
                if (strcmp(p.bs, bs) == 0) {
                    p.dev_id = dev_id;   // a re-ADDED: the earlier id is dead
                    return true;
                }
            if (pending_added.size() >= MAX_PENDING_ADDED) {
                memcpy(evicted, pending_added.front().bs, sizeof(evicted));
                pending_added.erase(pending_added.begin());
            }
            PendingAdded p{};
            // the length check above already bounds this; the precision only says so
            snprintf(p.bs, sizeof(p.bs), "%.*s", (int)sizeof(p.bs) - 1, bs);
            p.dev_id = dev_id;
            pending_added.push_back(p);
        }
        if (evicted[0])
            LOG_WARN("tcmu pending-ADDED table is full (`); dropped the entry for `",
                     MAX_PENDING_ADDED, evicted);
        return true;
    }

    // dev_id 0 drops whatever is pending under this name
    void forget_added(const char* bs, uint32_t dev_id) {
        SCOPED_LOCK(reg_lock);
        for (auto it = pending_added.begin(); it != pending_added.end(); )
            it = (strcmp(it->bs, bs) == 0 && (!dev_id || it->dev_id == dev_id))
                     ? pending_added.erase(it) : std::next(it);
    }

    uint32_t take_added(const char* bs) {
        SCOPED_LOCK(reg_lock);
        for (auto it = pending_added.begin(); it != pending_added.end(); ++it) {
            if (strcmp(it->bs, bs) != 0)
                continue;
            uint32_t id = it->dev_id;
            pending_added.erase(it);
            return id;
        }
        return 0;
    }
};

// Answer one of the kernel's *_DONE waits. A short-lived socket per reply, so a
// device may answer from any vcpu: GenlSock::transact waits on the CALLING
// vcpu's event engine, so the HBA's own socket cannot be shared. Replies
// are rare (device lifecycle), and a reply the kernel is not waiting for is
// benign (-ENODEV/-EINVAL in the ack), so failures only log at debug.
static int tcmu_send_done(int fam, uint8_t done_cmd, uint32_t dev_id, int32_t status) {
    if (!dev_id)
        return 0;   // no event tied to this device: nothing is owed
    GenlSock gs;
    if (gs.sk < 0)
        LOG_ERRNO_RETURN(0, -1, "failed to create a genetlink socket for a tcmu reply");
    char attrs[64], sink[64];
    size_t off = nla_append(attrs, 0, sizeof(attrs), TCMU_ATTR_CMD_STATUS, &status, sizeof(status));
    off += nla_append_u32(attrs, off, sizeof(attrs), TCMU_ATTR_DEVICE_ID, dev_id);
    ssize_t r = gs.transact(fam, done_cmd, attrs, off, sink, sizeof(sink));
    if (r < 0)
        LOG_DEBUG("tcmu DONE reply cmd=` dev=` status=` got `", (int)done_cmd, dev_id, status, (int)r);
    return 0;
}

struct TcmuDeviceImpl : IBlkDevice {
    static constexpr size_t WWN_BUF = 256;   // tcm_loop WWN buffer, see validate()
    static constexpr size_t BS_NAME_BUF = TcmuRegistry::BS_NAME_BUF;   // backstore name, see validate()

    // Field order is padding-driven, do not tidy it: the four bools used to sit
    // among the wide members, where each stranded the align-4 or align-8 member
    // behind it -- own_backend/started cost 2 bytes before lock_fd, created and
    // lun_attached 5 before `server`, on top of 7 bytes of tail -- so they now
    // trail the odd-sized bs_name instead. 1936 bytes vs 1944 (and vs the 1968
    // measured before TcmuServer shrank to 496).
    TcmuHBA::Config cfg;
    fs::IFile* backend = nullptr;

    int lock_fd = -1;
    // The scope directory this device claims its tombstone in, handed over by the
    // HBA -- the same one its list_orphans() and on_added() probe. COPIED, not
    // borrowed: a device may outlive its HBA (orphan() exists for that).
    char lock_dir[SCOPE_DIR_BUF] = {};
    char lock_name[80] = {};  // "tcmu-<bs_name>.lock" inside the lock dir
    // log as (const char*), never VALUE(): alog would emit all 128 bytes
    char bs_path[128] = {};      // configfs backstore dir (≤ 103 chars)
    char bs_name[BS_NAME_BUF] = {};   // backstore name (sanitized identity)
    char hba[32] = "user_0";     // configfs HBA dir under target/core/, our TcmuHBA's
    char hbanum[32] = "0";       // hba after "user_", for the uio name lookup
    char wwn[WWN_BUF] = {};      // tcm_loop WWN
    char lb_path[320] = {};      // configfs loopback/<wwn> dir (≤ 291 chars)
    char node_path[64] = {};     // "/dev/sdX" of the tcm_loop LUN; "" if none

    TcmuServer server;

    // HBA linkage: reg is null for a device whose TcmuHBA is already gone;
    // link carries the kernel dev_ids this device has to answer
    TcmuRegistry* reg = nullptr;
    TcmuLink link;
    bool kern_reply = false;    // our HBA engaged the reply protocol, so a backstore
                                // we create must NOT opt out of the kernel's wait
    bool own_backend = false;
    bool started = false;
    bool created = false;       // we created the registration (vs attached an existing one)
    bool lun_attached = false;

    // The backstore name is derived here exactly as start() would, so the device
    // joins the HBA's registry under its FINAL name from birth: the key never
    // changes content, and a REMOVED or an operator RECONFIG arriving before
    // start() is matched too.
    //
    // If this device answers an ADDED the HBA handed out, its dev_id is claimed
    // by name (TcmuRegistry::take_added): the id exists only in that one netlink
    // message and wait_for_event() already popped the event, so the caller could
    // not give it back even if asked. That is why blk.h requires info.identity to
    // NAME the backstore -- the derived name is the lookup key.
    TcmuDeviceImpl(TcmuRegistry* r, int fam, const char* subtype, bool reply,
                   const char* ldir, const TcmuHBA::Config& c)
        : cfg(c), reg(r), kern_reply(reply) {
        link.dev = this;
        link.fam = fam;
        if (subtype && strlen(subtype) < sizeof(hba)) {
            strcpy(hba, subtype);
            if (strncmp(hba, "user_", 5) == 0 && hba[5])
                strcpy(hbanum, hba + 5);
        }
        if (ldir)
            snprintf(lock_dir, sizeof(lock_dir), "%s", ldir);   // bounded: new_tcmu_hba checked
        sanitize(bs_name, sizeof(bs_name), cfg.info.identity.c_str());
        reg->register_link(&link, bs_name);
        link.added_id = reg->take_added(bs_name);
    }

    // clear a device's registry pointer -- the HBA is going away: stop calling into it
    static void orphan(TcmuLink* link) {
        if (link && link->dev)
            link->dev->reg = nullptr;
    }

    // the flock file name of a backstore. ONE implementation, called both by the
    // device (which claims the tombstone) and by the HBA (which probes it in the
    // orphan scans and in on_added), so the two sides cannot drift apart on who is
    // serving what. sanitize() is idempotent, so an already-sanitized name is fine.
    static void lock_file_name(const char* bs, char* out, size_t n) {
        char sn[65];
        sanitize(sn, sizeof(sn), bs);
        snprintf(out, n, "tcmu-%s.lock", sn);
    }

    // pure config validation -- no I/O, no kernel access. new_device() runs it
    // before constructing, so a constructed device is always config-valid.
    static int validate(const TcmuHBA::Config& c) {
        if (!c.info.size)
            LOG_ERROR_RETURN(EINVAL, -1, "device size must be nonzero");
        if (c.info.identity.empty())
            LOG_ERROR_RETURN(EINVAL, -1, "device identity must be non-empty");
        // The identity BECOMES the backstore dir name: sanitize() truncates into
        // bs_name, and a truncated name would not match the kernel's, so the HBA
        // could never match an ADDED to this device and the operator's enable
        // would hang. (It is also "photon/<identity>" in dev_config, read back
        // into 256-byte buffers -- 64 chars leaves that ample.)
        if (c.info.identity.size() >= BS_NAME_BUF)
            LOG_ERROR_RETURN(ENAMETOOLONG, -1, "device identity is too long for a tcmu backstore name (` bytes, max `)",
                             c.info.identity.size(), BS_NAME_BUF - 1);
        if (c.info.size % 512)
            LOG_ERROR_RETURN(EINVAL, -1, "size ` is not a multiple of the 512-byte tcmu sector",
                             c.info.size);
        if (c.info.sector_size_shift != 9)
            LOG_WARN("tcmu forces a 512-byte sector; ignoring sector_size_shift=`",
                     (int)c.info.sector_size_shift);
        if (!c.loopback_wwn.empty() && c.loopback_wwn.size() >= WWN_BUF)
            LOG_ERROR_RETURN(ENAMETOOLONG, -1, "loopback_wwn is too long (` bytes)",
                             c.loopback_wwn.size());
        return 0;
    }

    const BlkDevInfo& get_info() const override { return cfg.info; }

    // nullptr unless a tcm_loop LUN is attached and its /dev/sdX was resolved
    const char* get_device_node() override {
        return node_path[0] ? node_path : nullptr;
    }

    ~TcmuDeviceImpl() {
        // best-effort teardown for a serving OR detached device: bs_path is set
        // once start() registers and cleared only by a successful shutdown()
        if (started || bs_path[0])
            shutdown();
        // An ADDED this device claimed but never answered: it was constructed and
        // then abandoned, so start()'s DEFER never ran (a start() that ran at all
        // exchanged the id away, success and failure alike). Refuse it rather than
        // leave the operator's `echo 1 > enable` blocked until someone reaches for
        // reset_netlink -- the operator can retry against a successor.
        if (uint32_t id = link.added_id.exchange(0)) {
            LOG_WARN("tcmu device ` is going away with an unanswered ADDED (dev `); refusing it",
                     bs_name, id);
            tcmu_send_done(link.fam, TCMU_CMD_ADDED_DEVICE_DONE, id, -ENOSYS);
        }
        if (reg)
            reg->unregister_link(&link);
        if (own_backend)
            delete backend;
    }

    int start(fs::IFile* bk, bool ownership) override {
        if (started)
            LOG_ERROR_RETURN(EALREADY, -1, "tcmu device already started");
        if (!bk)
            LOG_ERROR_RETURN(EINVAL, -1, "backend IFile is null");

        backend = bk;
        own_backend = ownership;
        // bs_name and the registry entry are the ctor's (identity is fixed at
        // construction); these paths are cleared by rollback(), so re-derive them
        if (cfg.loopback_wwn.empty()) derive_wwn(wwn, sizeof(wwn), cfg.info.identity.c_str());
        else                          strcpy(wwn, cfg.loopback_wwn.c_str());
        snprintf(bs_path, sizeof(bs_path), "%s/core/%s/%s", TARGET_ROOT, hba, bs_name);
        snprintf(lb_path, sizeof(lb_path), "%s/loopback/%s", TARGET_ROOT, wwn);
        lock_file_name(bs_name, lock_name, sizeof(lock_name));

        bool ok = false;
        link.starting = true;   // an ADDED arriving from here on is ours to answer
        // One may also have arrived BEFORE this start(): detach() keeps the
        // registration but frees the flock, so an operator re-enabling it hands the
        // event to the caller instead of getting an answer from the listener -- and
        // a caller that restarts THIS object already ran its constructor. Only a
        // nonzero id, so one the ctor claimed is not clobbered by a miss.
        if (uint32_t id = reg ? reg->take_added(bs_name) : 0)
            link.added_id = id;
        DEFER({
            int e = errno;
            link.starting = false;
            if (!ok)
                rollback();
            // answer the operator's blocked `echo 1 > enable` if the HBA handed
            // this device an ADDED: 0 only now that the ring is served, else the
            // failure errno so the operator's write fails with it
            tcmu_send_done(link.fam, TCMU_CMD_ADDED_DEVICE_DONE, link.added_id.exchange(0),
                           ok ? 0 : -(e ? e : EIO));
            errno = e;
        });

        if (acquire_lock() < 0)
            return -1;

        if (path_exists(bs_path)) {
            if (validate_existing() < 0)
                return -1;
        } else if (create_backstore() < 0)
            return -1;

        // geometry: hw_block_size is read-only and the kernel sets it to 512 at
        // enable, so read it back as authoritative
        char hp[300];
        snprintf(hp, sizeof(hp), "%s/attrib/hw_block_size", bs_path);
        server.block_size = (uint32_t)cfg_read_u64(hp, 512);
        if (!server.block_size) server.block_size = 512;
        server.dev_size = cfg.info.size;
        server.num_lbas = cfg.info.size / server.block_size;
        server.backend = backend;
        snprintf(server.identity, sizeof(server.identity), "%s", cfg.info.identity.c_str());
        server.read_only = cfg.read_only;
        server.features = cfg.info.features;
        server.poll = cfg.poll;
        server.spin_us = cfg.spin_us;
        server.pending_capacity_ua = false;

        // discover the uio node (appears once the backstore is enabled)
        char node[64];
        if (find_uio(node, sizeof(node)) < 0)
            return -1;

        // SERVE THE RING BEFORE attaching the LUN: the attach triggers a SCSI
        // scan that blocks until a handler answers INQUIRY/READ CAPACITY
        if (server.start(node, cfg.queue_depth, cfg.vcpus, resolve_stack_size(cfg.stack_size)) < 0)
            return -1;

        if (cfg.loopback_lun && attach_lun() < 0)
            return -1;

        started = true;
        ok = true;
        link.serving = true;   // the listener hands REMOVED events over from now on
        LOG_INFO("tcmu device started, ", VALUE(cfg.info.identity), VALUE(cfg.info.size),
                 make_named_value("bs_path", (const char*)bs_path), node, "loopback_lun=", (int)cfg.loopback_lun);
        return 0;
    }

    int detach(bool wait_pending) override {
        if (!started)
            return 0;
        // stop serving but KEEP the configfs registration + LUN so a later
        // start() (possibly another process) takes over and harvests the ring
        server.stop(/*flush=*/wait_pending);
        release_lock();
        started = false;
        link.serving = false;
        // the operator's rmdir/enable=0 is blocked on this: answer only now that
        // serving has stopped, since the kernel unregisters the uio right after
        tcmu_send_done(link.fam, TCMU_CMD_REMOVED_DEVICE_DONE, link.removed_id.exchange(0), 0);
        return 0;
    }

    // detach the LUN, stop serving, then destroy the registration. EBUSY if the
    // initiator still holds the device (the backstore rmdir refuses).
    //
    // Works from the detached state too (a prior detach() kept the registration
    // + LUN but stopped the pump): re-acquire the lock and restart the pump
    // first, because detaching the LUN makes the kernel's SCSI disk driver issue
    // its teardown commands (SYNCHRONIZE CACHE / START STOP UNIT) that only a
    // live pump answers. EBUSY from the lock means another live process took the
    // identity over -- leave its device alone.
    int shutdown() override {
        // An operator-driven removal: its rmdir/enable=0 is blocked inside the
        // kernel waiting for our answer, so do NOT re-serve to detach a LUN or
        // write to configfs -- that would deadlock against the very write we have
        // to unblock. Stop serving, answer, forget the registration (the operator
        // is destroying it; the LUN is already gone, since the kernel refuses to
        // remove a backstore a LUN still references).
        if (uint32_t id = link.removed_id.exchange(0)) {
            if (started) {
                server.stop(/*flush=*/true);
                release_lock();
                started = false;
            }
            link.serving = false;
            tcmu_send_done(link.fam, TCMU_CMD_REMOVED_DEVICE_DONE, id, 0);
            bs_path[0] = '\0';
            lun_attached = false;
            created = false;
            LOG_INFO("tcmu device ` removed by its operator", bs_name);
            return 0;
        }
        // answer a pending REMOVED on every path that leaves us not serving, so a
        // failure here cannot leave the operator's rmdir blocked forever
        DEFER(if (!started)
            tcmu_send_done(link.fam, TCMU_CMD_REMOVED_DEVICE_DONE, link.removed_id.exchange(0), 0));
        if (!started) {
            if (!bs_path[0])
                return 0;   // virgin, or already shut down: nothing tracked
            if (acquire_lock() < 0)
                return -1;
            char node[64];
            if (find_uio(node, sizeof(node)) < 0 ||
                server.start(node, cfg.queue_depth, cfg.vcpus, resolve_stack_size(cfg.stack_size)) < 0) {
                release_lock();
                return -1;
            }
            started = true;
            link.serving = true;
        }
        // remove the LUN FIRST while the server still answers the teardown
        // commands the initiator may send, then stop the ring
        if (lun_attached)
            detach_lun();
        server.stop(/*flush=*/true);
        release_lock();
        started = false;
        // BEFORE destroy_backstore(): our own disable/rmdir fires a REMOVED that
        // the listener must answer for us, since this vcpu is inside that write
        link.serving = false;
        if (destroy_backstore() < 0)
            return -1;
        bs_path[0] = '\0';  // registration gone: shutdown() is now idempotent
        return 0;
    }

    // Make the device's size new_size (the backend must already be enlarged --
    // blk.h contract). Declarative, and the answer to an operator-driven
    // RECONFIG: with one pending for this device, apply it and reply
    // RECONFIG_DEVICE_DONE -- a shrink or a misaligned size is vetoed with a
    // negative status, which fails the operator's write and leaves the old value
    // committed, because the kernel commits dev_size only after our reply.
    // Otherwise write dev_size ourselves; the HBA's listener recognizes the
    // resulting event by self_size and answers it, since THIS vcpu is blocked
    // inside the configfs write and could never answer itself. (No throwaway
    // subscription is needed for the ESRCH case: a TcmuHBA is always
    // subscribed.) Pends a one-shot UNIT ATTENTION so the initiator revalidates
    // its cached capacity; works from the detached state too.
    int resize(uint64_t new_size) override {
        if (!bs_path[0])
            LOG_ERROR_RETURN(ENODEV, -1, "tcmu resize: no registration (virgin or shut down)");
        uint64_t bs = server.block_size;
        uint64_t cur = cfg.info.size;
        int32_t veto = 0;
        if (new_size % bs)
            veto = -EINVAL;
        else if (new_size < cur)
            veto = -EINVAL;   // shrink: it would truncate the backend under us

        if (uint32_t id = link.reconfig_id.exchange(0)) {
            uint64_t want = link.pending_size.exchange(0);
            if (want != new_size) {   // a different request is waiting: leave it
                link.pending_size = want;
                link.reconfig_id = id;
                LOG_ERROR_RETURN(EBUSY, -1, "an operator reconfig of ` to ` is pending; answer that size first", bs_name, want);
            }
            if (!veto && new_size != cur)
                apply_size(new_size, bs);
            tcmu_send_done(link.fam, TCMU_CMD_RECONFIG_DEVICE_DONE, id, veto);
            if (veto)
                LOG_ERROR_RETURN(-veto, -1, "tcmu reconfig of ` vetoed: size ` (block `, cur `)",
                                 bs_name, new_size, bs, cur);
            return 0;
        }

        if (new_size % bs)
            LOG_ERROR_RETURN(EINVAL, -1, "resize size ` is not a multiple of the `-byte sector",
                             new_size, bs);
        if (new_size < cur)
            LOG_ERROR_RETURN(EINVAL, -1, "tcmu resize: shrink (` -> `) is rejected", cur, new_size);
        if (new_size == cur)
            return 0;
        link.self_size = new_size;
        DEFER(link.self_size = 0);
        char p[PATH_MAX];   // log as (const char*), never VALUE(): alog would emit all PATH_MAX bytes
        snprintf(p, sizeof(p), "%s/attrib/dev_size", bs_path);
        if (cfg_write_u64(p, new_size) < 0) {
            // no such attrib: the kernel predates the dev_size/reconfig support
            if (errno == ENOENT)
                LOG_ERROR_RETURN(ENOTSUP, -1, "tcmu resize needs the kernel's dev_size attrib (v4.13+), ",
                                 make_named_value("p", (const char*)p));
            return -1;   // already logged
        }
        apply_size(new_size, bs);
        LOG_INFO("tcmu device resized, ", VALUE(cfg.info.identity), VALUE(cur), VALUE(new_size));
        return 0;
    }

    void apply_size(uint64_t new_size, uint64_t bs) {
        server.num_lbas = new_size / bs;
        server.dev_size = new_size;
        cfg.info.size = new_size;
        server.pending_capacity_ua = true;
    }

    // ----- flock: the per-device liveness key for orphan detection -----

    int acquire_lock() {
        if (devlock_acquire(lock_dir, lock_name, &lock_fd) < 0) {
            if (errno == EBUSY)
                LOG_ERROR_RETURN(EBUSY, -1, "tcmu device ` is held by another live server", bs_name);
            return -1;   // devlock_acquire logged it
        }
        return 0;
    }
    void release_lock() {
        devlock_release(lock_fd);
        lock_fd = -1;
    }

    // ----- configfs create / validate / destroy -----

    int create_backstore() {
        char core[64], hba_dir[96];
        snprintf(core, sizeof(core), "%s/core", TARGET_ROOT);
        if (!path_exists(core))
            LOG_ERROR_RETURN(ENOENT, -1, "configfs target not mounted (missing `)", core);
        snprintf(hba_dir, sizeof(hba_dir), "%s/%s", core, hba);
        if (cfg_mkdir(hba_dir) < 0)
            return -1;
        if (cfg_mkdir(bs_path) < 0)
            return -1;
        created = true;

        char p[PATH_MAX], dev_config[256];
        snprintf(dev_config, sizeof(dev_config), "photon/%s", cfg.info.identity.c_str());
        // The per-backstore attribs are v4.13+. Older kernels take the same
        // information as an option string ("dev_config=... dev_size=...") written
        // to the LIO-generic `control` file, which this implementation does not
        // write -- so name the floor instead of failing with a bare ENOENT.
        // RHEL/CentOS 7.3-7.5 are exactly that shape despite already carrying the
        // v4.13+ uapi; 7.6+ has the full attrib set. See KERNEL FLOOR in blk.h.
        snprintf(p, sizeof(p), "%s/attrib/dev_config", bs_path);
        if (cfg_write(p, dev_config) < 0) {
            if (errno == ENOENT)
                LOG_ERROR_RETURN(ENOTSUP, -1, "no `: tcmu backstore attribs need kernel v4.13+", p);
            return -1;
        }
        snprintf(p, sizeof(p), "%s/attrib/dev_size", bs_path);
        if (cfg_write_u64(p, cfg.info.size) < 0) return -1;
        if (cfg.timeout) {
            snprintf(p, sizeof(p), "%s/attrib/cmd_time_out", bs_path);
            if (cfg_write_u64(p, cfg.timeout) < 0) return -1;
            // qfull_time_out is the restart-window knob: commands queue this long
            // while the daemon is down, covering an orderly restart. It exists only
            // since v4.19 (RHEL/CentOS 7.5+); before that qfull commands queue with
            // no timeout at all, so a missing attrib is weaker protection, not an
            // error.
            snprintf(p, sizeof(p), "%s/attrib/qfull_time_out", bs_path);
            if (cfg_write_u64(p, cfg.timeout) < 0) {
                if (errno != ENOENT)
                    return -1;
                LOG_WARN("no ` (kernel older than v4.19): qfull commands wait without a timeout", p);
            }
        }
        if (cfg.info.features & FEATURE_FLUSH) {
            snprintf(p, sizeof(p), "%s/attrib/emulate_write_cache", bs_path);
            if (cfg_write_u64(p, 1) < 0) return -1;
        }
        // Opt out of the kernel's command-reply wait unless our HBA engaged the
        // protocol itself. The flag that arms those waits is MODULE-global -- any
        // daemon can raise it, and it has no getter -- while we only answer waits
        // we raised ourselves. Without this, another daemon's flag would make our
        // own `echo 1 > enable` below hang forever, uninterruptibly (and on RHEL,
        // whose tcmu_netlink_event waits for every command rather than only
        // ADDED, the later remove and resize writes as well). The attrib is
        // v4.15+ (RHEL/CentOS 7.6+); on v4.13/v4.14 -- and on RHEL 7.3-7.5,
        // which backported the protocol but not this attrib -- there is no
        // per-device opt-out and the caller's only protection is new_tcmu_hba's
        // defensive_reply (see blk.h).
        if (!kern_reply) {
            snprintf(p, sizeof(p), "%s/attrib/nl_reply_supported", bs_path);
            if (cfg_write(p, "-1") < 0) {
                if (errno != ENOENT)
                    return -1;
                LOG_WARN("no ` (kernel older than v4.15): this backstore cannot opt out of a "
                         "foreign reply-mode daemon; pass defensive_reply=true to new_tcmu_hba "
                         "if one may run on this host", p);
            }
        }

        snprintf(p, sizeof(p), "%s/enable", bs_path);
        if (cfg_write(p, "1") < 0)
            return -1;
        return 0;
    }

    // attach to an existing registration: validate cfg.info against it (config
    // drift is a hard EINVAL)
    int validate_existing() {
        char p[PATH_MAX], b[256];
        snprintf(p, sizeof(p), "%s/attrib/dev_config", bs_path);
        if (cfg_read(p, b, sizeof(b)) < 0)
            return -1;
        if (cfg.adopt_external) {
            // an operator's backstore (targetcli/rtslib/overlaybd): dev_config is
            // theirs, so only the size is ours to validate
            LOG_INFO("adopting external tcmu backstore `, dev_config=", bs_name, b);
        } else {
            // dev_config is "photon/<identity>"
            if (strncmp(b, "photon/", 7) != 0)
                LOG_ERROR_RETURN(EINVAL, -1, "backstore ` is not a photon device (dev_config=`); set adopt_external to serve an operator's backstore", bs_name, b);
            const char* ident = b + 7;
            if (strcmp(ident, cfg.info.identity.c_str()) != 0)
                LOG_ERROR_RETURN(EINVAL, -1, "identity drift: registration `, requested `",
                                 ident, cfg.info.identity);
        }
        snprintf(p, sizeof(p), "%s/attrib/dev_size", bs_path);
        uint64_t sz = cfg_read_u64(p, 0);
        if (sz != cfg.info.size)
            LOG_ERROR_RETURN(EINVAL, -1, "size drift: registration `, requested `", sz, cfg.info.size);
        created = false;
        return 0;
    }

    // disable + rmdir the registration; EBUSY surfaces if a LUN still references it
    int destroy_backstore() {
        if (!bs_path[0] || !path_exists(bs_path)) {
            created = false;
            return 0;
        }
        // disable is best-effort: it may fail (e.g. the kernel already tore the
        // device down); the rmdir below is the real cleanup, so tolerate it
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/enable", bs_path);
        cfg_write_best(p, "0");
        if (::rmdir(bs_path) != 0) {
            int e = errno;
            if (e == EBUSY)
                LOG_ERROR_RETURN(EBUSY, -1, "tcmu backstore ` is still in use (LUN attached / mounted)", bs_name);
            LOG_ERRNO_RETURN(0, -1, "failed to rmdir backstore ", bs_path);
        }
        created = false;
        return 0;
    }

    // ----- uio discovery (sysfs scan; the genetlink path is P2) -----

    int find_uio(char* node, size_t n) {
        // the uio device appears when the backstore is enabled; poll briefly
        for (int i = 0; i < 3000; i++) {
            if (TcmuUio::find(hbanum, bs_name, node, n) == 0)
                return 0;
            photon::thread_usleep(1000);
        }
        LOG_ERROR_RETURN(ETIMEDOUT, -1, "uio device for backstore ` did not appear within 3s", bs_name);
    }

    // ----- tcm_loop LUN: makes a local /dev/sdX appear -----

    // The /dev/sdX of our tcm_loop LUN, resolved EXACTLY rather than by scanning
    // /sys/block for a matching SCSI vendor and size: the fabric's read-only
    // `address` attrib is "<host>:0:<tpgt>" (present since v4.13, like the rest
    // of our floor) and the loopback fabric's port link adds the scsi device at
    // (host, 0, tpgt, lun) -- and our LUN is always lun_0.
    //
    // It is NOT there the instant the LUN symlink returns. The kernel adds the
    // scsi device synchronously inside that write, but the disk registration --
    // and so the block/ link -- is handed to async work, and that work issues
    // READ CAPACITY, which this backstore's pump must answer. So poll ON the vcpu,
    // yielding to the pump, exactly as find_uio() does for the uio node. A
    // timeout costs the convenience getter only, never the export.
    void resolve_node() {
        node_path[0] = '\0';
        char p[PATH_MAX], addr[64];
        snprintf(p, sizeof(p), "%s/tpgt_1/address", lb_path);
        if (cfg_read(p, addr, sizeof(addr)) < 0)
            return;   // already logged, naming the path
        int host = -1, tpgt = -1;
        if (sscanf(addr, "%d:0:%d", &host, &tpgt) != 2) {
            LOG_ERROR("unparsable tcm_loop address ` in ", addr, p);
            return;
        }
        char blk[PATH_MAX];
        snprintf(blk, sizeof(blk), "/sys/bus/scsi/devices/%d:0:%d:0/block", host, tpgt);
        for (int i = 0; i < 3000; i++) {
            if (DIR* d = ::opendir(blk)) {
                DEFER(::closedir(d));
                struct dirent* e;
                while ((e = ::readdir(d))) {
                    if (e->d_name[0] == '.')
                        continue;
                    // a kernel disk name is at most DISK_NAME_LEN-1 = 31 chars so
                    // this never truncates; the precision just tells the compiler
                    // so (dirent declares d_name as char[256])
                    snprintf(node_path, sizeof(node_path), "/dev/%.*s",
                             (int)sizeof(node_path) - 6, e->d_name);
                    return;
                }
            }
            photon::thread_usleep(1000);
        }
        LOG_WARN("no block device appeared under ` within 3s; the tcmu block node "
                 "stays unknown (the export itself is unaffected)", blk);
    }

    int attach_lun() {
        char tpgt[352], lun0[384], link[512];   // chained: lb_path/tpgt_1/lun/lun_0/<bs_name>
        snprintf(tpgt, sizeof(tpgt), "%s/tpgt_1", lb_path);
        snprintf(lun0, sizeof(lun0), "%s/lun/lun_0", tpgt);
        snprintf(link, sizeof(link), "%s/%s", lun0, bs_name);
        if (path_exists(link)) {   // already attached (e.g. re-start after detach)
            lun_attached = true;
            resolve_node();
            return 0;
        }
        // The LUN symlink makes the kernel add the scsi device synchronously inside
        // the syscall, issuing an INQUIRY this backstore's pump must answer -- so the
        // whole sequence runs off the vcpu (run_off_vcpu). Flag attached eagerly: a
        // partial failure is still unwound by rollback(). Pump is alive (server.start
        // ran before attach_lun).
        lun_attached = true;
        int ret = run_off_vcpu([&]() -> int {
            // Fabric dirs are USERSPACE-created on all supported kernels:
            // loading the module only lists the fabric, and mkdir
            // target/loopback triggers the configfs make_group that
            // materializes it (this is what rtslib/targetcli have always
            // done). EEXIST = a previous attach left it in place.
            char fab[288];
            snprintf(fab, sizeof(fab), "%s/loopback", TARGET_ROOT);
            if (cfg_mkdir(fab) < 0) return -1;
            if (cfg_mkdir(lb_path) < 0) return -1;
            if (cfg_mkdir(tpgt) < 0) return -1;
            char nx[PATH_MAX];
            snprintf(nx, sizeof(nx), "%s/nexus", tpgt);
            if (cfg_write(nx, wwn) < 0) return -1;
            if (cfg_mkdir(lun0) < 0) return -1;
            if (::symlink(bs_path, link) != 0)
                LOG_ERRNO_RETURN(0, -1, "failed to link backstore into tcm_loop lun ", link);
            return 0;
        });
        if (ret == 0)
            resolve_node();
        return ret;
    }

    // teardown reverse of attach_lun: rm symlink -> rmdir lun_0 -> rmdir tpgt_1
    // -> rmdir <wwn>. The lun/ dir cannot be rmdir'd directly (it goes with tpgt_1).
    void detach_lun() {
        char tpgt[352], lun0[384], link[512];   // same chain as attach_lun
        snprintf(tpgt, sizeof(tpgt), "%s/tpgt_1", lb_path);
        snprintf(lun0, sizeof(lun0), "%s/lun/lun_0", tpgt);
        snprintf(link, sizeof(link), "%s/%s", lun0, bs_name);
        // Unlinking the LUN tears down the scsi device, and the kernel's disk
        // remove path issues SYNCHRONIZE CACHE / START STOP UNIT that the pump
        // must answer, so keep the teardown off the vcpu like attach_lun. Pump is alive
        // here (shutdown/rollback both call detach_lun before server.stop).
        run_off_vcpu([&]() -> int {
            ::unlink(link);
            ::rmdir(lun0);
            ::rmdir(tpgt);
            ::rmdir(lb_path);
            return 0;
        });
        lun_attached = false;
        node_path[0] = '\0';   // the scsi device went with the LUN link
    }

    // ----- rollback: unwind to the virgin state so start() is retryable -----

    void rollback() {
        if (lun_attached)
            detach_lun();
        if (server.pump_th || server.uio.fd >= 0)
            server.stop(true);
        if (created)
            destroy_backstore();
        release_lock();
        created = false;
        bs_path[0] = '\0';
        // virgin state includes backend ownership: a failed start leaves the
        // backend with the caller (else the destructor and the caller's own
        // cleanup would both delete it)
        backend = nullptr;
        own_backend = false;
    }
};

// ----------------------------------------------------------------------------
// TcmuHBAImpl: one configfs HBA (blk.h has the API). A dedicated LISTENER
// vcpu owns the genetlink subscription and does one of two things with an event:
//   - it belongs to a device of this HBA: answer the kernel itself and do
//     NOT queue it. These are the self-inflicted ones -- our own `echo 1 >
//     enable` in start() and our own resize() dev_size write both block the
//     CALLING vcpu inside a configfs write, so nothing on that vcpu could ever
//     send the *_DONE that unblocks it.
//   - otherwise: queue it for wait_for_event(). The caller maps dev_config to a
//     backend, serves it through new_device(), and answers the kernel from there
//     (start/detach/shutdown/resize), so no callback and no resolver is needed.
// Two genl sockets on this vcpu: lsk only listens (subscribed), rsk only
// transacts (SET_FEATURES and the auto-answers) -- a reply transact on the
// listener could swallow a co-arriving notification, because transact() drops
// messages with a non-matching sequence number. Device-side answers use their
// own short-lived socket (tcmu_send_done), so a device may run on any vcpu.
// ----------------------------------------------------------------------------

struct TcmuHBAImpl : TcmuHBA {
    // Field order is padding-driven, do not tidy it: the wide members pack from
    // the vptr and the odd-sized ones (fam, state, the five bools, q_lock) trail
    // them, so that q_lock's single byte is all `q` has to skip. Interleaved as
    // before, fam stranded 2 bytes, stop_req 3 before the handshake semaphores,
    // draining 6 before reg and q_lock 7 before q. 616 bytes vs 632.
    // log as (const char*), never VALUE(): alog would emit all 32 bytes
    char subtype[32] = "user_0";  // the configfs HBA dir under target/core/
    char hbanum[32] = {};         // subtype after "user_"
    // The HBA's one and only scope: list_orphans() and on_added() probe it, and
    // the devices new_device() builds claim their tombstones in it. Empty =
    // /run/photon-blk. new_tcmu_hba() bounds it, so the copy below cannot truncate.
    char lock_dir[SCOPE_DIR_BUF] = {};
    std::thread vcpu_thread;
    // the listener vcpu's two handshakes, one signal each: it publishes state
    // then signals started, and signals exited as its very last act -- after
    // vcpu_fini, from a plain std thread, which semaphore::signal supports
    photon::semaphore started_sem{0}, exited_sem{0};
    // the device registry and the ADDED hand-over table; both vcpus touch it
    TcmuRegistry reg;
    int fam = 0;                // TCM-USER family id, valid process-wide
    std::atomic<int> state{0};                  // 0 starting, 1 running, <0 = -errno
    bool netlink_reply = false;   // engage the kernel's module-global reply protocol
    bool defensive_reply = false; // answer our own HBA's events even though we did
                                  // NOT engage that protocol -- the only protection
                                  // on v4.13/v4.14, which lack the per-backstore
                                  // nl_reply_supported opt-out we otherwise use
    std::atomic<bool> stop_req{false};
    bool reply_mode = false;                    // we raised the kernel global; restore it
    bool draining = false;                      // listener loop exited: refuse, do not queue

    // events queued for wait_for_event(): produced on this vcpu, consumed on the
    // caller's (photon::semaphore signals across vcpus, one signal per event)
    photon::spinlock q_lock;
    std::deque<TcmuHBA::Event> q;
    photon::semaphore q_sem;

    TcmuHBAImpl(const char* st, bool reply, const char* ld, bool defensive)
        : netlink_reply(reply), defensive_reply(defensive) {
        if (!st || !*st)
            st = "user_0";
        // an over-long subtype leaves hbanum empty, which start() rejects: a
        // truncated copy would silently claim a different HBA directory
        if (snprintf(subtype, sizeof(subtype), "%s", st) < (int)sizeof(subtype) &&
            strncmp(subtype, "user_", 5) == 0 && subtype[5])
            strcpy(hbanum, subtype + 5);
        if (ld)
            snprintf(lock_dir, sizeof(lock_dir), "%s", ld);
    }

    // Devices are the caller's, not ours: they outlive nothing here, but a device
    // that outlives its HBA must not call into it, so orphan them all.
    ~TcmuHBAImpl() {
        stop();
        SCOPED_LOCK(reg.reg_lock);
        for (auto& kv : reg.devs)
            TcmuDeviceImpl::orphan(kv.second);
        reg.devs.clear();
        reg.pending_added.clear();   // their dev_ids die with this subscription
    }

    // ----- TcmuHBA API (caller's vcpu) -----

    int wait_for_event(TcmuHBA::Event* out, Timeout tmo) override {
        if (!out)
            LOG_ERROR_RETURN(EINVAL, -1, "tcmu wait_for_event needs an output event");
        if (q_sem.wait_interruptible(1, tmo) < 0)
            LOG_ERRNO_RETURN(0, -1, "no tcmu event");   // ETIMEDOUT on expiry, EINTR if interrupted
        SCOPED_LOCK(q_lock);
        if (q.empty())
            LOG_ERROR_RETURN(ENOENT, -1, "tcmu event queue is empty after a wakeup");
        *out = q.front();
        q.pop_front();
        return 0;
    }

    // Enumerate this HBA's orphaned devices: a photon backstore registration
    // exists under the HBA and no live server holds its flock. The returned
    // identity is the recovery key. features cannot be reconstructed from
    // configfs and is left 0 -- the recovering caller supplies the backend's
    // real feature set.
    std::vector<BlkDevInfo> list_orphans() override {
        std::vector<BlkDevInfo> ret;
        char hba[384];   // TARGET_ROOT/core/<subtype>
        snprintf(hba, sizeof(hba), "%s/core/%s", TARGET_ROOT, subtype);
        DIR* hd = opendir(hba);
        if (!hd)
            return ret;   // target not mounted, or no such HBA: no orphans
        DEFER(closedir(hd));
        struct dirent* be;
        while ((be = readdir(hd))) {
            if (be->d_name[0] == '.') continue;
            char bs[640];   // hba/<d_name>
            snprintf(bs, sizeof(bs), "%s/%s", hba, be->d_name);
            struct stat st;
            if (::stat(bs, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            char ap[672], p[704];   // bs/attrib, ap/<attrib file>
            snprintf(ap, sizeof(ap), "%s/attrib", bs);
            snprintf(p, sizeof(p), "%s/dev_config", ap);
            char b[256];
            if (cfg_read(p, b, sizeof(b)) < 0) continue;
            if (strncmp(b, "photon/", 7) != 0) continue;   // an operator's device, not ours
            const char* identity = b + 7;
            // probe the flock: free => no live server => orphan. Read-only open,
            // no O_CREAT: a query must not create files. A real photon orphan
            // always has its lock file (acquire_lock precedes create_backstore in
            // start()); a missing one is an inconsistent state, so skip it.
            char name[80];
            TcmuDeviceImpl::lock_file_name(identity, name, sizeof(name));
            if (devlock_free(lock_dir, name) != 1) continue;
            BlkDevInfo info;
            info.identity = identity;
            snprintf(p, sizeof(p), "%s/dev_size", ap);
            info.size = cfg_read_u64(p, 0);
            snprintf(p, sizeof(p), "%s/hw_block_size", ap);
            uint64_t hbs = cfg_read_u64(p, 512);
            info.sector_size_shift = (uint8_t)(hbs ? __builtin_ctzll(hbs) : 9);
            info.features = 0;
            ret.push_back(info);
        }
        return ret;
    }

    IBlkDevice* new_device(const TcmuHBA::Config& cfg) override {
        if (TcmuDeviceImpl::validate(cfg) < 0)
            return nullptr;
        return new TcmuDeviceImpl(&reg, fam, subtype, netlink_reply, lock_dir, cfg);
    }

    int deny(const TcmuHBA::Event& ev, int err) override {
        uint8_t done = tcmu_done_cmd(ev.kind);
        if (!done)
            LOG_ERROR_RETURN(EINVAL, -1, "bad tcmu event kind ", (int)ev.kind);
        // a refused ADDED must not stay claimable: a device built later for the
        // same name would answer it a second time, and the kernel may have
        // recycled that dev_id onto another backstore by then
        if (ev.kind == TcmuHBA::EventKind::ADDED)
            reg.forget_added(ev.bs_name, ev.dev_id);
        if (auto l = reg.find_link(ev.bs_name)) {
            // the device must not answer an event we just refused
            if (ev.kind == TcmuHBA::EventKind::ADDED && l->added_id.load() == ev.dev_id)
                l->added_id = 0;
            if (ev.kind == TcmuHBA::EventKind::RECONFIG && l->reconfig_id.load() == ev.dev_id) {
                l->reconfig_id = 0;
                l->pending_size = 0;
            }
        }
        return tcmu_send_done(fam, done, ev.dev_id, err ? -abs(err) : -ENOSYS);
    }

    // ----- queue -----

    void queue(const TcmuHBA::Event& ev) {
        {
            SCOPED_LOCK(q_lock);
            q.push_back(ev);
        }
        q_sem.signal(1);   // wakes the consumer on its own vcpu
    }

    // Hand an event to the caller -- unless we are draining: after the listen
    // loop exits nobody will ever consume the queue, so refuse it instead. A
    // queued-but-unread event would leave the operator's configfs write blocked
    // forever (recoverable only via reset_netlink), while -ENOSYS fails it
    // cleanly and the operator can retry against a successor.
    bool deliver(GenlSock& rsk, const TcmuHBA::Event& ev) {
        if (draining) {
            reply_done(rsk, fam, tcmu_done_cmd(ev.kind), ev.dev_id, -ENOSYS);
            return false;
        }
        queue(ev);
        return true;
    }

    static uint8_t tcmu_done_cmd(TcmuHBA::EventKind kind) {
        switch (kind) {
        case TcmuHBA::EventKind::ADDED:    return TCMU_CMD_ADDED_DEVICE_DONE;
        case TcmuHBA::EventKind::REMOVED:  return TCMU_CMD_REMOVED_DEVICE_DONE;
        case TcmuHBA::EventKind::RECONFIG: return TCMU_CMD_RECONFIG_DEVICE_DONE;
        }
        return 0;
    }

    // ----- lifecycle -----

    // 0 on success, -errno on failure (already logged), with the vcpu joined so
    // that the caller may safely delete this object
    int start() {
        if (!hbanum[0])
            LOG_ERROR_RETURN(EINVAL, -EINVAL, "tcmu subtype ` must be a configfs HBA name like user_0", subtype);
        vcpu_thread = std::thread([this] { vcpu_main(); });
        started_sem.wait(1);
        int st = state.load();
        if (st < 0) {
            join();   // already exited: instant
            LOG_ERROR_RETURN(-st, st, "failed to start the tcmu HBA's listener vcpu");
        }
        LOG_INFO("tcmu HBA started, ", make_named_value("subtype", (const char*)subtype),
                 "netlink_reply=", (int)netlink_reply, "defensive_reply=", (int)defensive_reply);
        return 0;
    }

    void stop() {
        stop_req = true;
        join();
    }

    void join() {
        if (!vcpu_thread.joinable())
            return;   // never spawned (start() failed early), or already joined
        exited_sem.wait(1);   // already exited: the join itself is instant
        vcpu_thread.join();
    }

    void vcpu_main() {
        DEFER(exited_sem.signal(1));   // registered first, so it runs last
        photon::vcpu_init();
        DEFER(photon::vcpu_fini());
        if (photon::fd_events_init(photon::INIT_EVENT_EPOLL) < 0) {
            int e = errno ? errno : EIO;
            LOG_ERROR("failed to init the tcmu HBA's event engine");
            return publish(-e);
        }
        DEFER(photon::fd_events_fini());

        GenlSock lsk, rsk;
        if (lsk.sk < 0 || rsk.sk < 0) {
            int e = errno ? errno : EIO;
            LOG_ERROR("failed to create the tcmu genetlink sockets, ", ERRNO());
            return publish(-e);
        }
        fam = lsk.resolve_family(TCMU_GENL_FAMILY);
        if (fam < 0) { publish_errno(); return; }
        int grp = lsk.resolve_mcast_group(TCMU_GENL_FAMILY, TCMU_MCGRP_CONFIG);
        if (grp < 0) { publish_errno(); return; }
        if (lsk.tune_for_notifications() < 0 || lsk.subscribe((uint32_t)grp) < 0) {
            publish_errno();
            return;
        }
        // the reply protocol is module-global; engaging it makes every tcmu
        // configure/remove/reconfig on the host wait for a *_DONE (see blk.h)
        if (netlink_reply) {
            if (set_reply_supported(rsk, fam, 1) < 0) { publish_errno(); return; }
            reply_mode = true;
        }
        // subscribe BEFORE the scan: an ADDED racing the scan lands in the socket
        // buffer, and the flock probe keeps a double report benign
        initial_scan();
        publish(1);

        while (!stop_req.load(std::memory_order_acquire)) {
            if (photon::wait_for_fd_readable(lsk.sk, 50 * 1000) < 0) {
                if (errno == ETIMEDOUT)
                    continue;   // the stop tick
                LOG_ERROR("tcmu HBA netlink wait failed, ", ERRNO());
                break;
            }
            int n = lsk.recv_notifications([&](uint16_t f, uint8_t cmd, const char* a, size_t al) {
                if (f == (uint16_t)fam)
                    on_event(rsk, cmd, a, al);
            });
            if (n < 0)
                LOG_ERROR("tcmu HBA netlink recv failed, ", ERRNO());
        }

        if (reply_mode) {
            // unblock any operator write whose event armed a kernel wait just
            // before we stopped; the wait has no timeout, so reply first. Nothing
            // will consume the queue any more, so draining also refuses events.
            draining = true;
            drain_replies(lsk, rsk, fam);
            if (set_reply_supported(rsk, fam, 0) < 0)
                LOG_WARN("failed to restore tcmu netlink reply support, ", ERRNO());
        }
    }

    // publish the outcome and wake start(): one signal per start() attempt
    void publish(int st) {
        state = st;
        started_sem.signal(1);
    }

    void publish_errno() {
        publish(-(errno ? errno : EIO));   // the failure was logged where detected
    }

    int set_reply_supported(GenlSock& rsk, int fam, uint8_t v) {
        char attrs[16], sink[64];
        size_t off = nla_append(attrs, 0, sizeof(attrs), TCMU_ATTR_SUPP_KERN_CMD_REPLY, &v, 1);
        if (!off || rsk.transact(fam, TCMU_CMD_SET_FEATURES, attrs, off, sink, sizeof(sink)) < 0)
            // an unknown SET_FEATURES command means a kernel without the reply
            // protocol at all; netlink_reply is unavailable there, not optional
            LOG_ERROR_RETURN(errno ? errno : EIO, -1,
                             "tcmu SET_FEATURES supp_kern_cmd_reply=` failed (the netlink "
                             "reply protocol needs kernel v4.13+; pass netlink_reply=false)", (int)v);
        return 0;
    }

    // Send one *_DONE (CMD_STATUS s32 + DEVICE_ID u32 of the event answered). A
    // reply the kernel is not waiting for is benign but NOT free: it finds no
    // armed command, answers the ack with -ENODEV and logs a pr_err on its side
    // ("could not find device with dev id N"), which is why the two wrappers
    // below are gated instead of this being called unconditionally.
    void send_done(GenlSock& rsk, int fam, uint8_t done_cmd, uint32_t dev_id, int32_t status) {
        char attrs[64], sink[64];
        size_t off = nla_append(attrs, 0, sizeof(attrs), TCMU_ATTR_CMD_STATUS, &status, sizeof(status));
        off += nla_append_u32(attrs, off, sizeof(attrs), TCMU_ATTR_DEVICE_ID, dev_id);
        ssize_t r = rsk.transact(fam, done_cmd, attrs, off, sink, sizeof(sink));
        if (r < 0)
            LOG_DEBUG("tcmu DONE reply cmd=` dev=` status=` got `", (int)done_cmd, dev_id, status, (int)r);
    }

    // Answer an event of OUR OWN HBA. The kernel arms its wait whenever the
    // module-global reply flag is up, and ANOTHER daemon may have raised it --
    // the flag has no getter, so we cannot tell. We answer when we raised it
    // ourselves, or when the caller chose the defensive policy; on v4.15+ our own
    // backstores opt out per device instead (create_backstore writes
    // nl_reply_supported=-1), so no wait is ever armed for them and this stays
    // silent. Consequences of each choice are in blk.h.
    void reply_done(GenlSock& rsk, int fam, uint8_t done_cmd, uint32_t dev_id, int32_t status) {
        if (!reply_mode && !defensive_reply)
            return;
        send_done(rsk, fam, done_cmd, dev_id, status);
    }

    // Refuse an event of ANOTHER HBA. Only an instance that engaged the protocol
    // is the host's tcmu authority, so only it may do this: a merely defensive
    // one does not serve those devices, and its -ENOSYS could fail an operator's
    // write that their own daemon was about to accept.
    void deny_foreign(GenlSock& rsk, int fam, uint8_t done_cmd, uint32_t dev_id) {
        if (!reply_mode)
            return;
        send_done(rsk, fam, done_cmd, dev_id, -ENOSYS);
    }

    // drain whatever events remain after the listen loop exited and answer the
    // ones that could be blocking an operator write: events for devices that are
    // not ours are answered here, everything else deliver() refuses (draining)
    void drain_replies(GenlSock& lsk, GenlSock& rsk, int fam) {
        for (int i = 0; i < 50; i++) {   // ~50ms grace for in-transit events
            int n = lsk.recv_notifications([&](uint16_t f, uint8_t cmd, const char* a, size_t al) {
                if (f == (uint16_t)fam)
                    on_event(rsk, cmd, a, al);
            });
            if (n <= 0 && i >= 4)   // a few empty polls: nothing in flight
                break;
            photon::thread_usleep(1000);
        }
    }

    // ----- event handlers (listener vcpu only) -----

    static bool to_kind(uint8_t cmd, TcmuHBA::EventKind* k) {
        switch (cmd) {
        case TCMU_CMD_ADDED_DEVICE:    *k = TcmuHBA::EventKind::ADDED;    return true;
        case TCMU_CMD_REMOVED_DEVICE:  *k = TcmuHBA::EventKind::REMOVED;  return true;
        case TCMU_CMD_RECONFIG_DEVICE: *k = TcmuHBA::EventKind::RECONFIG; return true;
        }
        return false;
    }

    void on_event(GenlSock& rsk, uint8_t cmd, const char* attrs, size_t alen) {
        TcmuHBA::EventKind kind;
        if (!to_kind(cmd, &kind))
            return;   // not a device event
        auto dev_name = (const char*)nla_find(attrs, alen, TCMU_ATTR_DEVICE, nullptr);
        uint32_t minor = 0, dev_id = 0;
        get_u32(attrs, alen, TCMU_ATTR_MINOR, &minor);
        get_u32(attrs, alen, TCMU_ATTR_DEVICE_ID, &dev_id);
        if (!dev_name) {
            LOG_WARN("tcmu event cmd=` lacks the device name", (int)cmd);
            return;
        }
        char hban[32], bs[256];
        const char* dev_cfg = nullptr;
        if (!split_uio_name(dev_name, hban, sizeof(hban), bs, sizeof(bs), dev_cfg)) {
            LOG_WARN("tcmu event cmd=` has an unparsable device name `", (int)cmd, dev_name);
            return;
        }
        if (strcmp(hban, hbanum) != 0) {
            // Only an instance that engaged the reply protocol answers for the
            // whole host, and it must refuse what is not ours -- otherwise the
            // operator's enable would succeed and leave a device nobody serves.
            // Anyone else ignores it: that HBA's own daemon will answer.
            if (reply_mode) {
                LOG_WARN("tcmu device ` is on HBA user_`, not ours (user_`); refusing", bs, hban, hbanum);
                deny_foreign(rsk, fam, tcmu_done_cmd(kind), dev_id);
            } else {
                LOG_DEBUG("tcmu device ` is on HBA user_`, not ours (user_`); ignoring", bs, hban, hbanum);
            }
            return;
        }
        switch (kind) {
        case TcmuHBA::EventKind::ADDED:    on_added(rsk, bs, dev_cfg, minor, dev_id); break;
        case TcmuHBA::EventKind::REMOVED:  on_removed(rsk, bs, dev_id); break;
        case TcmuHBA::EventKind::RECONFIG: on_reconfig(rsk, bs, dev_id, attrs, alen); break;
        }
    }

    void on_added(GenlSock& rsk, const char* bs, const char* dev_cfg,
                  uint32_t minor, uint32_t dev_id) {
        auto mine = reg.find_link(bs);
        if (mine && (mine->starting.load() || mine->serving.load())) {
            // ours, and a ring is on its way up: start() is blocked in `echo 1 >
            // enable` on the caller's vcpu, so nothing there could ever send the
            // reply that unblocks it. A device that is merely CONSTRUCTED does not
            // qualify -- it registers from birth, and answering for one would
            // acknowledge a backstore nobody serves, so fall through and let the
            // caller decide.
            reply_done(rsk, fam, TCMU_CMD_ADDED_DEVICE_DONE, dev_id, 0);
            return;
        }
        char lock[80];
        TcmuDeviceImpl::lock_file_name(bs, lock, sizeof(lock));
        if (devlock_free(lock_dir, lock) == 0) {
            // another PROCESS serves it: our own start() is covered by the
            // starting flag above, which it raises before taking this lock. That
            // server serves the ring itself, and the kernel only needs an answer
            // to unblock the operator
            LOG_INFO("tcmu device ` is served by a live server; staying out", bs);
            reply_done(rsk, fam, TCMU_CMD_ADDED_DEVICE_DONE, dev_id, 0);
            return;
        }
        TcmuHBA::Event ev{};
        ev.kind = TcmuHBA::EventKind::ADDED;
        ev.dev_id = dev_id;
        snprintf(ev.bs_name, sizeof(ev.bs_name), "%s", bs);
        snprintf(ev.dev_config, sizeof(ev.dev_config), "%s", dev_cfg);
        snprintf(ev.uio_node, sizeof(ev.uio_node), "/dev/uio%u", minor);
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/core/%s/%s/attrib/dev_size", TARGET_ROOT, subtype, bs);
        ev.size = cfg_read_u64(p, 0);
        // Arm the hand-over BEFORE queueing: the consumer runs on another vcpu and
        // may call new_device() the instant the event is delivered, and this
        // dev_id exists nowhere else for it to be learned from.
        bool armed = reg.remember_added(bs, dev_id);
        if (!deliver(rsk, ev)) {
            if (armed)
                reg.forget_added(bs, dev_id);   // refused while draining: no device will follow
            return;
        }
        LOG_INFO("tcmu backstore ` enabled, dev_config=`, waiting to be served", bs, dev_cfg);
    }

    void on_removed(GenlSock& rsk, const char* bs, uint32_t dev_id) {
        // The registration is going away, so an ADDED still waiting to be claimed
        // named a backstore that is about to stop existing -- and its dev_id is
        // about to be free for the kernel to hand to another one.
        reg.forget_added(bs, 0);
        if (auto l = reg.find_link(bs)) {
            if (!l->serving.load()) {
                // Nothing to stop: either the device is detached, or this REMOVED
                // was fired by our own shutdown()/rollback() -- whose configfs
                // write is blocked on the caller's vcpu and, in reply mode, could
                // never answer itself. So answer here.
                reply_done(rsk, fam, TCMU_CMD_REMOVED_DEVICE_DONE, dev_id, 0);
                return;
            }
            TcmuHBA::Event ev{};
            ev.kind = TcmuHBA::EventKind::REMOVED;
            ev.dev_id = dev_id;
            snprintf(ev.bs_name, sizeof(ev.bs_name), "%s", bs);
            // arm the device BEFORE queueing: the consumer runs on another vcpu
            // and may call shutdown() the instant the event is delivered. The
            // device answers once it has stopped serving, because the kernel
            // unregisters the uio right after its wait and our mmap must be gone.
            l->removed_id = dev_id;
            if (!deliver(rsk, ev))
                l->removed_id = 0;   // refused while draining: leave it to its owner
            return;
        }
        reply_done(rsk, fam, TCMU_CMD_REMOVED_DEVICE_DONE, dev_id, 0);   // not ours
    }

    void on_reconfig(GenlSock& rsk, const char* bs, uint32_t dev_id,
                     const char* attrs, size_t alen) {
        auto l = reg.find_link(bs);
        if (!l) {   // not ours: never make an operator wait on a device we do not serve
            reply_done(rsk, fam, TCMU_CMD_RECONFIG_DEVICE_DONE, dev_id, 0);
            return;
        }
        TcmuHBA::Event ev{};
        ev.kind = TcmuHBA::EventKind::RECONFIG;
        ev.dev_id = dev_id;
        snprintf(ev.bs_name, sizeof(ev.bs_name), "%s", bs);
        size_t pl = 0;
        auto p = nla_find(attrs, alen, TCMU_ATTR_DEV_SIZE, &pl);
        if (p && pl >= sizeof(ev.size)) {
            // the event carries the NEW size: the committing store runs only
            // after the reply, so the attrib still reads the old one
            memcpy(&ev.size, p, sizeof(ev.size));
            snprintf(ev.attr, sizeof(ev.attr), "dev_size");
            if (ev.size == l->self_size.load()) {
                // our own resize() is blocked in that very write on the caller's
                // vcpu: answer for it and do not report it back at ourselves
                reply_done(rsk, fam, TCMU_CMD_RECONFIG_DEVICE_DONE, dev_id, 0);
                return;
            }
            // arm resize() to answer it BEFORE queueing: the consumer runs on
            // another vcpu and may call resize() the instant it gets the event
            l->pending_size = ev.size;
            l->reconfig_id = dev_id;
            if (!deliver(rsk, ev)) {
                l->reconfig_id = 0;
                l->pending_size = 0;
                return;
            }
        } else if ((p = nla_find(attrs, alen, TCMU_ATTR_DEV_CFG, &pl))) {
            // no device method can answer this one: the caller must deny() it
            snprintf(ev.attr, sizeof(ev.attr), "dev_config");
            snprintf(ev.dev_config, sizeof(ev.dev_config), "%.*s", (int)pl, (const char*)p);
            if (!deliver(rsk, ev))
                return;
        } else {
            LOG_WARN("tcmu RECONFIG of ` carries neither dev_size nor dev_config", bs);
            reply_done(rsk, fam, TCMU_CMD_RECONFIG_DEVICE_DONE, dev_id, 0);
            return;
        }
        LOG_INFO("tcmu device ` reconfig `, size `, queued for its handler", bs, ev.attr, ev.size);
    }

    // ----- the startup scan (listener vcpu) -----

    // Synthesize ADDED events for backstores configured before we subscribed
    // (crash recovery), so one event loop covers both the backlog and whatever
    // arrives later. No DONE is owed -- that configure completed already, or
    // hung in a way nobody can answer, because the dev_id only ever existed in
    // the missed event (see blk.h on reset_netlink) -- hence dev_id stays 0 and
    // the event is marked synthesized. Backstores a live server holds are
    // skipped: there is nothing to hand over.
    void initial_scan() {
        char hba_dir[128];   // TARGET_ROOT/core/<subtype>
        snprintf(hba_dir, sizeof(hba_dir), "%s/core/%s", TARGET_ROOT, subtype);
        DIR* hd = opendir(hba_dir);
        if (!hd)
            return;
        DEFER(closedir(hd));
        struct dirent* e;
        while ((e = readdir(hd))) {
            if (e->d_name[0] == '.')
                continue;
            char bs_path[384], p[PATH_MAX];   // hba_dir/<d_name>
            snprintf(bs_path, sizeof(bs_path), "%s/%s", hba_dir, e->d_name);
            struct stat st;   // the HBA dir also carries attribute FILES
            if (::stat(bs_path, &st) != 0 || !S_ISDIR(st.st_mode))
                continue;     // (hba_info/hba_mode): skip them
            snprintf(p, sizeof(p), "%s/enable", bs_path);
            if (cfg_read_u64(p, 0) != 1)
                continue;   // not configured (or a configure is mid-flight; its
                            // ADDED is in the socket buffer and will be handled)
            char lock[80];
            TcmuDeviceImpl::lock_file_name(e->d_name, lock, sizeof(lock));
            if (devlock_free(lock_dir, lock) == 0)
                continue;   // a live server holds it
            char dc[256], node[64];
            snprintf(p, sizeof(p), "%s/attrib/dev_config", bs_path);
            if (cfg_read(p, dc, sizeof(dc)) < 0)
                continue;
            TcmuHBA::Event ev{};
            ev.kind = TcmuHBA::EventKind::ADDED;
            ev.synthesized = true;
            snprintf(ev.bs_name, sizeof(ev.bs_name), "%s", e->d_name);
            snprintf(ev.dev_config, sizeof(ev.dev_config), "%s", dc);
            snprintf(p, sizeof(p), "%s/attrib/dev_size", bs_path);
            ev.size = cfg_read_u64(p, 0);
            if (TcmuUio::find(hbanum, e->d_name, node, sizeof(node)) == 0)
                snprintf(ev.uio_node, sizeof(ev.uio_node), "%s", node);
            queue(ev);
            LOG_INFO("tcmu scan found unserved backstore `, dev_config=", e->d_name, dc);
        }
    }

    // uio name = "tcm-user/<hbanum>/<bs>[/<dev_config>]"; dev_config may itself
    // contain '/', so split only the first three fields. dev_cfg points into
    // dev_name (a NUL-terminated nla string).
    static bool split_uio_name(const char* name, char* hbanum, size_t hcap,
                               char* bs, size_t bcap, const char*& dev_cfg) {
        static const char PFX[] = "tcm-user/";
        if (strncmp(name, PFX, sizeof(PFX) - 1) != 0)
            return false;
        const char* hb = name + sizeof(PFX) - 1;
        const char* p1 = strchr(hb, '/');
        if (!p1 || (size_t)(p1 - hb) >= hcap)
            return false;
        memcpy(hbanum, hb, p1 - hb);
        hbanum[p1 - hb] = '\0';
        const char* p2 = strchr(p1 + 1, '/');
        size_t bl = p2 ? (size_t)(p2 - p1 - 1) : strlen(p1 + 1);
        if (!bl || bl >= bcap)
            return false;
        memcpy(bs, p1 + 1, bl);
        bs[bl] = '\0';
        dev_cfg = p2 ? p2 + 1 : "";
        return true;
    }

    static void get_u32(const char* attrs, size_t alen, uint16_t type, uint32_t* out) {
        size_t pl = 0;
        if (auto p = nla_find(attrs, alen, type, &pl); p && pl >= sizeof(uint32_t))
            memcpy(out, p, sizeof(*out));
    }
};

TcmuHBA* new_tcmu_hba(const char* subtype, bool netlink_reply, const char* lock_dir,
                      bool defensive_reply) {
    // Before anything else: a truncated copy would be a DIFFERENT directory, and
    // this one scopes both the orphan scan and every device's tombstone claim
    if (validate_scope_dir(lock_dir) < 0)
        return nullptr;   // already logged
    auto sys = new TcmuHBAImpl(subtype, netlink_reply, lock_dir, defensive_reply);
    if (sys->start() < 0) {   // errno set and logged inside, the vcpu already joined
        delete sys;
        return nullptr;
    }
    return sys;
}

}  // namespace blk
}  // namespace photon
