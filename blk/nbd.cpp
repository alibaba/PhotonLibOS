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

// NBD transport. Serves a photon IFile over the Network Block Device protocol
// on a unix socket (unix_path) and/or a TCP endpoint (enable_tcp); with
// cfg.loopback_device it also plays the nbd-client role itself and attaches
// the export to a free /dev/nbdN, so local consumers get a kernel block
// device node. The three endpoints are independent and at least one must be
// enabled. The loopback connection is a socketpair: the kernel nbd driver
// performs no NBD negotiation and expects a socket already in the
// transmission phase, which a brand-new socketpair trivially is.
//
// Concurrency: conns, workers, Conn::in_flight and stopping are guarded -- a spinlock,
// a second spinlock and two atomics -- so that serve_conn can run on a different vcpu
// from accept_loop and the API calls, which is what BlkConfig::pool is for. They used
// to need no guard, and three separate comments said so, on the strength of every
// serving coroutine sharing the vcpu that called start(). What still needs no guard is
// noted where it is relied on (depth/bytes, Conn::wlock).

#include "blk.h"
#include "utils.h"

#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/utility.h>
#include <photon/io/fd-events.h>
#include <photon/net/basic_socket.h>
#include <photon/net/socket.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>

#include <sys/socket.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <linux/genetlink.h>
#include <linux/nbd-netlink.h>
#include <linux/nbd.h>
#include <linux/netlink.h>
#include <sys/ioctl.h>
#endif

namespace photon {
namespace blk {

// NBD protocol constants
// (github.com/NetworkBlockDevice/nbd/blob/master/doc/proto.md). All on-wire
// integers are big-endian; the protocol structs' encode()/decode() convert
// them with the __builtin_bswap family (little-endian hosts only).

static constexpr uint64_t NBD_INIT_MAGIC     = 0x4e42444d41474943ull;  // "NBDMAGIC"
static constexpr uint64_t NBD_OPTS_MAGIC     = 0x49484156454f5054ull;  // "IHAVEOPT"
static constexpr uint64_t NBD_REP_MAGIC      = 0x0003e889045565a9ull;
static constexpr uint32_t NBD_REQ_MAGIC      = 0x25609513;
static constexpr uint32_t NBD_SIMPLE_REP_MAGIC = 0x67446698;

// server handshake flags
static constexpr uint16_t NBD_FLAG_FIXED_NEWSTYLE = 1u << 0;
static constexpr uint16_t NBD_FLAG_NO_ZEROES      = 1u << 1;
// client handshake flags
static constexpr uint32_t NBD_FLAG_C_FIXED_NEWSTYLE = 1u << 0;
static constexpr uint32_t NBD_FLAG_C_NO_ZEROES      = 1u << 1;
// options
static constexpr uint32_t NBD_OPT_EXPORT_NAME      = 1;
static constexpr uint32_t NBD_OPT_ABORT            = 2;
static constexpr uint32_t NBD_OPT_LIST             = 3;
static constexpr uint32_t NBD_OPT_INFO             = 6;
static constexpr uint32_t NBD_OPT_GO               = 7;
// option replies
static constexpr uint32_t NBD_REP_LIST     = 2;
static constexpr uint32_t NBD_REP_INFO     = 3;
static constexpr uint32_t NBD_REP_ACK      = 1;
static constexpr uint32_t NBD_REP_ERR_UNSUP = (1u << 31) | 1;
static constexpr uint32_t NBD_REP_ERR_INVALID = (1u << 31) | 3;
static constexpr uint16_t NBD_INFO_EXPORT  = 0;
// transmission flags; renamed NBD_TRANS_* to avoid <linux/nbd.h> macros of the
// same NBD_FLAG_* names
static constexpr uint16_t NBD_TRANS_HAS_FLAGS         = 1u << 0;
static constexpr uint16_t NBD_TRANS_READ_ONLY         = 1u << 1;
static constexpr uint16_t NBD_TRANS_SEND_FLUSH        = 1u << 2;
static constexpr uint16_t NBD_TRANS_SEND_FUA          = 1u << 3;
static constexpr uint16_t NBD_TRANS_SEND_TRIM         = 1u << 5;
static constexpr uint16_t NBD_TRANS_SEND_WRITE_ZEROES = 1u << 6;
// commands
static constexpr uint16_t NBD_CMD_READ         = 0;
static constexpr uint16_t NBD_CMD_WRITE        = 1;
static constexpr uint16_t NBD_CMD_DISC         = 2;
static constexpr uint16_t NBD_CMD_FLUSH        = 3;
static constexpr uint16_t NBD_CMD_TRIM         = 4;
static constexpr uint16_t NBD_CMD_WRITE_ZEROES = 6;
// per-request command-flags field (16 bits at request offset 4). The kernel
// client packs these into the upper 16 bits of its 32-bit `type` word, so FUA
// lands on bit 0 here. Named NBD_REQ_* to avoid the <linux/nbd.h> macro.
static constexpr uint16_t NBD_REQ_FUA     = 1u << 0;
// errors
static constexpr uint32_t NBD_SUCCESS = 0;
static constexpr uint32_t NBD_EPERM   = 1;
static constexpr uint32_t NBD_EIO     = 5;
static constexpr uint32_t NBD_ENOMEM  = 12;
static constexpr uint32_t NBD_EINVAL  = 22;
static constexpr uint32_t NBD_ENOSPC  = 28;
static constexpr uint32_t NBD_ENOTSUP = 95;

static constexpr uint32_t DEFAULT_QUEUE_DEPTH = 128;
static constexpr uint32_t MAX_BLOCK_SIZE      = 32u << 20;
// Simultaneous client connections. queue_depth bounds in-flight REQUESTS, not
// connections: a client that connects and sends nothing costs nothing against
// it, so a TCP-facing export would otherwise be exhaustible by connect count
// alone (one serve_conn coroutine plus one Conn each).
static constexpr size_t   MAX_CONNECTIONS     = 1024;
// Bytes of request buffer outstanding at once. queue_depth alone bounds this
// only at queue_depth * MAX_BLOCK_SIZE = 4 GiB, which a deep queue of large
// sequential requests reaches legitimately rather than maliciously -- so gate on
// bytes as well and let the count gate handle concurrency.
static constexpr uint64_t MAX_INFLIGHT_BYTES  = 512ull << 20;
// The byte gate must be able to admit a single maximum-size request on its own,
// or that request waits forever for more tokens than the budget holds. Today
// 32 MiB < 512 MiB so byte_cost's clamp never binds; this is what stops a later
// raise of MAX_BLOCK_SIZE from silently inverting that.
static_assert(MAX_BLOCK_SIZE <= MAX_INFLIGHT_BYTES,
              "a single maximum-size nbd request must fit the whole byte budget");
static constexpr uint32_t MAX_OPTION_LEN      = 1u << 20;
// The stack of the coroutine whose only job is to join a finished connection
// coroutine (retire_self). It calls one function and blocks in it, so it needs
// none of the 8 MiB a serving coroutine gets for request buffers and backend
// call depth -- and one of these is created per connection ever made, which is
// exactly the count the reaping keeps from accumulating stacks.
static constexpr uint32_t REAPER_STACK_SIZE   = 64u << 10;

// Protocol structs, laid out exactly as the packed big-endian wire form
// (sizeof == wire size): streams read/write them whole, and encode()/decode()
// byte-swap the members in place. Runs on little-endian hosts only
// (x86_64/aarch64), where wire order is the byte-reverse of host order, so
// one bswap serves both directions

struct NbdGreeting {          // server -> client, 18 bytes
    uint64_t init_magic, opts_magic;
    uint16_t flags;
    explicit NbdGreeting(uint16_t flags)
        : init_magic(NBD_INIT_MAGIC), opts_magic(NBD_OPTS_MAGIC), flags(flags) {}
    void encode() {
        init_magic = __builtin_bswap64(init_magic);
        opts_magic = __builtin_bswap64(opts_magic);
        flags = __builtin_bswap16(flags);
    }
}__attribute__((packed));
static_assert(sizeof(NbdGreeting) == 18, "NBD greeting is 18 bytes on the wire");

struct NbdOptionHeader {      // client -> server, 16 bytes
    uint64_t magic;
    uint32_t opt, length;
    bool decode() {
        magic = __builtin_bswap64(magic);
        if (magic != NBD_OPTS_MAGIC)
            return false;
        opt = __builtin_bswap32(opt);
        length = __builtin_bswap32(length);
        return true;
    }
}__attribute__((packed));
static_assert(sizeof(NbdOptionHeader) == 16, "NBD option header is 16 bytes on the wire");

struct NbdOptionReply {       // server -> client option reply header, 20 bytes
    uint64_t magic;
    uint32_t opt, type, length;
    NbdOptionReply(uint32_t opt, uint32_t type, uint32_t length)
        : magic(NBD_REP_MAGIC), opt(opt), type(type), length(length) {}
    void encode() {
        magic = __builtin_bswap64(magic);
        opt = __builtin_bswap32(opt);
        type = __builtin_bswap32(type);
        length = __builtin_bswap32(length);
    }
}__attribute__((packed));
static_assert(sizeof(NbdOptionReply) == 20, "NBD option reply header is 20 bytes on the wire");

struct NbdExportMeta {        // export size + transmission flags, 10 bytes
    uint64_t size;
    uint16_t flags;
    void encode() {
        size = __builtin_bswap64(size);
        flags = __builtin_bswap16(flags);
    }
}__attribute__((packed));
static_assert(sizeof(NbdExportMeta) == 10, "NBD export meta is 10 bytes on the wire");

struct NbdExportInfo {        // NBD_REP_INFO payload of type NBD_INFO_EXPORT, 12 bytes
    uint16_t type;
    NbdExportMeta meta;
    void encode() {
        type = __builtin_bswap16(type);
        meta.encode();
    }
}__attribute__((packed));
static_assert(sizeof(NbdExportInfo) == 12, "NBD export info is 12 bytes on the wire");

struct NbdRequest {           // client -> server request header, 28 bytes
    uint32_t magic;
    uint16_t flags, type;
    uint64_t handle, offset;
    uint32_t length;
    bool decode() {
        magic = __builtin_bswap32(magic);
        if (magic != NBD_REQ_MAGIC)
            return false;
        flags = __builtin_bswap16(flags);
        type = __builtin_bswap16(type);
        handle = __builtin_bswap64(handle);
        offset = __builtin_bswap64(offset);
        length = __builtin_bswap32(length);
        return true;
    }
}__attribute__((packed));
static_assert(sizeof(NbdRequest) == 28, "NBD request header is 28 bytes on the wire");

struct NbdSimpleReply {       // server -> client reply header, 16 bytes
    uint32_t magic, error;
    uint64_t handle;
    NbdSimpleReply(uint32_t error, uint64_t handle)
        : magic(NBD_SIMPLE_REP_MAGIC), error(error), handle(handle) {}
    void encode() {
        magic = __builtin_bswap32(magic);
        error = __builtin_bswap32(error);
        handle = __builtin_bswap64(handle);
    }
}__attribute__((packed));
static_assert(sizeof(NbdSimpleReply) == 16, "NBD simple reply header is 16 bytes on the wire");

static uint32_t errno_to_nbd(int e) {
    switch (e) {
    case 0:       return NBD_SUCCESS;
    case EPERM:   return NBD_EPERM;
    case ENOMEM:  return NBD_ENOMEM;
    case EINVAL:  return NBD_EINVAL;
    case ENOSPC:  return NBD_ENOSPC;
    case ENOTSUP: return NBD_ENOTSUP;
    case ENOSYS:  return NBD_ENOTSUP;
    default:      return NBD_EIO;
    }
}

// Bounds the reads that must not wait forever. A photon socket stream applies its
// timeout to a read() as a whole -- one deadline for the entire count -- so a WRITE
// payload read under this guard either arrives or fails within `us`. skip_read is
// weaker and this says so rather than pretending otherwise: it loops over 1 KiB
// read()s, so what the guard bounds there is each chunk, and a client that dribbles
// a byte every `us` keeps it alive. That costs nothing where it is used with
// skip_read, because neither of those two sites holds a gate while draining.
// us == 0 means no deadline, and the stream's own timeout is restored rather than
// assumed to have been unlimited.
struct stall_guard {
    net::ISocketStream* s;
    uint64_t saved;
    bool armed;
    stall_guard(net::ISocketStream* stream, uint64_t us)
        : s(stream), saved(stream->timeout()), armed(us != 0) {
        if (armed)
            s->timeout(us);
    }
    ~stall_guard() {
        if (armed)
            s->timeout(saved);
    }
};

struct NbdDeviceImpl : NbdDevice {
    struct Conn {
        // The positional inits below ({s, negotiate}) pin the first two members:
        // reordering those two silently changes what the braces assign.
        net::ISocketStream* s;  // owned: accepted streams pass ownership to
                                // the caller, the loopback one is heap-made;
                                // serve_conn's teardown deletes it
        bool negotiate;         // false only for the loopback socketpair end,
                                // which starts already in transmission phase
        std::atomic<uint32_t> in_flight{0};
        photon::mutex wlock;    // serializes the reply writes of concurrent
                                // execute coroutines (header + data must not
                                // interleave). in_flight is atomic because
                                // detach(wait_pending) polls it on the caller's
                                // vcpu while the execute coroutines bump it on
                                // whichever vcpu serve_conn runs on.
    };

    NbdConfig cfg;
    fs::IFile* backend = nullptr;
    photon::semaphore depth;    // in-flight limit; photon::semaphore has no
                                // reset, and every detach() drains it back to
                                // full, so it is signaled once at the first
                                // start() and reused across restarts
    photon::semaphore bytes;    // buffer-memory limit, same seed-once lifecycle;
                                // counted in BYTES, taken after depth and
                                // released before it so the two gates are always
                                // acquired in one order and cannot deadlock

    // What a request of this type and length holds against MAX_INFLIGHT_BYTES.
    // Only READ and WRITE allocate a buffer. The clamp does not bind at today's
    // constants (MAX_BLOCK_SIZE < MAX_INFLIGHT_BYTES, pinned by the static_assert
    // above); it is here so that raising MAX_BLOCK_SIZE cannot silently produce a
    // request that waits for more tokens than the budget holds. serve_conn takes
    // it and execute()'s DEFER gives it back, so both call THIS -- the formula
    // must not drift.
    static uint64_t byte_cost(uint16_t type, uint32_t len) {
        if (type != NBD_CMD_READ && type != NBD_CMD_WRITE)
            return 0;
        return std::min<uint64_t>(len ? len : 1, MAX_INFLIGHT_BYTES);
    }

    net::ISocketServer* uds_server = nullptr;  // unix_path mode
    net::ISocketServer* tcp_server = nullptr;  // enable_tcp mode
    photon::thread* uds_accept_th = nullptr;
    photon::thread* tcp_accept_th = nullptr;
    std::vector<Conn*> conns;
    std::vector<photon::thread*> workers;  // the connection coroutines still to be
                                           // reaped; one leaves it as it finishes
                                           // (see retire_self), so this tracks LIVE
                                           // connections rather than every one ever
                                           // made -- a joinable photon thread keeps
                                           // its 8 MiB stack until somebody joins it
    bool own_backend = false;
    bool started = false;
    std::atomic<bool> stopping{false};
    // Connection coroutines that have not finished retiring. cleanup_runtime joins
    // the handles it took out of `workers` and then waits for this to reach zero,
    // which covers a worker that left the list on its own way out: after that
    // departure its only remaining touches of this object are the departure itself.
    std::atomic<uint32_t> live_workers{0};
    // Guards conns AND workers. A spinlock, not a photon::mutex: every critical
    // section is yield-free (the longest is a vector erase and a raw ::shutdown),
    // so no holder can be preempted while another vcpu spins.
    photon::spinlock conns_lock;
    uint16_t trans_flags = 0;
    uint32_t stack_size = DEFAULT_STACK_SIZE;   // copied from cfg at start()
    uint64_t stall_us = 0;                      // cfg.stall_timeout in us; 0 = none

#ifdef __linux__
    // Loopback-attach state. Attaching an nbd device is Linux-only -- ioctls on
    // /dev/nbdN, or the netlink CONNECT/DISCONNECT commands -- so on any other
    // platform none of this exists and get_device_node() answers nullptr.
    std::thread doit_thread;        // runs the kernel's DO_IT loop; legacy path only
    int nbd_fd = -1;                // the attached /dev/nbdN
    uint32_t nbd_index = 0;         // netlink-allocated device index
    bool loopback_netlink = false;  // attach path: netlink (no DO_IT) vs legacy ioctls
    // log as (const char*), never VALUE(): alog would emit all 64 bytes
    char loopback_node[64] = {};    // "/dev/nbdN" while attached; empty = detached
#endif

    explicit NbdDeviceImpl(const NbdConfig& c) : cfg(c) {
        // The effective half of the descriptor; blk.h documents each axis. nbd keeps
        // no queue across detach() -- the connections ARE the queue, and cleanup_runtime
        // closes them on both paths -- so there is no backlog to harvest and no
        // registration for a later start() to drift against. detach(false) still joins
        // workers that are waiting on backend I/O, which is why detach_no_wait is false.
        cfg.info.offered = FEATURE_FLUSH | FEATURE_DISCARD | FEATURE_WRITE_ZEROES;
        cfg.info.backlog = BlkBacklog::None;
        cfg.info.shutdown_refusal = BlkShutdownRefusal::Disconnects;
        cfg.info.resize_effect = BlkResizeEffect::Unsupported;
        cfg.info.adoption = BlkAdoption::NoRegistration;
        cfg.info.detach_no_wait = false;
    }

    // pure config validation -- no I/O, no kernel access. The factory runs it
    // before constructing, so a constructed device is always config-valid.
    static int validate(const NbdConfig& c) {
        if (validate_info(c.info, /*virtio=*/false) < 0)
            return -1;
        if (c.unix_path.empty() && !c.enable_tcp && !c.loopback_device)
            LOG_ERROR_RETURN(EINVAL, -1, "no endpoint enabled: configure unix_path, enable_tcp, or loopback_device");
        return 0;
    }

    const BlkDevInfo& get_info() const override { return cfg.info; }

    ~NbdDeviceImpl() {
        if (started)
            shutdown();
        // shutdown() already released an owned backend; this covers the paths that
        // did not -- a detach() followed by destruction, and an unowned one, which
        // release_backend() leaves alone
        release_backend();
    }

    // Delete a backend this object owns and forget it either way, so that neither
    // the destructor nor a later start() can see it. Legal only once no serving
    // coroutine can touch it, i.e. after detach(true) has drained every connection
    // and cleanup_runtime has joined every worker.
    void release_backend() {
        if (own_backend)
            delete backend;
        backend = nullptr;
        own_backend = false;
    }

    int start(fs::IFile* bk, bool ownership) override {
        if (started)
            LOG_ERROR_RETURN(EALREADY, -1, "nbd device already started");
        if (!bk)
            LOG_ERROR_RETURN(EINVAL, -1, "backend IFile is null");

        backend = bk;
        own_backend = ownership;
        bool ok = false;
        // a failed start returns the object to its virgin state -- INCLUDING
        // backend ownership: the caller keeps the backend (else the destructor
        // and the caller would both delete it). cleanup_runtime() must not do
        // this itself because detach() shares it and must keep the ownership.
        DEFER(if (!ok) {
            int e = errno;
            cleanup_runtime();
            backend = nullptr;
            own_backend = false;
            errno = e;
        });

        // Every connection coroutine is about to be migrated into the pool and
        // will block on its client socket there, so the pool's vcpus must be able
        // to host fd waits. Checked once, before anything is bound: a failure
        // here has nothing to roll back beyond the DEFER's own reset.
        if (check_pool_engines(cfg.pool) < 0)
            return -1;   // DEFER rolls back

        trans_flags = NBD_TRANS_HAS_FLAGS;
        if (cfg.read_only)                            trans_flags |= NBD_TRANS_READ_ONLY;
        if (cfg.info.features & FEATURE_FLUSH)        trans_flags |= NBD_TRANS_SEND_FLUSH | NBD_TRANS_SEND_FUA;
        if (cfg.info.features & FEATURE_DISCARD)      trans_flags |= NBD_TRANS_SEND_TRIM;
        if (cfg.info.features & FEATURE_WRITE_ZEROES) trans_flags |= NBD_TRANS_SEND_WRITE_ZEROES;

        uint32_t cap = cfg.queue_depth ? cfg.queue_depth : DEFAULT_QUEUE_DEPTH;
        if (depth.count() == 0)
            depth.signal(cap);
        if (bytes.count() == 0)
            bytes.signal(MAX_INFLIGHT_BYTES);
        stack_size = cfg.stack_size;
        stall_us = (uint64_t)cfg.stall_timeout * 1000 * 1000;

        if (!cfg.unix_path.empty()) {
            // blk.h start() contract: EBUSY when another live server holds the
            // endpoint. Unlinking a LIVE backend's socket would steal the path:
            // it keeps serving the orphaned inode while new clients come to us.
            // A verdict that cannot be reached is refused the same way, and so is
            // a node that is not a socket -- neither is evidence this path is ours.
            int r = unix_endpoint_replaceable(cfg.unix_path.c_str());
            if (r < 0)
                LOG_ERRNO_RETURN(0, -1, "nbd reached no verdict on the unix socket ", cfg.unix_path);
            if (r == 0)
                LOG_ERRNO_RETURN(0, -1, "nbd refuses to take over the unix socket ", cfg.unix_path);
            // No unlink of our own: bind() below goes through photon's socket
            // server with autoremove on, which clears an existing node itself and
            // only if it is a socket. So the removal inherits a type check this
            // function would otherwise have to repeat, and a regular file at the
            // path survives to fail the bind instead of being deleted.
            uds_server = net::new_uds_server(true);
            if (start_server(uds_server, uds_accept_th, [&] {
                    if (uds_server->bind(cfg.unix_path.c_str()) < 0)
                        LOG_ERRNO_RETURN(0, -1, "failed to bind ", cfg.unix_path);
                    return 0;
                }) < 0)
                return -1;
        }
        if (cfg.enable_tcp) {
            tcp_server = net::new_tcp_socket_server();
            if (start_server(tcp_server, tcp_accept_th, [&] {
                    if (tcp_server->bind(cfg.tcp_endpoint) < 0)
                        LOG_ERRNO_RETURN(0, -1, "failed to bind ", cfg.tcp_endpoint);
                    return 0;
                }) < 0)
                return -1;
        }

        if (cfg.loopback_device && attach_loopback() < 0)
            return -1;  // DEFER rolls back

        started = true;
        ok = true;
        // At the success tail, not where trans_flags is built: a start() that fails
        // afterwards must leave the descriptor saying "nothing negotiated", which is
        // what blk.h promises. Read back off trans_flags rather than re-derived from
        // the request, so there is one source of truth for what went on the wire. The
        // handshake lets a client accept these flags or not connect at all -- it cannot
        // decline one and keep the session -- so what we advertised is what was settled.
        cfg.info.negotiated = ((trans_flags & NBD_TRANS_SEND_FLUSH) ? FEATURE_FLUSH : 0) |
                              ((trans_flags & NBD_TRANS_SEND_TRIM) ? FEATURE_DISCARD : 0) |
                              ((trans_flags & NBD_TRANS_SEND_WRITE_ZEROES) ? FEATURE_WRITE_ZEROES : 0);
#ifdef __linux__
        LOG_INFO("nbd device started, ", VALUE(cfg.info.identity), VALUE(cfg.info.size), make_named_value("loopback_node", (const char*)loopback_node));
#else
        LOG_INFO("nbd device started, ", VALUE(cfg.info.identity), VALUE(cfg.info.size));
#endif
        return 0;
    }

    int detach(bool wait_pending) override {
        if (!started)
            return 0;
        if (wait_pending) {
            // nbd has no kernel-side queue to hand over: draining means waiting
            // for every in-flight request on every connection to complete, then
            // dropping the connections (external clients get EIO afterwards)
            while (true) {
                bool busy = false;
                {
                    SCOPED_LOCK(conns_lock);
                    for (auto c : conns)
                        if (c->in_flight.load(std::memory_order_relaxed)) {
                            busy = true;
                            break;
                        }
                }
                if (!busy)
                    break;
                photon::thread_usleep(1000);
            }
        }
        cleanup_runtime();
        return 0;
    }

    // nbd has no kernel-side registration: shutdown() is detach(true) plus
    // releasing the backend, which is what blk.h's start() contract promises --
    // "ownership = this object deletes it (on shutdown and in the destructor)".
    // There is nothing to destroy (a connected client cannot block it -- we close
    // the connections, which is the nbd version of "release the device"), and after
    // it returns the object is started=false with no backend, so a later start()
    // serves a different one instead of overwriting the pointer and leaking it.
    //
    // detach() alone must NOT release: it is also the rollback path of a failed
    // start(), where the caller keeps the backend.
    int shutdown() override {
        int r = detach(true);
        release_backend();
        return r;
    }

    SocketServers get_server_sockets() override {
        if (!started) return {};
        return {uds_server, tcp_server};
    }

    std::vector<net::ISocketStream*> get_client_connections() override {
        std::vector<net::ISocketStream*> ret;
        SCOPED_LOCK(conns_lock);
        for (auto c : conns)
            ret.push_back(c->s);
        return ret;
    }

    const char* get_device_node() override {
#ifdef __linux__
        return loopback_node[0] ? loopback_node : nullptr;
#else
        return nullptr;   // only a loopback attach names a node, and that is Linux-only
#endif
    }

    // bind + listen + accept loop for one server; bind() logs its own failure
    template <typename BindFn>
    int start_server(net::ISocketServer* srv, photon::thread*& accept_th, BindFn bind) {
        if (!srv)
            LOG_ERROR_RETURN(ENOMEM, -1, "failed to create socket server");
        if (bind() < 0)
            return -1;
        if (srv->listen() < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to listen");
        accept_th = photon::thread_create11(&NbdDeviceImpl::accept_loop, this, srv);
        photon::thread_enable_join(accept_th);
        return 0;
    }

    // Spawn a serve_conn worker for c and register it, then move it into the
    // pool. Registration comes FIRST and the migration LAST: the spawn and the
    // registration are yield-free, but that only orders them against this vcpu,
    // and once migrated the worker runs on another OS thread that does not wait
    // for us to yield. Migrating first would let serve_conn fail out and its
    // DEFER erase-and-delete c before push_back stored the pointer, leaving
    // cleanup_runtime to interrupt and join a dangling entry.
    void spawn_serve_conn(Conn* c) {
        // one coroutine per connection, so this is the stack MAX_CONNECTIONS
        // multiplies -- BlkConfig::stack_size is the caller's knob for it
        auto th = photon::thread_create11(stack_size, &NbdDeviceImpl::serve_conn, this, c);
        if (!th) {
            // serve_conn's DEFER owns both the Conn and its stream, and it never
            // ran -- so nothing else would free either, and workers would hold a
            // null that cleanup_runtime interrupts and joins
            LOG_ERROR("nbd: cannot create the connection coroutine, dropping the client");
            c->s->close();
            delete c->s;
            delete c;
            return;
        }
        photon::thread_enable_join(th);
        // counted before the migration, i.e. before this coroutine can run at all:
        // from here on cleanup_runtime owes it either a join or a wait
        live_workers.fetch_add(1, std::memory_order_relaxed);
        {
            SCOPED_LOCK(conns_lock);
            conns.push_back(c);
            workers.push_back(th);
        }
        // The connection coroutine is the fan-out unit here: nbd has no queue count
        // to declare, its parallelism is however many clients connect. Safe to move
        // because nothing in the socket path caches a vcpu -- wait_for_fd_readable
        // resolves the engine from the CURRENT vcpu on every call, so the stream
        // this accept produced keeps working on the vcpu it lands on.
        migrate_to_pool(cfg.pool, th);
    }

    void accept_loop(net::ISocketServer* srv) {
        while (!stopping.load(std::memory_order_relaxed)) {
            auto s = srv->accept();
            if (!s) {
                if (stopping.load(std::memory_order_relaxed))
                    break;
                photon::thread_usleep(1000);
                continue;
            }
            // Refuse rather than wait at the cap: waiting would only move the
            // unbounded queue into the listen backlog, and a refused client
            // retries. The read is under conns_lock because serve_conn's DEFER
            // erases from serve_conn's own vcpu, which is not necessarily this one.
            size_t nconns;
            {
                SCOPED_LOCK(conns_lock);
                nconns = conns.size();
            }
            if (nconns >= MAX_CONNECTIONS) {
                LOG_WARN("nbd: at the `-connection limit, refusing a client", MAX_CONNECTIONS);
                s->close();
                delete s;
                continue;
            }
            spawn_serve_conn(new Conn{s, true});
        }
    }

    void serve_conn(Conn* c) {
        DEFER({
            // no new dispatch past the loop below; wait for the in-flight
            // ones, so none of them writes to the closed stream (or touches
            // this Conn) afterwards
            while (c->in_flight.load(std::memory_order_relaxed))
                photon::thread_usleep(1000);
            c->s->close();
            {
                SCOPED_LOCK(conns_lock);
                conns.erase(std::remove(conns.begin(), conns.end(), c), conns.end());
            }
            delete c->s;
            delete c;
            retire_self();
        });

        if (c->negotiate && negotiate(c->s) < 0)
            return;

        while (!stopping.load(std::memory_order_relaxed)) {
            NbdRequest req;
            if (c->s->read(&req, sizeof(req)) != (ssize_t)sizeof(req))
                break;
            if (!req.decode()) {
                LOG_WARN("bad nbd request magic, closing connection");
                break;
            }
            if (req.type == NBD_CMD_DISC)
                break;

            // the data buffer of READ and WRITE alike: allocated below, freed
            // by execute(); a WRITE payload is read into it inline
            char* buf = nullptr;
            bool need_buf = req.type == NBD_CMD_READ || req.type == NBD_CMD_WRITE;
            if (need_buf && req.length > MAX_BLOCK_SIZE) {
                if (req.type == NBD_CMD_WRITE) {
                    stall_guard stall(c->s, stall_us);
                    if (!c->s->skip_read(req.length))
                        break;
                }
                send_reply(c, req.handle, NBD_EINVAL, nullptr, 0);
                continue;
            }

            // the queue-depth gate must be interruptible: an uninterruptible
            // wait would swallow cleanup_runtime's shutdown interrupt, let one
            // more request dispatch, then block in read() with no interrupt
            // pending -- hanging the worker join
            //
            // It also has to come BEFORE the buffer: allocating first let every
            // connection park a buffer here while it waited, so memory was
            // bounded by the connection count instead of by the gates. Holding
            // the slot across the payload read is the point -- that read is what
            // the buffer is for.
            if (depth.wait_interruptible(1) < 0)
                break;

            // The byte gate bounds that same memory by SIZE rather than by count:
            // depth alone caps it at queue_depth * MAX_BLOCK_SIZE (4 GiB), which
            // a deep queue of large sequential requests reaches legitimately.
            // Taken after depth and released before it, so the two are always
            // acquired in one order and cannot deadlock against each other.
            // cost is 0 unless this request allocates a buffer.
            uint64_t cost = byte_cost(req.type, req.length);
            if (cost && bytes.wait_interruptible(cost) < 0) {
                depth.signal(1);
                break;
            }

            if (need_buf) {
                buf = (char*)malloc(req.length ? req.length : 1);
                if (!buf) {
                    bytes.signal(cost);
                    depth.signal(1);
                    if (req.type == NBD_CMD_WRITE) {
                        stall_guard stall(c->s, stall_us);
                        if (!c->s->skip_read(req.length))
                            break;
                    }
                    send_reply(c, req.handle, NBD_ENOMEM, nullptr, 0);
                    continue;
                }
                if (req.type == NBD_CMD_WRITE && req.length) {
                    // Bounded because BOTH gates are already held: a client that
                    // sent this header and then stopped would hold a queue-depth
                    // slot and its share of the byte budget until it felt like
                    // finishing, so queue_depth would end up bounding the honest
                    // clients only. A read() gets one deadline for its whole
                    // count, so this either arrives or fails within stall_timeout.
                    // Dropping the connection is the answer: the client still owes
                    // bytes this request will never receive, so the stream is out
                    // of step from here on whatever we replied.
                    stall_guard stall(c->s, stall_us);
                    if (c->s->read(buf, req.length) != (ssize_t)req.length) {
                        free(buf);
                        bytes.signal(cost);
                        depth.signal(1);
                        break;
                    }
                }
            }

            c->in_flight++;
            // passing packed fields through thread_create11's forwarding-
            // reference parameters is rejected by gcc: copy them out first
            uint16_t type = req.type, flags = req.flags;
            uint64_t handle = req.handle, offset = req.offset;
            uint32_t len = req.length;
            // execute()'s DEFER is the sole owner of buf, the depth token, the
            // byte-gate tokens and c->in_flight, so give all four back if the
            // create fails (nullptr = photon's stack allocation failed).
            // Otherwise the buffer leaks, a queue-depth slot and its byte budget
            // are lost forever, and serve_conn's DEFER drain hangs on an
            // in_flight that never reaches 0 -- which hangs the worker join in
            // cleanup_runtime. The client is waiting on this handle, so answer it
            // instead of going silent; send_reply's result is ignored as the
            // neighbouring error paths do, since a failed write means the stream
            // is dead and the read above breaks out.
            if (!photon::thread_create11(stack_size, &NbdDeviceImpl::execute, this, c,
                                         type, flags, handle, offset, len, buf)) {
                c->in_flight--;
                bytes.signal(cost);
                depth.signal(1);
                free(buf);
                LOG_ERROR("nbd: cannot create the request coroutine, handle `", handle);
                send_reply(c, req.handle, NBD_ENOMEM, nullptr, 0);
            }
        }
    }

    static void reap_worker(photon::thread* w) {
        photon::thread_join((photon::join_handle*)w);
    }

    // Last act of a connection coroutine: leave the list cleanup_runtime joins, and
    // only if that departure really happened, hand its own handle to a throwaway
    // coroutine that does nothing but join it.
    //
    // A connection coroutine has to be joinable -- cleanup_runtime must be able to
    // interrupt one that is parked in a gate wait, and only a joinable thread can
    // be joined afterwards -- but a joinable photon thread keeps its stack until
    // somebody joins it, and a coroutine cannot join itself. Leaving all of them to
    // cleanup_runtime therefore made the list grow with connection HISTORY rather
    // than with live connections: stack_size per connection ever made, which is
    // 8 MiB each at the default. The reaper is created WITHOUT join enabled, so
    // photon frees it when it exits.
    //
    // WHETHER THE HANDLE WAS FOUND is what decides who joins it, and the lock is
    // what makes that one owner: cleanup_runtime takes the whole list under this
    // same lock, so either it swapped first -- this departure then finds nothing,
    // the handle is in the list it is joining, and no reaper is made -- or this
    // departure ran first and that swap never sees the handle. Creating the reaper
    // before asking the question lets both sides join it, and a second thread_join
    // on a handle the first one disposed dereferences freed memory.
    //
    // live_workers is decremented last, whichever side owns the join, because
    // cleanup_runtime waits on it only after joining everything it did take.
    void retire_self() {
        photon::thread* self = photon::CURRENT;
        bool ours;
        {
            SCOPED_LOCK(conns_lock);
            auto it = std::find(workers.begin(), workers.end(), self);
            ours = it != workers.end();
            if (ours)
                workers.erase(it);
        }
        if (ours && !photon::thread_create11(REAPER_STACK_SIZE, &reap_worker, self)) {
            // No reaper, so put the handle back for cleanup_runtime to join. If that
            // swap has already happened in between, nothing ever joins it and one
            // stack leaks -- which is what an out-of-memory thread creation costs
            // here, and is why this branch hands the handle back instead of dropping
            // it: dropping it would leak the stack on EVERY failure, not just the one
            // that races a shutdown.
            SCOPED_LOCK(conns_lock);
            workers.push_back(self);
        }
        live_workers.fetch_sub(1, std::memory_order_relaxed);
    }

    void execute(Conn* c, uint16_t type, uint16_t cflags, uint64_t handle, uint64_t offset, uint32_t len, char* buf) {
        DEFER({
            free(buf);
            // reverse of serve_conn's acquisition order: bytes were taken after
            // depth, so they go back before it. byte_cost is the SAME function
            // serve_conn charged, which is the only thing keeping the two sides
            // of the budget in agreement.
            bytes.signal(byte_cost(type, len));
            depth.signal(1);
            // the decrement is the last touch of the Conn: serve_conn's DEFER
            // frees it once in_flight reaches 0
            c->in_flight--;
        });

        uint32_t err = NBD_SUCCESS;
        uint64_t sz = cfg.info.size;
        bool oob = offset > sz || len > sz - offset;
        switch (type) {
        case NBD_CMD_READ:
            if (oob) { err = NBD_EINVAL; break; }
            {
                struct iovec iov{buf, len};
                ssize_t r = backend->preadv(&iov, 1, offset);
                if (r != (ssize_t)len)
                    err = errno_to_nbd(r < 0 ? errno : EIO);
            }
            break;
        case NBD_CMD_WRITE:
            if (cfg.read_only) { err = NBD_EPERM; break; }
            if (oob) { err = NBD_EINVAL; break; }
            {
                struct iovec iov{buf, len};
                ssize_t w = backend->pwritev(&iov, 1, offset);
                if (w != (ssize_t)len) {
                    err = errno_to_nbd(w < 0 ? errno : EIO);
                    break;   // nothing landed, so there is nothing to make durable
                }
                // FUA is pwritev + fdatasync, not pwritev2(RWF_DSYNC): pwritev2 is
                // not pure virtual, and its base body discards `flags` and forwards
                // to pwritev, so a backend that does not override it would have
                // acked this write before it was durable -- and the reply below is
                // the device's word that it is. fdatasync IS pure virtual, so every
                // backend answers it. Same choice and same reasoning as the shared
                // virtio engine's write-through path, which this must not disagree
                // with. It costs a second syscall, and one that waits for the whole
                // file's dirty data rather than only this range, which is what the
                // client asked for by setting FUA on this request instead of
                // batching a FLUSH.
                if ((cflags & NBD_REQ_FUA) && backend->fdatasync() < 0)
                    err = errno_to_nbd(errno);
            }
            break;
        case NBD_CMD_FLUSH:
            if (backend->fdatasync() < 0)
                err = errno_to_nbd(errno);
            break;
        case NBD_CMD_TRIM:
#ifdef __linux__
            if (cfg.read_only) { err = NBD_EPERM; break; }
            if (oob) { err = NBD_EINVAL; break; }
            if (backend->trim(offset, len) < 0) {
                err = errno_to_nbd(errno);
                break;
            }
            // FUA is legal on TRIM as well, and a punch-hole the client asked to be
            // durable is not durable until the filesystem's metadata is: the same
            // fdatasync as the WRITE path, for the same reason.
            if ((cflags & NBD_REQ_FUA) && backend->fdatasync() < 0)
                err = errno_to_nbd(errno);
#else
            err = NBD_ENOTSUP;  // IFile::trim is fallocate-based, Linux-only
#endif
            break;
        case NBD_CMD_WRITE_ZEROES:
#ifdef __linux__
            if (cfg.read_only) { err = NBD_EPERM; break; }
            if (oob) { err = NBD_EINVAL; break; }
            if (backend->zero_range(offset, len) < 0) {
                // Both spellings of "this backend has no hole-punch": a filesystem
                // whose fallocate lacks ZERO_RANGE answers EOPNOTSUPP, and an IFile
                // that does not forward fallocate at all answers ENOSYS. Only those
                // two fall back to writing zeroes -- an EIO or ENOSPC from a real
                // attempt is the backend's own answer and has to reach the client
                // unchanged, or a failing device would look like a slow one.
                if (errno != EOPNOTSUPP && errno != ENOSYS) {
                    err = errno_to_nbd(errno);
                    break;
                }
                err = write_zeroes_fallback(offset, len);
                if (err != NBD_SUCCESS)
                    break;
            }
            if ((cflags & NBD_REQ_FUA) && backend->fdatasync() < 0)
                err = errno_to_nbd(errno);
#else
            err = NBD_ENOTSUP;  // IFile::zero_range is fallocate-based, Linux-only
#endif
            break;
        default:
            err = NBD_ENOTSUP;
        }
        send_reply(c, handle, err, (type == NBD_CMD_READ && err == NBD_SUCCESS) ? buf : nullptr, len);
    }

    // backends whose fallocate lacks ZERO_RANGE support (e.g. tmpfs) still get
    // correct semantics: we never advertise NBD_FLAG_SEND_FAST_ZERO, so the
    // client accepts zeroing by explicit writes
    uint32_t write_zeroes_fallback(uint64_t offset, uint32_t len) {
        static const char zeros[256 << 10] = {};  // only ever read
        while (len) {
            uint32_t k = len > sizeof(zeros) ? sizeof(zeros) : len;
            struct iovec iov{(void*)zeros, k};
            ssize_t w = backend->pwritev(&iov, 1, offset);
            // A short count is a legal pwritev result and leaves errno alone, so
            // translating with errno here would report "wrote k-1 of k bytes" as
            // whatever the last unrelated failure happened to leave behind -- and
            // errno_to_nbd(0) is NBD_SUCCESS, which would ack the rest of the range
            // as zeroed when it was not. Same shape as the READ and WRITE paths:
            // negative means errno, non-negative but short means EIO.
            if (w != (ssize_t)k)
                return errno_to_nbd(w < 0 ? errno : EIO);
            offset += k;
            len -= k;
        }
        return NBD_SUCCESS;
    }

    int send_reply(Conn* c, uint64_t handle, uint32_t err, const void* data, uint32_t len) {
        NbdSimpleReply rep{err, handle};
        rep.encode();
        SCOPED_LOCK(c->wlock);
        if (c->s->write(&rep, sizeof(rep)) != (ssize_t)sizeof(rep))
            return -1;
        if (data && len && c->s->write(data, len) != (ssize_t)len)
            return -1;
        return 0;
    }

    // What the export name in an option payload turned out to be.
    enum class Name { MATCH, MISMATCH, MALFORMED };

    // Parse the payload of NBD_OPT_EXPORT_NAME, NBD_OPT_INFO or NBD_OPT_GO,
    // consuming exactly `length` bytes: a 32-bit export-name length, the name, and
    // -- for INFO and GO only -- a 16-bit count of requested information items,
    // each a 16-bit type, a 16-bit length and that many bytes. `length` is the
    // option header's, already bounded by MAX_OPTION_LEN.
    //
    // A payload that does not parse is refused rather than skipped. Skipping is
    // what let a 4-byte OPT_GO through, although even an empty name with no items
    // needs 6, and it left the item list unread, so nothing in it could be honoured
    // or even seen.
    //
    // The name is compared as it is read instead of being buffered: only a name
    // whose length equals the identity's can match, and the identity IS this
    // export's name, so a client asking for a different export is refused rather
    // than served this one. An empty name asks for the default export and matches.
    //
    // The requested items are parsed and dropped. They are a request, not a
    // requirement -- the server answers with the items it chooses, and this one
    // always answers NBD_INFO_EXPORT alone. NBD_INFO_BLOCK_SIZE in particular
    // cannot be honoured: the geometry is cfg.info's for the whole export, not
    // something one connection can renegotiate.
    Name read_export_option(net::ISocketStream* s, uint32_t length, bool with_items) {
        const std::string& id = cfg.info.identity;
        if (length < (with_items ? 6u : 4u))
            return Name::MALFORMED;
        uint32_t name_len;
        if (s->read(&name_len, sizeof(name_len)) != (ssize_t)sizeof(name_len))
            return Name::MALFORMED;
        name_len = __builtin_bswap32(name_len);
        uint32_t used = sizeof(name_len);
        if (name_len > length - used)
            return Name::MALFORMED;
        // An empty name asks for the default export, which is the only one there is,
        // so it matches whatever the identity happens to be.
        bool match = name_len == 0 || name_len == (uint32_t)id.size();
        for (uint32_t off = 0; off < name_len; ) {
            char chunk[256];
            uint32_t k = name_len - off;
            if (k > sizeof(chunk))
                k = (uint32_t)sizeof(chunk);
            if (s->read(chunk, k) != (ssize_t)k)
                return Name::MALFORMED;
            if (match && memcmp(chunk, id.data() + off, k) != 0)
                match = false;
            off += k;
        }
        used += name_len;
        if (!with_items)
            return used == length ? (match ? Name::MATCH : Name::MISMATCH) : Name::MALFORMED;
        uint16_t items;
        if (s->read(&items, sizeof(items)) != (ssize_t)sizeof(items))
            return Name::MALFORMED;
        items = __builtin_bswap16(items);
        used += sizeof(items);
        for (uint16_t i = 0; i < items; i++) {
            uint16_t hdr[2];   // the item's type, which nothing here acts on, then its length
            if (length - used < sizeof(hdr) || s->read(hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr))
                return Name::MALFORMED;
            uint16_t ilen = __builtin_bswap16(hdr[1]);
            used += sizeof(hdr);
            if (ilen > length - used || !s->skip_read(ilen))
                return Name::MALFORMED;
            used += ilen;
        }
        // The walk has to land exactly on the option's own length. Anything else
        // means this side and the client disagree about the framing, and going on
        // would read the next option out of the middle of this one.
        if (used != length)
            return Name::MALFORMED;
        return match ? Name::MATCH : Name::MISMATCH;
    }

    int negotiate(net::ISocketStream* s) {
        // Every read of the handshake is bounded: a client that connects and then
        // stops mid-handshake holds a connection slot and this coroutine's stack
        // for as long as it likes, and cfg.timeout releases neither -- that one is
        // the kernel's request timeout for the loopback device. Restored on the way
        // out, because the transmission phase that follows has to leave an idle
        // client alone: waiting for its next request is the job, not a stall.
        stall_guard stall(s, stall_us);
        NbdGreeting greeting{NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES};
        greeting.encode();
        if (s->write(&greeting, sizeof(greeting)) != (ssize_t)sizeof(greeting))
            return -1;

        uint32_t cflags;
        if (s->read(&cflags, sizeof(cflags)) != (ssize_t)sizeof(cflags))
            return -1;
        cflags = __builtin_bswap32(cflags);
        if (!(cflags & NBD_FLAG_C_FIXED_NEWSTYLE)) {
            LOG_WARN("client without NBD_FLAG_C_FIXED_NEWSTYLE, rejecting");
            return -1;
        }
        bool no_zeroes = cflags & NBD_FLAG_C_NO_ZEROES;

        while (true) {
            NbdOptionHeader opt;
            if (s->read(&opt, sizeof(opt)) != (ssize_t)sizeof(opt))
                return -1;
            if (!opt.decode()) {
                LOG_WARN("bad nbd option magic, closing connection");
                return -1;
            }
            if (opt.length > MAX_OPTION_LEN) {
                uint32_t length = opt.length;
                LOG_ERROR_RETURN(EPROTO, -1, "nbd option too long, ", VALUE(length));
            }

            switch (opt.opt) {
            case NBD_OPT_EXPORT_NAME: {
                // The protocol allows no reply to EXPORT_NAME -- the server either
                // sends the export meta or closes -- so a refusal here is a close.
                Name v = read_export_option(s, opt.length, false);
                if (v != Name::MATCH) {
                    if (v == Name::MALFORMED)
                        LOG_WARN("nbd OPT_EXPORT_NAME payload does not parse, closing");
                    else
                        LOG_WARN("nbd client asked for an export this device does not serve, closing");
                    return -1;
                }
                NbdExportMeta meta{cfg.info.size, trans_flags};
                meta.encode();
                if (s->write(&meta, sizeof(meta)) != (ssize_t)sizeof(meta))
                    return -1;
                if (!no_zeroes) {
                    // 124 reserved trailing bytes
                    static const char zeros[124] = {};
                    if (s->write(zeros, sizeof(zeros)) != (ssize_t)sizeof(zeros))
                        return -1;
                }
                return 0;  // transmission phase
            }
            case NBD_OPT_ABORT:
                send_opt_reply(s, opt.opt, NBD_REP_ACK, nullptr, 0);
                return -1;
            case NBD_OPT_LIST: {
                if (!s->skip_read(opt.length))
                    return -1;
                const std::string& name = cfg.info.identity;
                char entry[4 + 256];
                if (name.size() <= 256) {
                    uint32_t name_len = __builtin_bswap32((uint32_t)name.size());
                    memcpy(entry, &name_len, 4);
                    memcpy(entry + 4, name.data(), name.size());
                    send_opt_reply(s, opt.opt, NBD_REP_LIST, entry, 4 + name.size());
                }
                send_opt_reply(s, opt.opt, NBD_REP_ACK, nullptr, 0);
                break;
            }
            case NBD_OPT_INFO:
            case NBD_OPT_GO: {
                Name v = read_export_option(s, opt.length, true);
                if (v != Name::MATCH) {
                    if (v == Name::MALFORMED) {
                        // a packed field cannot bind to the logger's reference
                        uint32_t which = opt.opt;
                        LOG_WARN("nbd option ` payload does not parse, closing", which);
                    } else
                        LOG_WARN("nbd client asked for an export this device does not serve, closing");
                    // best-effort: an error reply ends the option phase, so this
                    // connection closes whether the reply went out or not
                    send_opt_reply(s, opt.opt, NBD_REP_ERR_INVALID, nullptr, 0);
                    return -1;
                }
                NbdExportInfo info{NBD_INFO_EXPORT, {cfg.info.size, trans_flags}};
                info.encode();
                if (send_opt_reply(s, opt.opt, NBD_REP_INFO, &info, sizeof(info)) < 0 ||
                    send_opt_reply(s, opt.opt, NBD_REP_ACK, nullptr, 0) < 0)
                    return -1;
                if (opt.opt == NBD_OPT_GO)
                    return 0;  // transmission phase
                break;
            }
            default:
                // notably NBD_OPT_STRUCTURED_REPLY: refuse, the client falls
                // back to simple replies
                if (!s->skip_read(opt.length))
                    return -1;
                if (send_opt_reply(s, opt.opt, NBD_REP_ERR_UNSUP, nullptr, 0) < 0)
                    return -1;
            }
        }
    }

    static int send_opt_reply(net::ISocketStream* s, uint32_t opt, uint32_t type, const void* data, uint32_t len) {
        NbdOptionReply rep{opt, type, len};
        rep.encode();
        if (s->write(&rep, sizeof(rep)) != (ssize_t)sizeof(rep))
            return -1;
        if (len && s->write(data, len) != (ssize_t)len)
            return -1;
        return 0;
    }

    void cleanup_runtime() {
        stopping.store(true, std::memory_order_relaxed);
        // coroutines blocked in photon fd-event waits (accept / read) only wake
        // via thread_interrupt -- closing the fd does not fire the event engine,
        // and ISocketServer::terminate() is a no-op here because we drive
        // accept() ourselves instead of start_loop()
        if (uds_accept_th)
            photon::thread_interrupt(uds_accept_th);
        if (tcp_accept_th)
            photon::thread_interrupt(tcp_accept_th);
        if (uds_server)
            uds_server->terminate();
        if (tcp_server)
            tcp_server->terminate();
        if (uds_accept_th) {
            photon::thread_join((photon::join_handle*)uds_accept_th);
            uds_accept_th = nullptr;
        }
        if (tcp_accept_th) {
            photon::thread_join((photon::join_handle*)tcp_accept_th);
            tcp_accept_th = nullptr;
        }
        // the execute coroutines are not in the interrupt list below; one
        // blocked in send_reply's write to a stalled client (full socket
        // buffer) would never finish, so its serve_conn DEFER's in_flight
        // drain would spin forever and hang the worker join. Half-closing
        // makes the fd report HUP, waking that write with EPIPE.
        //
        // The interrupt/join below must stay outside conns_lock: a worker's
        // DEFER takes the same lock, so joining it while holding the lock
        // deadlocks.
        {
            SCOPED_LOCK(conns_lock);
            for (auto c : conns)
                c->s->shutdown(ShutdownHow::ReadWrite);
            std::vector<Conn*>().swap(conns);  // each Conn is closed+deleted by its worker
        }
        std::vector<photon::thread*> ws;
        {
            // taken under the same lock a finishing worker uses to leave the list,
            // so this snapshot and a departure cannot interleave
            SCOPED_LOCK(conns_lock);
            ws.swap(workers);
        }
        for (auto w : ws)
            photon::thread_interrupt(w);
        // join, and let the workers close their own streams: an interrupted
        // coroutine cancels its pending fd-event wait before its serve_conn
        // DEFER closes the fd, so the fd stays valid for both; closing here
        // first would invalidate the cancellation, and closing here after the
        // join would dereference Conns the workers already deleted
        for (auto w : ws)
            photon::thread_join((photon::join_handle*)w);
        // A worker that finished on its own left the list before this snapshot was
        // taken, so the joins above never saw it. It is now between that departure
        // and its own exit, which has no yield in it, and this wait is what keeps
        // the object alive until it is through -- the departure itself was its last
        // touch of anything here.
        while (live_workers.load(std::memory_order_relaxed))
            photon::thread_usleep(1000);
        disconnect_loopback();
        if (uds_server) {
            delete uds_server;
            uds_server = nullptr;
        }
        if (tcp_server) {
            delete tcp_server;
            tcp_server = nullptr;
        }
        started = false;
        stopping.store(false, std::memory_order_relaxed);
    }

#ifdef __linux__
    static int find_free_nbd(char* node, size_t size) {
        DIR* d = opendir("/sys/block");
        if (!d)
            LOG_ERRNO_RETURN(0, -1, "failed to open /sys/block");
        DEFER(closedir(d));
        struct dirent* e;
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, "nbd", 3) != 0)
                continue;
            // What follows "nbd" has to be the device index and nothing else:
            // /sys/block holds every disk on the box and "nbd" is only a prefix,
            // so a name like "nbd_backup" would otherwise be probed as though it
            // named a device. Bounding it to digits is also what keeps the two
            // snprintf's below inside the caller's buffer by construction rather
            // than by their precision alone.
            const char* p = e->d_name + 3;
            if (!*p)
                continue;
            bool index_only = true;
            for (; *p; p++)
                if (*p < '0' || *p > '9') {
                    index_only = false;
                    break;
                }
            if (!index_only)
                continue;
            // /sys/block holds one entry per disk and a disk name is at most 31
            // characters, so these never truncate into our caller's node buffer;
            // the precision (size, less the literal text around it and the NUL)
            // just tells the compiler so -- dirent declares d_name as char[256].
            // /sys/block/nbdN/pid exists only while the device is attached
            snprintf(node, size, "/sys/block/%.*s/pid", (int)size - 16, e->d_name);
            if (::access(node, F_OK) == 0)
                continue;
            // node now holds "/dev/nbdN", the result on return
            snprintf(node, size, "/dev/%.*s", (int)size - 6, e->d_name);
            if (::access(node, F_OK) != 0)
                continue;
            return 0;
        }
        LOG_ERROR_RETURN(ENODEV, -1, "no free /dev/nbdN found (is the nbd module loaded?)");
    }

    // both attach paths configure the size before the device is actually up
    // (legacy: NBD_DO_IT reaches nbd_start_device; netlink: CONNECT starts
    // the device before replying); poll until the capacity appears and udev
    // has made the node
    int wait_capacity(const char* node) {
        uint64_t expect_sectors = cfg.info.size >> 9;
        char size_path[128];
        snprintf(size_path, sizeof(size_path), "/sys/block/%s/size", node + 5);
        for (int i = 0; i < 3000; i++) {
            char buf[32] = {};
            int sfd = ::open(size_path, O_RDONLY);
            if (sfd >= 0) {
                ssize_t n = ::read(sfd, buf, sizeof(buf) - 1);
                (void)n;
                ::close(sfd);
                if (strtoull(buf, nullptr, 10) == expect_sectors && ::access(node, F_OK) == 0)
                    return 0;
            }
            photon::thread_usleep(1000);
        }
        LOG_ERROR_RETURN(ETIMEDOUT, -1, "nbd device ` did not reach capacity ` within 3s", node, expect_sectors);
    }

    int attach_loopback_legacy() {
        char node[64];
        if (find_free_nbd(node, sizeof(node)) < 0)
            return -1;

        int sp[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0)
            LOG_ERRNO_RETURN(0, -1, "socketpair() failed");
        bool ok = false;
        bool sock_handed = false;  // sp[1] already closed post-NBD_SET_SOCK
        bool conn_handed = false;  // serve_conn owns sp[0] once it is spawned
        bool doit_handed = false;  // disconnect_loopback owns nbd_fd once DO_IT runs
        DEFER({
            if (ok) return;
            int e = errno;
            if (!sock_handed)
                ::close(sp[1]);
            if (!conn_handed)
                ::close(sp[0]);
            if (!doit_handed && nbd_fd >= 0) {
                ::close(nbd_fd);
                nbd_fd = -1;
            }
            errno = e;
        });

        nbd_fd = ::open(node, O_RDWR);
        if (nbd_fd < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to open ", node);
        if (ioctl(nbd_fd, NBD_SET_SOCK, sp[1]) < 0)
            LOG_ERRNO_RETURN(0, -1, "NBD_SET_SOCK failed on ", node);
        // Close our end here, exactly as the netlink path does after CONNECT.
        // NBD_SET_SOCK hands the socket to the kernel and the kernel keeps its
        // own reference to it, so our fd table entry is not what keeps the
        // connection alive. Leaving it to the DEFER's sock_handed flag leaked one
        // fd per attach/detach cycle -- disconnect_loopback() closes nbd_fd and
        // the config fd but not this one.
        ::close(sp[1]);
        sock_handed = true;
        // set early so that cleanup_runtime's disconnect_loopback() can clear
        // the config even if a later step fails
        snprintf(loopback_node, sizeof(loopback_node), "%s", node);
        uint64_t ss = 1ull << cfg.info.sector_size_shift;
        // NBD_SET_FLAGS carries the same word the netlink path passes as
        // NBD_ATTR_SERVER_FLAGS, and the flag bits are the same ones the handshake
        // sends: this is the legacy path's only chance to tell the kernel that the
        // export is read-only and that it may send flushes, FUA writes and trims.
        // Without it a legacy attach and a netlink attach of the same config
        // disagree about all four, and silently: the kernel is never told it may
        // ask for durability, so it stops asking, and never told the export is
        // read-only, so it accepts writes locally that this server then refuses.
        if (ioctl(nbd_fd, NBD_SET_BLKSIZE, ss) < 0 ||
            ioctl(nbd_fd, NBD_SET_SIZE_BLOCKS, (unsigned long)(cfg.info.size / ss)) < 0 ||
            ioctl(nbd_fd, NBD_SET_FLAGS, (unsigned long)trans_flags) < 0 ||
            ioctl(nbd_fd, NBD_SET_TIMEOUT, (unsigned long)cfg.timeout) < 0)
            LOG_ERRNO_RETURN(0, -1, "NBD_SET_* ioctls failed on ", node);

        // a brand-new socketpair has exchanged zero bytes, i.e. it is already
        // at the beginning of the transmission phase: exactly what the kernel
        // needs, since it performs no NBD negotiation itself
        auto s = net::new_kernel_socket_stream(sp[0]);
        if (!s)
            return -1;
        spawn_serve_conn(new Conn{s, false});
        conn_handed = true;

        // NBD_DO_IT blocks for the lifetime of the attachment: dedicated OS thread
        doit_thread = std::thread([fd = nbd_fd] {
            ::ioctl(fd, NBD_DO_IT);
        });
        doit_handed = true;

        if (wait_capacity(node) < 0)
            return -1;
        ok = true;
        LOG_INFO("nbd loopback attached via legacy ioctls, ", make_named_value("loopback_node", (const char*)loopback_node));
        return 0;
    }

    // NBD_CMD_CONNECT allocates the device index dynamically and the kernel
    // spawns its own recv/send workers -- no user-space DO_IT thread needed
    int attach_loopback_netlink() {
        GenlSock gs;
        if (gs.sk < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to open netlink socket");
        int fam = gs.resolve_family(NBD_GENL_FAMILY_NAME);
        if (fam < 0)
            return -1;  // no nbd netlink family: fall back to legacy ioctls

        int sp[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0)
            LOG_ERRNO_RETURN(0, -1, "socketpair() failed");
        bool ok = false;
        bool sock_handed = false;  // the kernel owns sp[1] after CONNECT
        bool conn_handed = false;  // serve_conn owns sp[0] once it is spawned
        DEFER({
            if (ok) return;
            int e = errno;
            if (sock_handed) {
                // CONNECT succeeded but a later step failed: the device is
                // still configured, so an explicit DISCONNECT is required
                char d[32];
                size_t dl = nla_append_u32(d, 0, sizeof(d), NBD_ATTR_INDEX, nbd_index);
                gs.request(fam, NBD_CMD_DISCONNECT, d, dl, NBD_ATTR_UNSPEC, nullptr);
            } else {
                ::close(sp[1]);
            }
            if (!conn_handed)
                ::close(sp[0]);
            loopback_node[0] = '\0';
            loopback_netlink = false;
            errno = e;
        });

        uint64_t ss = 1ull << cfg.info.sector_size_shift;
        char attrs[256];
        size_t len = 0;
        len += nla_append_u64(attrs, len, sizeof(attrs), NBD_ATTR_SIZE_BYTES, cfg.info.size);
        len += nla_append_u64(attrs, len, sizeof(attrs), NBD_ATTR_BLOCK_SIZE_BYTES, ss);
        len += nla_append_u64(attrs, len, sizeof(attrs), NBD_ATTR_SERVER_FLAGS, trans_flags);
        if (cfg.timeout)
            len += nla_append_u64(attrs, len, sizeof(attrs), NBD_ATTR_TIMEOUT, cfg.timeout);
        // NBD_ATTR_SOCKETS { NBD_SOCK_ITEM { NBD_SOCK_FD } }; the doit looks
        // the fd up in our fd table and takes its own reference
        char item[32], socks[64];
        size_t ilen = nla_append_u32(item, 0, sizeof(item), NBD_SOCK_FD, (uint32_t)sp[1]);
        size_t slen = nla_append(socks, 0, sizeof(socks), NBD_SOCK_ITEM | NLA_F_NESTED, item, ilen);
        len += nla_append(attrs, len, sizeof(attrs), NBD_ATTR_SOCKETS | NLA_F_NESTED, socks, slen);
        if (!len || !ilen || !slen)
            LOG_ERROR_RETURN(E2BIG, -1, "netlink CONNECT attributes overflow");

        uint32_t idx = 0;
        if (gs.request(fam, NBD_CMD_CONNECT, attrs, len, NBD_ATTR_INDEX, &idx) < 0)
            return -1;
        ::close(sp[1]);  // the kernel keeps its own reference past CONNECT
        sock_handed = true;
        nbd_index = idx;
        char node[64];
        snprintf(node, sizeof(node), "/dev/nbd%u", idx);
        // set early so that cleanup_runtime's disconnect_loopback() can tear
        // down even if a later step fails
        snprintf(loopback_node, sizeof(loopback_node), "%s", node);
        loopback_netlink = true;

        // same as the legacy path: a fresh socketpair is already at the
        // beginning of the transmission phase
        auto s = net::new_kernel_socket_stream(sp[0]);
        if (!s)
            return -1;
        spawn_serve_conn(new Conn{s, false});
        conn_handed = true;

        if (wait_capacity(node) < 0)
            return -1;
        ok = true;
        LOG_INFO("nbd loopback attached via netlink, ", make_named_value("loopback_node", (const char*)loopback_node));
        return 0;
    }

    int attach_loopback() {
        // netlink first (kernel 4.9+), legacy ioctls as fallback
        if (attach_loopback_netlink() == 0)
            return 0;
        return attach_loopback_legacy();
    }

    void disconnect_loopback() {
        if (!loopback_node[0])
            return;
        if (loopback_netlink) {
            // a netlink device stays configured after its socket dies
            // (USER_RECOVERY semantics); only an explicit NBD_CMD_DISCONNECT
            // releases it
            GenlSock gs;
            int fam = gs.sk >= 0 ? gs.resolve_family(NBD_GENL_FAMILY_NAME) : -1;
            char d[32];
            size_t dl = nla_append_u32(d, 0, sizeof(d), NBD_ATTR_INDEX, nbd_index);
            if (fam < 0 || !dl ||
                gs.request(fam, NBD_CMD_DISCONNECT, d, dl, NBD_ATTR_UNSPEC, nullptr) < 0)
                LOG_ERROR("failed to disconnect nbd ` via netlink, ", loopback_node, ERRNO());
            loopback_node[0] = '\0';
            loopback_netlink = false;
            return;
        }
        int dfd = ::open(loopback_node, O_RDWR);
        if (dfd >= 0)
            ioctl(dfd, NBD_DISCONNECT);
        if (doit_thread.joinable())
            doit_thread.join();
        if (dfd >= 0) {
            ioctl(dfd, NBD_CLEAR_QUE);
            ioctl(dfd, NBD_CLEAR_SOCK);
            ::close(dfd);
        }
        if (nbd_fd >= 0) {
            ::close(nbd_fd);
            nbd_fd = -1;
        }
        loopback_node[0] = '\0';
    }
#else
    int attach_loopback() {
        LOG_ERROR_RETURN(ENOSYS, -1, "nbd loopback_device is Linux-only");
    }
    void disconnect_loopback() {}
#endif
};

NbdDevice* new_nbd_device(const NbdConfig& cfg) {
    if (NbdDeviceImpl::validate(cfg) < 0)
        return nullptr;
    return new NbdDeviceImpl(cfg);
}

}  // namespace blk
}  // namespace photon
