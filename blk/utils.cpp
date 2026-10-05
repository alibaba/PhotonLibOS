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

// utils.cpp: the implementations behind utils.h. Compiled on EVERY platform
// (CMake module blk_utils): section 1 is portable and all-platform nbd.cpp
// calls it, while sections 2 and 3 -- the generic-netlink client and the
// virtio-blk device-model core -- are Linux ABI and sit inside #ifdef __linux__.

#include "utils.h"

#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>   // logging the engine name, a string_view
#include <photon/common/string_view.h>  // std::string_view, incl. the pre-C++17 alias
#include <photon/common/utility.h>      // DEFER
#include <photon/io/fd-events.h>        // wait_for_fd_readable / writable, get_engine_name
#include <photon/thread/thread.h>       // Timeout

#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef __linux__
#include <photon/net/basic_socket.h>

#include <linux/genetlink.h>
#include <linux/netlink.h>
#endif

namespace photon {
namespace blk {

// ===========================================================================
// 1. leaf helpers (all platforms)
// ===========================================================================

int validate_info(const BlkDevInfo& info, bool virtio) {
    if (!info.size)
        LOG_ERROR_RETURN(EINVAL, -1, "device size must be nonzero");
    if (virtio && info.size % 512)
        LOG_ERROR_RETURN(EINVAL, -1, "size ` is not a multiple of 512 (the virtio capacity unit)",
                         info.size);
    if (info.sector_size_shift < 9 || info.sector_size_shift > 12)
        LOG_ERROR_RETURN(EINVAL, -1, "sector_size_shift ` out of [9,12]",
                         (int)info.sector_size_shift);
    if (info.size % (1ull << info.sector_size_shift))
        LOG_ERROR_RETURN(EINVAL, -1, "size ` is not a multiple of the sector size `",
                         info.size, 1ull << info.sector_size_shift);
    return 0;
}

// IFile::zero_range is defined only under __linux__ (fs/virtual-file.cpp), so
// this helper is too: an unguarded definition would reference a symbol that
// does not exist on macOS and break the libphoton link. Its callers (tcmu,
// ublk) are Linux-only anyway.
#ifdef __linux__
int zero_fill(fs::IFile* backend, uint64_t off, uint64_t len) {
    if (backend->zero_range(off, len) == 0)
        return 0;
    if (errno != EOPNOTSUPP && errno != ENOSYS)
        return -1;   // a real failure; zero_range logged it
    static const char zeros[256 << 10] = {};
    while (len) {
        uint64_t k = std::min<uint64_t>(len, sizeof(zeros));
        iovec v{(void*)zeros, (size_t)k};
        if (backend->pwritev(&v, 1, (off_t)off) != (ssize_t)k)
            LOG_ERRNO_RETURN(0, -1, "zero_fill pwritev failed at `, len `", off, k);
        off += k;
        len -= k;
    }
    return 0;
}
#endif  // __linux__

// ----------------------------------------------------------------------------
// the per-device flock: daemon exclusion + orphan detection. The DIRECTORY is
// each controller's (TcmuHBA / UblkController / VduseController, and the socket
// dir for VhostUserController), never the device config's -- one scope per
// controller is what makes an orphan scan and the devices recovered from it agree.
// ----------------------------------------------------------------------------

int validate_scope_dir(const char* dir, const char* what) {
    if (!dir || !*dir)
        LOG_ERROR_RETURN(EINVAL, -1, "a ` directory is required; there is no default, because a shared one would let two applications adopt each other's orphans", what);
    size_t len = strlen(dir);
    if (len >= SCOPE_DIR_BUF)
        LOG_ERROR_RETURN(ENAMETOOLONG, -1, "` directory is too long (` bytes, max `): ",
                         what, len, SCOPE_DIR_BUF - 1, dir);
    return 0;
}

int devlock_acquire(const char* dir, const char* name, int* fd_out) {
    if (::mkdir(dir, 0755) != 0 && errno != EEXIST)
        LOG_ERRNO_RETURN(0, -1, "failed to create lock dir ", dir);
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    int fd = ::open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
        LOG_ERRNO_RETURN(0, -1, "failed to open lock file ", path);
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int e = (errno == EWOULDBLOCK) ? EBUSY : errno;
        ::close(fd);
        if (e != EBUSY)
            LOG_ERROR("flock on ` failed, ", path, ERRNO());
        errno = e;
        return -1;
    }
    *fd_out = fd;
    return 0;
}

void devlock_release(int fd) {
    if (fd < 0) return;
    ::flock(fd, LOCK_UN);
    ::close(fd);
}

int devlock_free(const char* dir, const char* name) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    DEFER(::close(fd));
    bool free_lock = (::flock(fd, LOCK_EX | LOCK_NB) == 0);
    if (free_lock)
        ::flock(fd, LOCK_UN);
    return free_lock ? 1 : 0;
}

int devlock_unlink(const char* dir, const char* name) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (::unlink(path) != 0 && errno != ENOENT)
        LOG_ERRNO_RETURN(0, -1, "failed to remove the tombstone ", path);
    return 0;
}

ssize_t devlock_read_payload(const char* dir, const char* name, void* buf, size_t n) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    DEFER(::close(fd));
    ssize_t r = ::read(fd, buf, n);
    // A read that fails is reported as 0, the "no record" answer, rather than
    // propagated: EISDIR from a directory standing in for the tombstone is the
    // reachable case, and every caller's action on "nothing recorded" and on
    // "unreadable" is the same one -- proceed without the comparison.
    return r < 0 ? 0 : r;
}

int devlock_write_payload(int fd, const void* buf, size_t n) {
    if (fd < 0)
        LOG_ERROR_RETURN(EINVAL, -1, "a tombstone payload needs the fd devlock_acquire returned");
    ssize_t w = ::pwrite(fd, buf, n, 0);
    if (w != (ssize_t)n) {
        if (w >= 0)
            errno = EIO;   // a short write leaves no errno worth printing
        LOG_ERRNO_RETURN(0, -1, "failed to write ` bytes of tombstone payload", n);
    }
    return 0;
}

// Only these two prove that nobody is listening. Everything else a connect can
// report leaves the question open, and an open question answered as "dead" is
// what lets a caller unlink a socket another process is serving.
static bool probe_says_no_listener(int err) {
    return err == ECONNREFUSED || err == ENOENT;
}

// How long a connect may stay pending before the probe gives up. Long enough
// that a listener which is merely slow to accept is still answered, short
// enough that start() does not appear to hang.
static constexpr uint64_t PROBE_TIMEOUT_US = 1000 * 1000;

int unix_listener_live(const char* path) {
    size_t n = strlen(path);
    if (!n || n >= sizeof(sockaddr_un::sun_path))
        LOG_ERROR_RETURN(ENAMETOOLONG, -1, "unix socket path too long: ", path);
    // SOCK_CLOEXEC/SOCK_NONBLOCK are not portable; set them explicitly
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        LOG_ERRNO_RETURN(0, -1, "probe socket() failed");
    DEFER(::close(fd));
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    ::fcntl(fd, F_SETFL, O_NONBLOCK);
    sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    memcpy(un.sun_path, path, n + 1);
    if (::connect(fd, (sockaddr*)&un, sizeof(un)) == 0)
        return 1;
    int err = errno;
    if (probe_says_no_listener(err))
        return 0;
    if (err != EINPROGRESS && err != EAGAIN)
        return -1;   // EACCES on a node whose mode denies us, ENOTDIR, ... Left
                     // unlogged, as devlock_acquire leaves EBUSY: every caller has
                     // an identity to name and this has only a path.
    // A full accept queue is what EAGAIN means here, measured on a listener that
    // was never accepting: the connect is refused this way rather than held, the
    // socket stays unconnected, and its pending error stays clear -- so the answer
    // cannot come from SO_ERROR below, which reads 0 on a socket that getpeername
    // reports as ENOTCONN. It has to come from the refusal itself. Reading EAGAIN
    // as live is also the safe reading if it ever means something else: it refuses
    // a takeover instead of licensing a removal.
    if (err == EAGAIN)
        return 1;
    // A connect genuinely in flight. Running the clock out is NOT the
    // dead-listener answer, so it is refused rather than acted on. EBUSY rather
    // than ETIMEDOUT because that is the refusal blk.h's start() contract
    // documents, and "occupied by something we could not reach" is what a caller
    // can act on.
    if (wait_for_fd_writable(fd, Timeout(PROBE_TIMEOUT_US)) < 0) {
        if (errno == ETIMEDOUT)
            errno = EBUSY;
        return -1;
    }
    err = 0;
    socklen_t el = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0)
        return -1;
    if (err == 0)
        return 1;
    if (probe_says_no_listener(err))
        return 0;
    errno = err;
    return -1;
}

int run_off_vcpu(TempDelegate<int> fn) {
    photon::semaphore sem(0);   // signal() is documented std::thread-safe
    int ret = -1, err = 0;
    std::thread t([&] {
        ret = fn();
        err = errno;
        sem.signal(1);
    });
    sem.wait(1);
    t.join();
    errno = err;
    return ret;
}

void migrate_to_pool(photon::WorkPool* pool, photon::thread* th) {
    if (!pool || !th || pool->get_vcpu_num() == 0)
        return;   // the empty-pool test is load-bearing, not tidiness: WorkPool
                  // resolves an out-of-range index with `vcpu_index++ % size`,
                  // and size == 0 there is a SIGFPE
    if (pool->thread_migrate(th, -1ULL) < 0)
        LOG_WARN("failed to migrate a serving coroutine into the work pool, ", ERRNO());
}

namespace {

struct PoolProbe {
    photon::semaphore done;
    std::string_view ev;
};

// Runs ON the pool vcpu, so what it reports is that vcpu's: it asks the master
// engine this vcpu is currently pointing at to name itself. The view is copied out
// of a frame that dies with this coroutine, which the name's contract allows (see
// MasterEventEngine::get_engine_name). Signals before returning; nothing of the
// caller's is touched after the signal, so the caller may unwind as soon as it
// wakes.
void* pool_probe_thunk(void* a) {
    auto* p = (PoolProbe*)a;
    p->ev = photon::get_vcpu()->master_event_engine->get_engine_name();
    p->done.signal(1);
    return nullptr;
}

// The empty name means no engine is installed at all, which would log as nothing.
std::string_view show_engine(std::string_view name) {
    return name.empty() ? std::string_view("<none>") : name;
}

}   // namespace

int check_pool_engines(photon::WorkPool* pool) {
    if (!pool)
        return 0;
    int n = pool->get_vcpu_num();
    if (n <= 0)
        return 0;
    // Derived from the caller's own vcpu, and like the probe above it needs one:
    // master_event_engine is per-vcpu state, not a process-wide setting.
    const std::string_view need = photon::get_vcpu()->master_event_engine->get_engine_name();
    for (int i = 0; i < n; i++) {
        PoolProbe p;
        auto th = photon::thread_create(&pool_probe_thunk, &p);
        if (!th)
            LOG_ERROR_RETURN(ENOMEM, -1, "cannot create the work pool probe coroutine");
        // An in-range index, so this targets vcpus[i] instead of drawing the
        // round-robin cursor: every vcpu is checked exactly once, which a cursor
        // draw cannot promise.
        if (pool->thread_migrate(th, (size_t)i) < 0)
            LOG_ERRNO_RETURN(0, -1, "cannot reach work pool vcpu `", i);
        p.done.wait(1);
        if (p.ev.empty() || p.ev != need)
            LOG_ERROR_RETURN(EINVAL, -1,
                "work pool vcpu ` cannot host blk serving coroutines: event engine `, need ` (a pool built with the default ev_engine has none, and every fd wait on it fails at once)",
                i, show_engine(p.ev), show_engine(need));
    }
    return 0;
}

#ifdef __linux__

// ===========================================================================
// 2. minimal generic-netlink client
// ===========================================================================

size_t nla_append(char* buf, size_t off, size_t cap, uint16_t type,
                  const void* data, size_t len) {
    size_t total = sizeof(nlattr) + len;
    size_t padded = (total + 3) & ~(size_t)3;
    if (off + padded > cap)
        return 0;
    auto a = (nlattr*)(buf + off);
    a->nla_len = total;
    a->nla_type = type;
    memcpy(buf + off + sizeof(nlattr), data, len);
    memset(buf + off + total, 0, padded - total);
    return padded;
}

size_t nla_append_u32(char* buf, size_t off, size_t cap, uint16_t type, uint32_t v) {
    return nla_append(buf, off, cap, type, &v, sizeof(v));
}

size_t nla_append_u64(char* buf, size_t off, size_t cap, uint16_t type, uint64_t v) {
    return nla_append(buf, off, cap, type, &v, sizeof(v));
}

size_t nla_append_str(char* buf, size_t off, size_t cap, uint16_t type, const char* s) {
    return nla_append(buf, off, cap, type, s, strlen(s) + 1);
}

const void* nla_find(const char* attrs, size_t len, uint16_t type, size_t* plen) {
    for (size_t off = 0; off + sizeof(nlattr) <= len;) {
        auto a = (const nlattr*)(attrs + off);
        if (a->nla_len < sizeof(nlattr) || off + a->nla_len > len)
            break;
        if ((a->nla_type & NLA_TYPE_MASK) == type) {
            if (plen) *plen = a->nla_len - sizeof(nlattr);
            return attrs + off + sizeof(nlattr);
        }
        off += (a->nla_len + 3) & ~(size_t)3;
    }
    return nullptr;
}

void nla_for_each(const char* attrs, size_t len,
                  TempDelegate<bool, uint16_t, const void*, size_t> fn) {
    for (size_t off = 0; off + sizeof(nlattr) <= len;) {
        auto a = (const nlattr*)(attrs + off);
        if (a->nla_len < sizeof(nlattr) || off + a->nla_len > len)
            break;
        if (!fn(a->nla_type & NLA_TYPE_MASK, attrs + off + sizeof(nlattr), a->nla_len - sizeof(nlattr)))
            break;
        off += (a->nla_len + 3) & ~(size_t)3;
    }
}

GenlSock::GenlSock() {
    sk = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (sk >= 0)
        net::set_fd_nonblocking(sk);
}

GenlSock::~GenlSock() {
    if (sk >= 0)
        ::close(sk);
}

// No SCM_RIGHTS: netlink rejects fd-passing cmsgs with a synchronous
// sendmsg EINVAL; fd attributes (NBD_SOCK_FD) need no out-of-band fd
// because the genetlink doit runs synchronously in this sendmsg's
// context and looks the fd up in our fd table, taking its own ref
ssize_t GenlSock::transact(uint16_t family, uint8_t cmd, const char* attrs, size_t attrs_len,
                           char* out, size_t cap) {
    char msg[512];
    size_t payload = sizeof(genlmsghdr) + attrs_len;
    // NLMSG_LENGTH, not payload: the nlmsghdr in front of it is written into msg
    // too, so guarding on payload alone lets attrs_len in (492, 508] overflow
    // msg by up to 16 bytes. Every current caller passes <= 64.
    if (NLMSG_LENGTH(payload) > sizeof(msg))
        LOG_ERROR_RETURN(EMSGSIZE, -EMSGSIZE, "netlink message too large");
    auto nh = (nlmsghdr*)msg;
    nh->nlmsg_len = NLMSG_LENGTH(payload);
    nh->nlmsg_type = family;
    nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nh->nlmsg_seq = ++seq;
    nh->nlmsg_pid = 0;
    auto gh = (genlmsghdr*)NLMSG_DATA(nh);
    gh->cmd = cmd;
    gh->version = 1;
    gh->reserved = 0;
    memcpy((char*)gh + sizeof(genlmsghdr), attrs, attrs_len);

    iovec iov{msg, nh->nlmsg_len};
    msghdr mh{};
    sockaddr_nl addr{};
    addr.nl_family = AF_NETLINK;
    mh.msg_name = &addr;
    mh.msg_namelen = sizeof(addr);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;

    if (photon::wait_for_fd_writable(sk, 3 * 1000 * 1000) < 0)
        LOG_ERROR_RETURN(ETIMEDOUT, -ETIMEDOUT, "netlink socket not writable");
    if (::sendmsg(sk, &mh, 0) < 0)
        LOG_ERRNO_RETURN(0, -errno, "netlink sendmsg failed");

    char rbuf[8192];
    for (int i = 0; i < 16; i++) {
        if (photon::wait_for_fd_readable(sk, 3 * 1000 * 1000) < 0)
            LOG_ERROR_RETURN(ETIMEDOUT, -ETIMEDOUT, "netlink reply timed out");
        ssize_t n = ::recv(sk, rbuf, sizeof(rbuf), 0);
        if (n < 0) {
            if (errno == EAGAIN)
                continue;
            LOG_ERRNO_RETURN(0, -errno, "netlink recv failed");
        }
        for (auto h = (nlmsghdr*)rbuf; NLMSG_OK(h, (size_t)n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_seq != seq)
                continue;
            if (h->nlmsg_type == NLMSG_ERROR) {
                auto err = (nlmsgerr*)NLMSG_DATA(h);
                if (err->error)
                    LOG_ERROR_RETURN(-err->error, err->error, "netlink command ` failed", (int)cmd);
                return 0;  // plain ack
            }
            if (h->nlmsg_type == NLMSG_DONE)
                return 0;
            if (h->nlmsg_type != family)
                continue;
            auto rgh = (genlmsghdr*)NLMSG_DATA(h);
            size_t alen = h->nlmsg_len - NLMSG_LENGTH(sizeof(genlmsghdr));
            if (alen > cap)
                LOG_ERROR_RETURN(EMSGSIZE, -EMSGSIZE, "netlink reply attrs too large: `", alen);
            memcpy(out, (char*)rgh + sizeof(genlmsghdr), alen);
            return (ssize_t)alen;
        }
    }
    LOG_ERROR_RETURN(ETIMEDOUT, -ETIMEDOUT, "no netlink reply received");
}

int GenlSock::request(uint16_t family, uint8_t cmd, const char* attrs, size_t attrs_len,
                      uint16_t want_type, uint32_t* want_out) {
    char abuf[512];
    ssize_t alen = transact(family, cmd, attrs, attrs_len, abuf, sizeof(abuf));
    if (alen < 0)
        return (int)alen;
    if (alen == 0 || !want_out)
        return 0;
    size_t plen = 0;
    const void* p = nla_find(abuf, (size_t)alen, want_type, &plen);
    if (!p)
        LOG_ERROR_RETURN(ENODATA, -ENODATA, "netlink reply lacks expected attr `", (int)want_type);
    if (plen > sizeof(uint32_t))
        LOG_ERROR_RETURN(EPROTO, -EPROTO, "netlink attr ` payload too large: `", (int)want_type, plen);
    // NLA_U16 (e.g. CTRL_ATTR_FAMILY_ID) and NLA_U32 alike
    *want_out = 0;
    memcpy(want_out, p, plen);
    return 0;
}

int GenlSock::resolve_family(const char* name) {
    char attrs[64];
    size_t len = nla_append_str(attrs, 0, sizeof(attrs), CTRL_ATTR_FAMILY_NAME, name);
    if (!len)
        LOG_ERROR_RETURN(EINVAL, -EINVAL, "family name too long");
    uint32_t fid = 0;
    if (request(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, attrs, len, CTRL_ATTR_FAMILY_ID, &fid) < 0)
        return -1;
    return (int)fid;
}

int GenlSock::subscribe(uint32_t group_id) {
    if (::setsockopt(sk, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &group_id, sizeof(group_id)) < 0)
        LOG_ERRNO_RETURN(0, -1, "netlink subscribe group ` failed", group_id);
    return 0;
}

// the group list is a nested attribute (each entry = one group), so a keyed
// nla_find cannot express "the entry whose name matches" -- walk it with
// nla_for_each.
int GenlSock::resolve_mcast_group(const char* family_name, const char* group_name) {
    char attrs[64];
    size_t len = nla_append_str(attrs, 0, sizeof(attrs), CTRL_ATTR_FAMILY_NAME, family_name);
    if (!len)
        LOG_ERROR_RETURN(EINVAL, -EINVAL, "family name too long");
    char rbuf[8192];
    ssize_t alen = transact(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, attrs, len, rbuf, sizeof(rbuf));
    if (alen < 0)
        return (int)alen;
    size_t glen = 0;
    const void* groups = nla_find(rbuf, (size_t)alen, CTRL_ATTR_MCAST_GROUPS, &glen);
    if (!groups)
        LOG_ERROR_RETURN(ENODATA, -ENODATA, "netlink family ` has no multicast groups", family_name);
    uint32_t id = 0;
    nla_for_each((const char*)groups, glen, [&](uint16_t, const void* entry, size_t elen) {
        size_t nl = 0;
        const void* nm = nla_find((const char*)entry, elen, CTRL_ATTR_MCAST_GRP_NAME, &nl);
        if (!nm || strcmp((const char*)nm, group_name) != 0)
            return true;  // not this group; keep walking
        size_t il = 0;
        const void* ip = nla_find((const char*)entry, elen, CTRL_ATTR_MCAST_GRP_ID, &il);
        if (ip && il >= sizeof(uint32_t))
            memcpy(&id, ip, sizeof(id));
        return false;  // matched; stop
    });
    if (!id)
        LOG_ERROR_RETURN(ENODATA, -ENODATA, "netlink family ` has no multicast group `", family_name, group_name);
    return (int)id;
}

int GenlSock::tune_for_notifications() {
    int rcvbuf = 4 << 20;
    if (::setsockopt(sk, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf)) < 0)
        LOG_ERRNO_RETURN(0, -1, "netlink SO_RCVBUFFORCE failed");
    int one = 1;
    if (::setsockopt(sk, SOL_NETLINK, NETLINK_NO_ENOBUFS, &one, sizeof(one)) < 0)
        LOG_ERRNO_RETURN(0, -1, "netlink NETLINK_NO_ENOBUFS failed");
    return 0;
}

int GenlSock::recv_notifications(TempDelegate<void, uint16_t, uint8_t, const char*, size_t> cb) {
    char rbuf[8192];
    int count = 0;
    for (;;) {
        ssize_t n = ::recv(sk, rbuf, sizeof(rbuf), MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return count;
            LOG_ERRNO_RETURN(0, -errno, "netlink recv failed");
        }
        if (n == 0)   // a datagram socket can yield 0 only for a 0-length
            return count;  // datagram (netlink never sends one); don't spin
        for (auto h = (nlmsghdr*)rbuf; NLMSG_OK(h, (size_t)n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_ERROR || h->nlmsg_type == NLMSG_DONE)
                continue;
            auto gh = (genlmsghdr*)NLMSG_DATA(h);
            size_t alen = h->nlmsg_len - NLMSG_LENGTH(sizeof(genlmsghdr));
            cb(h->nlmsg_type, gh->cmd, (const char*)gh + sizeof(genlmsghdr), alen);
            count++;
        }
    }
}

// ===========================================================================
// 3. virtio-blk device-model core
// ===========================================================================

static constexpr int MAX_DESC_CHAIN = 64;

// One of the two byte streams a descriptor chain carries. The role boundaries
// fall INSIDE the streams rather than on descriptor boundaries, so a stream has
// to be addressable by byte offset and not only by element.
struct DescStream {
    iovec iov[MAX_DESC_CHAIN];
    int n = 0;
    uint64_t bytes = 0;

    bool push(void* base, uint32_t len) {
        if (!len)
            return true;   // a zero-length descriptor carries no bytes
        if (n == MAX_DESC_CHAIN)
            return false;
        iov[n].iov_base = base;
        iov[n].iov_len = len;
        n++;
        bytes += len;
        return true;
    }
    // drop the emptied elements at both ends and recount
    void compact() {
        int b = 0, e = n;
        while (b < e && !iov[b].iov_len) b++;
        while (e > b && !iov[e - 1].iov_len) e--;
        if (b || e < n) {
            memmove(iov, iov + b, (e - b) * sizeof(iovec));
            n = e - b;
        }
        bytes = 0;
        for (int i = 0; i < n; i++) bytes += iov[i].iov_len;
    }
    // gather-copy `len` bytes off the front into dst and consume them; false
    // and no change when the stream is shorter than that
    bool take_front(void* dst, size_t len) {
        if (bytes < len)
            return false;
        char* p = (char*) dst;
        for (int i = 0; len && i < n; i++) {
            size_t k = std::min<size_t>(iov[i].iov_len, len);
            memcpy(p, iov[i].iov_base, k);
            p += k;
            len -= k;
            iov[i].iov_base = (char*) iov[i].iov_base + k;
            iov[i].iov_len -= k;
        }
        compact();
        return true;
    }
    // scatter `len` bytes from src into the front of the stream without
    // consuming it; returns how many went in, which is fewer than len only when
    // the stream is shorter than that
    size_t fill(const void* src, size_t len) {
        const char* p = (const char*) src;
        size_t left = std::min<uint64_t>(len, bytes);
        size_t done = 0;
        for (int i = 0; done < left && i < n; i++) {
            size_t k = std::min<size_t>(iov[i].iov_len, left - done);
            memcpy(iov[i].iov_base, p + done, k);
            done += k;
        }
        return done;
    }
    // the last `len` bytes, or nullptr when they are not wholly inside the
    // final element: a status byte split across two descriptors is not
    // something this engine can write through a pointer
    uint8_t* tail(size_t len) {
        if (!n || len > iov[n - 1].iov_len)
            return nullptr;
        return (uint8_t*) iov[n - 1].iov_base + (iov[n - 1].iov_len - len);
    }
    void drop_back(size_t len) {
        if (n && len <= iov[n - 1].iov_len) {
            iov[n - 1].iov_len -= len;
            compact();
        }
    }
};

uint8_t virtio_blk_serve_chain(fs::IFile* backend, bool read_only, bool write_through,
                               bool allow_indirect, const char* serial, const char* tag,
                               const vring_desc* desc, uint16_t head,
                               uint32_t ring_num, uint64_t capacity,
                               VirtioBlkTranslate translate, uint32_t* written) {
    *written = 0;
    // Collect the chain's two byte streams first and split them into roles
    // afterwards. The device-readable stream is the header followed by a
    // WRITE's payload; the device-writable stream is a READ's destination
    // followed by the one-byte status. Nothing requires a driver to start a new
    // descriptor at either boundary, and handing a whole descriptor one role
    // drops whatever shares it -- a WRITE whose payload shared the header's
    // descriptor used to be answered VIRTIO_BLK_S_OK having written nothing.
    DescStream rd, wr;
    uint16_t d = head;
    bool bad = false;
    bool chain_end = false;
    for (int k = 0; k < MAX_DESC_CHAIN; k++) {
        // head and every next are guest-written. MAX_DESC_CHAIN bounds how many
        // steps we take, not where they land: unchecked, `desc[d]` reads up to
        // ~1 MiB past the mapped ring (d is a uint16, a desc is 16 bytes) and
        // then translate() maps whatever address that garbage holds.
        if (d >= ring_num) {
            LOG_ERROR("virtio-blk `: descriptor index ` outside the `-entry ring", tag, d, ring_num);
            bad = true;
            break;
        }
        const vring_desc* de = &desc[d];
        if (de->flags & VRING_DESC_F_INDIRECT) {
            // §2.7.5.3.2: "The device MUST handle the case of zero or more normal
            // chained descriptors followed by a single descriptor with
            // flags&VIRTQ_DESC_F_INDIRECT." So this branch is reachable at any step,
            // not only at head, and the descriptors already walked keep their bytes in
            // the SAME two streams this table's entries go into.
            if (!allow_indirect) {
                LOG_ERROR("virtio-blk `: indirect descriptor, unsupported", tag);
                bad = true;
                break;
            }
            // §2.7.5.3.1: "A driver MUST NOT set both VIRTQ_DESC_F_INDIRECT and
            // VIRTQ_DESC_F_NEXT in flags." Refused rather than served with the NEXT
            // ignored: ignoring it silently drops whatever the driver chained behind
            // the table, and a chain whose tail is missing gets SERVED as a shorter
            // request -- if the dropped tail was the status, wr.tail(1) below hands out
            // the last DATA byte and the request completes one byte short of what the
            // driver asked for.
            if (de->flags & VRING_DESC_F_NEXT) {
                LOG_ERROR("virtio-blk `: indirect descriptor at ` also carries NEXT", tag, d);
                bad = true;
                break;
            }
            // The table's length is a second peer-supplied integer, and the only one
            // that names a whole array rather than one buffer. Both halves are needed:
            // 0 entries is a request with no buffers, which has to be a refusal rather
            // than an empty walk, and a partial trailing entry is not inside the buffer
            // the driver declared -- on a transport whose regions are the whole guest
            // RAM it is inside somebody else's request.
            if (de->len == 0 || de->len % sizeof(vring_desc)) {
                LOG_ERROR("virtio-blk `: indirect table length ` is not a whole number of `-byte entries",
                          tag, de->len, sizeof(vring_desc));
                bad = true;
                break;
            }
            uint32_t n = (uint32_t)(de->len / sizeof(vring_desc));
            // Checked BEFORE the translate: mapping a range only to refuse it is a
            // wasted round trip on a transport whose translate is an ioctl.
            if (n > MAX_INDIRECT_ENTRIES) {
                LOG_ERROR("virtio-blk `: indirect table at ` declares ` entries, the limit is `",
                          tag, d, n, MAX_INDIRECT_ENTRIES);
                bad = true;
                break;
            }
            // Read-only, through the same delegate every other buffer goes through. The
            // table is guest memory this walk is about to READ, and the containment and
            // permission checks that make a translate a translate live inside that
            // delegate -- a private path for tables would bypass both. §2.7.5.3.2's "The
            // device MUST ignore the write-only flag (flags&VIRTQ_DESC_F_WRITE) in the
            // descriptor that refers to an indirect table" is why de->flags is not
            // consulted here.
            const vring_desc* tbl = (const vring_desc*)translate(de->addr, de->len, false);
            if (!tbl) {
                LOG_ERROR("virtio-blk `: unmappable indirect table address ` len `", tag, de->addr, de->len);
                bad = true;
                break;
            }
            // A SECOND index space. `t` is bounded by `n` -- a count our own arithmetic
            // produced from de->len and then clamped -- and never by ring_num: a table
            // entry's next has nothing to do with the ring, and testing it against
            // ring_num passes for values that read past the table. Indexing desc[] with
            // it instead of tbl[] is worse than a read past the end: it serves this
            // request out of ring descriptors the driver never named for it, which is
            // request A's data landing in request B's buffer with nothing in the status
            // to show it.
            //
            // `j` is the step budget and `t >= n` is the index bound, and both are
            // needed: a table whose entries point back at themselves satisfies the index
            // bound forever.
            bool tbl_end = false;
            uint32_t t = 0;
            for (uint32_t j = 0; j < n; j++) {
                if (t >= n) {
                    LOG_ERROR("virtio-blk `: indirect table entry ` outside its ` entries", tag, t, n);
                    bad = true;
                    break;
                }
                const vring_desc* te = &tbl[t];
                if (te->flags & VRING_DESC_F_INDIRECT) {
                    // §2.7.5.3.1: "The driver MUST NOT set the VIRTQ_DESC_F_INDIRECT flag
                    // within an indirect descriptor (ie. only one table per descriptor)."
                    // That is a DRIVER requirement -- §2.7.5.3.2 gives the device no
                    // matching MUST -- so refusing here is our own strictness, and the
                    // reason is the bound: nesting turns one step budget into a product
                    // of budgets, with the depth the peer's.
                    LOG_ERROR("virtio-blk `: nested indirect descriptor at table entry `", tag, t);
                    bad = true;
                    break;
                }
                // Same direction rule and same zero-length substitution as the ring walk
                // above: a zero len asks for one byte, so a zero-length entry gets that
                // answer instead of a vacuous success. Note the pair this forms with the
                // entry-count bound -- it is BECAUSE a zero-length entry still reaches
                // translate that a table of them is the carrier for that bound.
                bool tw = te->flags & VRING_DESC_F_WRITE;
                void* tva = translate(te->addr, te->len ? te->len : 1, tw);
                if (!tva) {
                    LOG_ERROR("virtio-blk `: unmappable indirect buffer address ` len ` writable ` at entry `",
                              tag, te->addr, te->len, (int)tw, t);
                    bad = true;
                    break;
                }
                // The same two streams, so the same depth bound: push() reports full
                // instead of storing past the end of a stack array. A mixed chain spends
                // ONE budget, not two.
                DescStream& ts = tw ? wr : rd;
                if (!ts.push(tva, te->len)) {
                    LOG_ERROR("virtio-blk `: indirect table overflows the `-element scatter list",
                              tag, MAX_DESC_CHAIN);
                    bad = true;
                    break;
                }
                if (!(te->flags & VRING_DESC_F_NEXT)) {
                    tbl_end = true;
                    break;
                }
                t = te->next;
            }
            // tbl_end, NOT chain_end, and the distinction is deliberate: chain_end means
            // "the ring walk reached a descriptor without NEXT", and the refusal below
            // is about the table. Conflating them would let a circular table pass as a
            // finished chain, or a finished chain be reported as a circular table.
            if (!tbl_end && !bad) {
                LOG_ERROR("virtio-blk `: indirect table at ` too long or circular", tag, d);
                bad = true;
            }
            // The table descriptor itself carries NO data: its len bytes ARE the table,
            // and pushing them would hand the descriptor array to pwritev as a WRITE's
            // payload -- the bytes the guest sees would be our own view of its request.
            // This is also why there is no push(de->addr, de->len) anywhere above.
            //
            // `tbl` is not re-validated after the walk, and the walk yields (translate
            // takes a mutex and may ioctl on one transport). That is argued, not tested:
            // the mappings a request holds are released only once no queue has anything
            // in flight, and this request is counted in in_flight from before dispatch
            // returned until handle_req's DEFER runs.
            if (bad)
                break;
            // An indirect descriptor cannot carry NEXT (refused above), so there is
            // nothing after it: the chain is complete.
            chain_end = true;
            break;
        }
        // Read once, used twice: it tells translate which access to expect, and it
        // is the same bit that sorts the descriptor into a stream below. Deriving
        // it in one place is what keeps the permission a mapping is checked
        // against from being able to disagree with the stream the bytes land in.
        bool writable = de->flags & VRING_DESC_F_WRITE;
        void* va = translate(de->addr, de->len ? de->len : 1, writable);
        if (!va) {
            LOG_ERROR("virtio-blk `: unmappable buffer address ` len ` writable `",
                      tag, de->addr, de->len, (int)writable);
            bad = true;
            break;
        }
        DescStream& s = writable ? wr : rd;
        if (!s.push(va, de->len)) {
            LOG_ERROR("virtio-blk `: chain overflows the `-element scatter list", tag, MAX_DESC_CHAIN);
            bad = true;
            break;
        }
        if (!(de->flags & VRING_DESC_F_NEXT)) {
            chain_end = true;
            break;
        }
        d = de->next;
    }
    if (!chain_end && !bad) {
        // longer than MAX_DESC_CHAIN or circular (a buggy/malicious guest can
        // craft either): never serve a truncated chain as success
        LOG_ERROR("virtio-blk `: descriptor chain too long or circular at head `", tag, head);
        bad = true;
    }

    virtio_blk_outhdr hdr{};
    uint8_t* status = nullptr;
    if (!bad) {
        // The status is the writable stream's last byte, and it is located
        // BEFORE the header is parsed so that a request this engine refuses
        // still says so. A driver that leaves its status byte at success reads
        // a refusal the device never wrote as a completed request -- the same
        // silence that answering OK to an unserved WRITE would have been.
        //
        // Only for a chain the walk finished: when it stopped early the last
        // writable byte seen is somewhere in the middle of the chain, and
        // writing IOERR over a data byte would leave the real status untouched.
        status = wr.tail(1);
        if (status)
            wr.drop_back(1);
        // the header is copied out rather than read in place: it may span two
        // descriptors, and a copy is what keeps the fields the bound check
        // below reads identical to the ones the dispatch acts on -- guest
        // memory stays writable while its request is being served
        if (!rd.take_front(&hdr, sizeof(hdr))) {
            LOG_ERROR("virtio-blk `: ` readable bytes leave no room for a `-byte header",
                      tag, rd.bytes, sizeof(hdr));
            bad = true;
        }
    }

    uint32_t data_written = 0;
    uint8_t st = VIRTIO_BLK_S_OK;
    if (bad) {
        st = VIRTIO_BLK_S_IOERR;
    } else {
        uint64_t off = hdr.sector << 9;   // virtio sectors are always 512B
        // what is left of each stream is payload: the WRITE's in the readable
        // one, a READ's or a GET_ID's in the writable one
        DescStream& data = (hdr.type == VIRTIO_BLK_T_OUT) ? rd : wr;
        uint64_t want = data.bytes;
        bool lba = (hdr.type == VIRTIO_BLK_T_IN || hdr.type == VIRTIO_BLK_T_OUT);
        // Bytes left in the stream this request type does not carry data in
        // describe a buffer with no defined meaning: a WRITE with writable
        // bytes left over would have pwritev() run over memory the guest never
        // filled in, and a READ or GET_ID with readable ones would silently
        // drop what the guest did fill in. Refuse rather than guess which the
        // driver meant. A FLUSH and an unknown type have no data phase at all,
        // so for those neither stream is stray and the bytes are ignored.
        uint64_t stray = (hdr.type == VIRTIO_BLK_T_OUT) ? wr.bytes
                       : (hdr.type == VIRTIO_BLK_T_IN || hdr.type == VIRTIO_BLK_T_GET_ID) ? rd.bytes
                       : 0;
        // Bound the two ops that carry an LBA, and compare in SECTORS:
        // hdr.sector is a full 64 bits, so `sector << 9` can wrap back into
        // range (the same trick tcmu's lba_to_off guards against). capacity is
        // a multiple of 512 -- validate_info(virtio=true) enforces it.
        //
        // A read past the end is harmless (it comes back short and turns into
        // IOERR anyway), but a WRITE past EOF on a regular-file backend
        // EXTENDS it, so without this a guest grows the image without bound.
        // tcmu (out_of_bounds) and nbd (oob) both gate it; this core did not.
        bool oob = lba && (hdr.sector > (capacity >> 9) || want > capacity - (hdr.sector << 9));
        if (stray) {
            LOG_ERROR("virtio-blk `: request type ` carries ` bytes the wrong way",
                      tag, hdr.type, stray);
            st = VIRTIO_BLK_S_IOERR;
        } else if (lba && (want & 511)) {
            // the LBA and the backend offset are both in 512-byte units, so a
            // payload that is not a whole number of sectors names a range this
            // engine cannot express without writing outside the sectors named
            LOG_ERROR("virtio-blk `: request type ` carries ` bytes, not a whole sector count",
                      tag, hdr.type, want);
            st = VIRTIO_BLK_S_IOERR;
        } else if (oob) {
            LOG_ERROR("virtio-blk `: ` bytes at sector ` past the `-byte end",
                      tag, want, hdr.sector, capacity);
            st = VIRTIO_BLK_S_IOERR;
        } else switch (hdr.type) {
        case VIRTIO_BLK_T_IN: {
            ssize_t r = data.n ? backend->preadv(data.iov, data.n, (off_t)off) : 0;
            if (r == (ssize_t)want)
                data_written = (uint32_t)r;
            else {
                if (r < 0)
                    LOG_ERROR("virtio-blk `: read backend failed, off `, ", tag, off, ERRNO());
                st = VIRTIO_BLK_S_IOERR;
            }
            break;
        }
        case VIRTIO_BLK_T_OUT: {
            if (read_only) { st = VIRTIO_BLK_S_IOERR; break; }
            ssize_t w = data.n ? backend->pwritev(data.iov, data.n, (off_t)off) : 0;
            if (w != (ssize_t)want) {
                if (w < 0)
                    LOG_ERROR("virtio-blk `: write backend failed, off `, ", tag, off, ERRNO());
                st = VIRTIO_BLK_S_IOERR;
                break;   // nothing was persisted, so there is nothing to persist
            }
            // Write-through: the bytes have to be on stable storage before this
            // returns, because the caller publishes the completion next and a
            // driver that did not negotiate FLUSH will never send one to ask.
            // A failed persist is reported, not logged and dropped -- the
            // completion is the device's word that the data is where it promised.
            //
            // fdatasync rather than pwritev2(RWF_DSYNC): pwritev2 is not pure
            // virtual, and the base implementation discards `flags` and forwards
            // to pwritev, so a backend that does not override it would silently
            // turn this back into a cached write. fdatasync is pure virtual, so
            // every backend answers it. It costs a second syscall, and one that
            // waits for the whole file's dirty data rather than only this range,
            // which is why a device that can afford write-back offers FLUSH and
            // lets the driver batch.
            if (w > 0 && write_through && backend->fdatasync() < 0) {
                LOG_ERROR("virtio-blk `: write-through persist failed, off `, ", tag, off, ERRNO());
                st = VIRTIO_BLK_S_IOERR;
            }
            break;
        }
        case VIRTIO_BLK_T_FLUSH:
            if (backend->fdatasync() < 0) {
                LOG_ERROR("virtio-blk `: flush failed, ", tag, ERRNO());
                st = VIRTIO_BLK_S_IOERR;
            }
            break;
        case VIRTIO_BLK_T_GET_ID: {
            // The ID is a fixed-width field: a guest that offered
            // VIRTIO_BLK_ID_BYTES of writable buffer expects all of it back,
            // NUL past the end of the serial. Writing only strlen(serial)
            // leaves that tail holding whatever the guest prefilled there, and
            // reports a used length stopping short of the field it asked about.
            //
            // A naive memcpy truncated to 20 bytes gives distinct devices the
            // same ID when their identities share a prefix (e.g. two sockets
            // in the same directory). Hash the full identity with FNV-1a into
            // a hex string that fills the field, so every distinct input
            // produces a distinct serial with overwhelming probability.
            char id[VIRTIO_BLK_ID_BYTES] = {};
            uint64_t h = 14695981039346656037ULL;   // FNV offset basis
            for (const char* p = serial; *p; p++) {
                h ^= (uint8_t)*p;
                h *= 1099511628211ULL;             // FNV prime
            }
            snprintf(id, sizeof(id), "%016llx", (unsigned long long)h);
            data_written = (uint32_t) data.fill(id, sizeof(id));
            break;
        }
        default:
            LOG_WARN("virtio-blk `: unsupported request type `", tag, hdr.type);
            st = VIRTIO_BLK_S_UNSUPP;
            break;
        }
    }
    if (status) {
        *status = st;
        data_written += 1;
    }
    *written = data_written;
    return st;
}

uint16_t vring_avail_idx(const vring_avail* avail) {
    return __atomic_load_n(&avail->idx, __ATOMIC_ACQUIRE);
}

uint16_t vring_used_idx(const vring_used* used) {
    return __atomic_load_n(&used->idx, __ATOMIC_ACQUIRE);
}

// The notification predicate for a ring WITHOUT VIRTIO_RING_F_EVENT_IDX. Once
// that is negotiated, avail->flags' low bit must be ignored (§2.7.7.2) and
// should_notify() takes over; this stays as the fallback half.
bool vring_need_irq(const vring_avail* avail) {
    return !(__atomic_load_n(&avail->flags, __ATOMIC_ACQUIRE) & VRING_AVAIL_F_NO_INTERRUPT);
}

uint16_t vring_used_append(vring_used* used, uint16_t used_idx, uint32_t num,
                           uint16_t id, uint32_t len) {
    used->ring[used_idx % num].id = id;
    used->ring[used_idx % num].len = len;
    uint16_t next = (uint16_t)(used_idx + 1);
    __atomic_store_n(&used->idx, next, __ATOMIC_RELEASE);
    return next;
}

uint16_t vring_used_event(const vring_avail* avail, uint32_t num) {
    return __atomic_load_n(&avail->ring[num], __ATOMIC_ACQUIRE);
}

void vring_set_avail_event(vring_used* used, uint32_t num, uint16_t v) {
    // the slot past the last used element is a uint16 that struct vring_used
    // does not name; the uapi spells it *(__virtio16 *)&used->ring[num]
    __atomic_store_n((uint16_t*)&used->ring[num], v, __ATOMIC_RELEASE);
}

uint16_t vring_avail_event(const vring_used* used, uint32_t num) {
    return __atomic_load_n((const uint16_t*)&used->ring[num], __ATOMIC_ACQUIRE);
}

bool vring_need_event(uint16_t event_idx, uint16_t new_idx, uint16_t old) {
    return (uint16_t)(new_idx - event_idx - 1) < (uint16_t)(new_idx - old);
}

// ----------------------------------------------------------------------------
// VirtQueueServer
// ----------------------------------------------------------------------------

void* VirtQueueServer::req_trampoline(void* a) {
    auto arg = (ReqArg*)a;
    arg->q->handle_req(arg->head, arg->gen);
    delete arg;
    return nullptr;
}

void VirtQueueServer::set_ring(vring_desc* d, vring_avail* a, vring_used* u, uint32_t n) {
    desc = d;
    avail = a;
    used = u;
    num = n;
    // Written before the bump and released by it, so a request whose acquire
    // load sees the new generation also sees the ring that goes with it. The
    // other direction needs no ordering: a request still holding the old
    // generation declines, which is what the token is for.
    generation.fetch_add(1, std::memory_order_release);
}

void VirtQueueServer::clear_ring() {
    set_ring(nullptr, nullptr, nullptr, 0);
}

bool VirtQueueServer::should_notify(uint16_t old_used_idx) {
    // BOTH notification modes need this, so it precedes the branch instead of
    // living in the EVENT_IDX arm. It pairs with the driver's barrier ("before
    // reading flags or avail_event, to avoid missing a notification"): our
    // used-ring write has to be visible before we read what the driver wrote.
    // Without it the two sides interleave -- we publish used and read a stale
    // flags/avail_event while the driver enables notifications and reads a stale
    // used -- and neither notifies, so a completed request goes unreported until
    // the next one arrives. A release store on used plus an acquire load on flags
    // does not close that: the hazard is a store/load pair across two locations,
    // which only a full fence orders. The spec imposes no device-side barrier
    // MUST -- this ordering argument is ours, see SPEC §3.3 -- so it cannot be
    // dropped on the strength of a citation.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!event_idx.load(std::memory_order_relaxed))
        return vring_need_irq(avail);
    // First decision on this ring notifies unconditionally. §2.7.7.1: "The
    // driver MUST handle spurious notifications from the device." It exists for
    // handover: a resumed ring starts used_idx at an arbitrary value while
    // used_event is whatever the driver left behind for the previous daemon, and
    // those two need not satisfy §2.7.7.2's equality -- without this the first
    // completion after adoption would never be reported.
    if (!notify_valid.load(std::memory_order_relaxed)) {
        notify_valid.store(true, std::memory_order_relaxed);
        return true;
    }
    return vring_need_event(vring_used_event(avail, num), used_idx, old_used_idx);
}

void VirtQueueServer::publish_avail_event() {
    if (event_idx.load(std::memory_order_relaxed))
        vring_set_avail_event(used, num, last_avail);
}

void VirtQueueServer::loop() {
    while (run.load(std::memory_order_relaxed) && !stopping.load(std::memory_order_relaxed)) {
        hooks.tick.fire();
        if (!hooks.ready.fire()) {
            photon::thread_usleep(1000);
            continue;
        }
        // No kickfd (a SET_VRING_KICK that carried NOFD) leaves no event source, so
        // the fallback re-poll below is the only way to make progress. Waiting on fd
        // -1 returns at once WITHOUT yielding -- it would hot-spin this vcpu and
        // starve every other coroutine on it (this queue's own request coroutines,
        // plus a transport's message and accept loops wherever the control plane
        // shares the vcpu), and log an error every pass -- so sleep the same
        // KICK_FALLBACK_US budget instead.
        int wr = 0;
        if (kickfd >= 0)
            wr = photon::wait_for_fd_readable(kickfd, Timeout(KICK_FALLBACK_US));
        else
            photon::thread_usleep(KICK_FALLBACK_US);
        if (!run.load(std::memory_order_relaxed) || stopping.load(std::memory_order_relaxed))
            break;
        if (wr < 0 && errno != ETIMEDOUT && errno != EINTR)
            LOG_WARN("` virtqueue: kickfd wait failed, ", tag, ERRNO());
        // Re-check readiness AFTER the yield, not just before it: the ring can be
        // invalidated while we slept, and dispatch_avail below dereferences avail
        // unconditionally. How much this check carries depends on the transport.
        // vhost-user quiesces the loop -- joins it and drains its requests -- before
        // SET_VRING_NUM / SET_VRING_ADDR retranslate and before anything nulls the
        // ring, so no mutation of those can land inside the window this covers;
        // there it is defence in depth, and what it does catch is the two writers
        // that deliberately clear `enabled` BEFORE quiescing, so that the loop stops
        // taking new work while the teardown is still being set up (its
        // handle_mem_table, and stop_session when the caller did not ask to drain
        // the backlog). Every other disable path clears `run` at the same time, and
        // the `run` test above breaks the loop before it reaches here. vduse's
        // message loop cannot quiesce -- the kernel blocks whoever sent a message
        // until it is answered and gives up after msg_timeout seconds, so that loop
        // may not wait for the requests this one dispatched. It settles the same
        // conflict with a per-queue generation counter instead: an invalidation
        // bumps the counter before it clears `ready`, and a refresh that finds it
        // bumped withholds its own publish, so the clear is the last word. What
        // this re-check catches there is an invalidation that lands inside the
        // kickfd wait, plus stop_serving when the caller did not ask to drain the
        // backlog. Its hooks.tick is the one thing that can yield (the iotlb cache
        // takes a mutex), but tick runs before the first readiness test, so
        // dispatch_avail is still yield-free and one re-check still covers the
        // whole dispatch that follows.
        if (!hooks.ready.fire())
            continue;
        uint64_t n;
        // drain the counter, which ends on EAGAIN -- hence the O_NONBLOCK
        // contract on kickfd (utils.h). On a blocking fd this second read parks
        // the vcpu until the next kick, and the wait_for_fd_readable timeout
        // above never gets to fire. Skipped entirely with no kickfd: read(-1)
        // would just set EBADF every pass, which a later ERRNO() could pick up.
        if (kickfd >= 0)
            while (::read(kickfd, &n, sizeof(n)) == (ssize_t)sizeof(n))
                ;
        dispatch_avail();
        if (event_idx.load(std::memory_order_relaxed)) {
            // Close a store-load race, not an optimization. The invariant above
            // means avail_event already equals last_avail; what is left is
            // ordering our publish before our re-read of avail->idx. Without a
            // fence the store buffer lets the peer see these two the other way
            // round, and the interleaving "driver reads a stale avail_event so it
            // does not kick / we read a stale avail->idx so we sleep" leaves that
            // buffer unannounced. KICK_FALLBACK_US caps that at 5 ms per silent
            // period but does not eliminate the missed notification, so the fence
            // still cannot be dropped. It also cannot be justified by coverage:
            // no test that reaches this loop has a timing assertion. This engine
            // is shared only by the vhost-user and vduse transports, and neither
            // suite measures elapsed time; the tcmu and ublk suites do, but they
            // never enter here, and their thresholds are 50 ms and up. The fence
            // rests on the ordering argument alone. Full argument in SPEC §3.3.
            //
            // The fence sits INSIDE the loop, before every read, not once before
            // a single recheck: a read that can be followed by a sleep has to be
            // ordered after every publish we have done. A single recheck fails
            // that -- when the recheck does dispatch, dispatch_avail's own last
            // read of avail->idx follows its last publish with no fence between,
            // so we can sleep on an unordered pair and the window reopens.
            //
            // The progress guard is what keeps this from being a livelock.
            // dispatch_avail has two early returns that leave last_avail
            // unadvanced -- the in-flight cap and a failed thread_create -- and
            // neither this loop nor dispatch_avail yields (see the comment above:
            // the kickfd wait is loop()'s only yield point). in_flight is
            // decremented only by handle_req's DEFER in another coroutine, which
            // cannot be scheduled while we spin, so a loop without this guard
            // spins forever once either early return fires. Breaking on no
            // progress hands the remainder to the outer loop, whose yielding
            // KICK_FALLBACK_US re-dispatch is exactly the recovery the cap's own
            // comment documents.
            //
            // No readiness or teardown re-check inside. run and stopping are
            // atomic, written by teardown which may be on another vcpu;
            // re-reading them here would still be in the wrong place to help,
            // because the avail->idx read below is what would dereference an
            // invalidated ring and it comes first. What actually stops this loop
            // is wake() + interrupt + join, which teardown performs -- the
            // atomics guarantee an untorn read, never a wakeup. The ready hook
            // is a pure field read, and the fields it reads are invalidated
            // only by a message-loop retranslate. Same guarantee the
            // unconditional dispatch just above already runs under.
            for (;;) {
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
                if (vring_avail_idx(avail) == last_avail)
                    break;
                uint16_t before = last_avail;
                dispatch_avail();
                if (last_avail == before)
                    break;
            }
        }
    }
}

void VirtQueueServer::dispatch_avail() {
    for (;;) {
        uint16_t aidx = vring_avail_idx(avail);
        if (last_avail == aidx)
            return;
        // avail->idx is a guest-written free-running counter, so this loop needs
        // a bound of its own: without one a single kick spawns up to 65535
        // coroutines. `num` is the outer bound -- every chain consumes at least
        // one descriptor, so a correct driver can never exceed it (an indirect
        // chain consumes exactly one: its table is not in the ring) -- and
        // queue_depth is the caller's, when the caller set one. Taking the lesser
        // is what makes BlkConfig::queue_depth mean here what it already means in
        // tcmu and nbd; without it the two virtio transports bound concurrency by
        // the ring the PEER chose, which on vhost-user is the frontend's
        // SET_VRING_NUM and has nothing to do with what was asked for. Recomputed
        // every pass rather than cached, because a frontend may resize the ring
        // under a live device. Anything left pending stays pending (last_avail is
        // not advanced): redispatch_backlog takes it as soon as a slot frees, and
        // loop()'s KICK_FALLBACK_US re-read is the backstop behind that.
        uint32_t cap = num;
        if (queue_depth && queue_depth < cap)
            cap = queue_depth;
        if (in_flight.load() >= cap)
            return;
        uint16_t head = avail->ring[last_avail % num];
        // Commit last_avail and in_flight only once the create has succeeded.
        // handle_req's DEFER is the only thing that ever decrements in_flight,
        // so a failed create (photon_thread_create returns nullptr when the
        // stack allocation does) would leave it permanently above 0: drain()
        // and teardown's pre-join drain would spin forever, and once in_flight
        // reached num nothing would ever dispatch again. Returning without
        // advancing leaves the chain available for the next pass -- the same
        // recovery the cap above relies on.
        // The engine's one allocation on a request's behalf, and dispatch's rather
        // than serving's: test-blk-vq.cpp's "serving allocates nothing" budget is
        // about the chain walk into serve_chain's own DescStream, not this. Freed by
        // req_trampoline, or below if the create fails.
        auto* arg = new ReqArg{this, head, generation.load(std::memory_order_acquire)};
        if (!photon::thread_create(&VirtQueueServer::req_trampoline, arg, stack_size)) {
            delete arg;
            LOG_ERROR("` virtqueue: cannot create the request coroutine for head `, leaving it available",
                      tag, head);
            return;
        }
        last_avail++;
        in_flight++;
        // Per consumed head, not per batch: §2.7.10.1 has the driver decide at
        // the instant it writes a descriptor, and it MUST NOT notify unless the
        // index it wrote equals avail_event. Batching the publish would leave a
        // window where avail_event < last_avail, and a buffer the driver puts in
        // slot `last_avail` during that window is legitimately never announced
        // to us -- not a driver bug, a stale value we published. So
        // avail_event == last_avail is an invariant; the cost is one 2-byte
        // store per descriptor.
        publish_avail_event();
    }
}

// A completion is the only event that can free a slot under a binding cap, and by
// then loop() is parked in its kickfd wait until either a kick arrives or
// KICK_FALLBACK_US expires. The chains the cap held back are not the peer's to
// re-announce -- the driver already put them in the ring and already kicked, and
// under EVENT_IDX it will not kick again for a buffer whose index sits behind the
// avail_event we published -- so without this the fallback re-read is the whole
// recovery: 5 ms per request, which at a depth of 1 is a ceiling of roughly 200
// requests a second that no backend slowness explains.
//
// Dispatching rather than wake()-ing the loop, so this costs no syscall and still
// works when there is no kickfd at all -- a SET_VRING_KICK that carried NOFD, which
// is exactly the configuration loop() has no other event source in. It rests on the
// single-vcpu invariant complete_req already rests on: request coroutines are
// created by dispatch_avail on the loop's vcpu and nothing migrates them, so this
// reads last_avail where the loop writes it. dispatch_avail is yield-free and
// thread_create only queues, so there is no recursion -- the coroutine this admits
// runs after this one has unwound.
void VirtQueueServer::redispatch_backlog() {
    // `stopping` and `run` are what keep a teardown honest: one that did not ask to
    // drain must leave the backlog STRANDED, which is the documented handover --
    // whoever serves next resumes from used->idx -- and filling a slot here would
    // take it back. `run` is the same flag loop() gates on, so a queue the
    // transport has stopped cannot be dispatched into by a completion either.
    //
    // No generation check, deliberately. This is loop()'s own dispatch precondition
    // and no stricter: ready is what says the ring now published is one that may be
    // dispatched into, and `avail` non-null is what makes reading it safe. Adding
    // the token here would decline a redispatch after a retranslate that republished
    // a perfectly good ring -- the request that just completed belonged to the OLD
    // one, which is why handle_req checks it, but the chains still pending belong
    // to whichever ring is published now, exactly as they would on loop()'s next
    // pass.
    if (!run.load(std::memory_order_relaxed) || stopping.load(std::memory_order_relaxed) || !avail)
        return;
    if (!hooks.ready.fire())
        return;
    if (vring_avail_idx(avail) != last_avail)
        dispatch_avail();
}

void VirtQueueServer::handle_req(uint16_t head, uint64_t gen) {
    // Reverse order, and the order is the point: DEFER guards run last-declared
    // first, so this one fires AFTER the decrement below. Free the slot, then let
    // whoever takes it look at a count that says it is free.
    DEFER(redispatch_backlog());
    DEFER(in_flight--);
    // What neither gate consults is `hooks.ready`, and that absence is the point.
    // ready answers "may the loop take more work from this ring", and false does
    // NOT imply the ring is gone: a frontend pausing a queue clears it with the
    // ring published and every mapping intact, and so does the quiesce a transport
    // runs around an interrupt-fd swap. A request standing here holds a chain
    // dispatch_avail already consumed: last_avail moved past it, and no driver
    // re-publishes a buffer it is still waiting on. Declining it therefore loses it
    // outright -- the backend IO lands, the status byte is written, used->idx never
    // advances, and the frontend waits for an interrupt that cannot come.
    // Re-enabling does not recover it either, because the restart re-derives
    // last_avail from the used ring and the wrap-safe comparison there refuses to
    // rewind it.
    //
    // The two facts that DO retire a request are the ones tested below. `stopping`
    // is this engine's own teardown, set before the transport unmaps anything the
    // request holds. The generation says the ring this request was dispatched
    // against is still the published one: every path that nulls or replaces those
    // four fields goes through set_ring()/clear_ring(), and both bump it. A ring
    // that is gone declines here; a ring that is merely paused does not.
    //
    // This gates serve_chain and not only the completion below, because this
    // coroutine was queued by dispatch_avail and may run long after. serve_chain
    // bounds the descriptor INDEX against num but cannot tell a null desc from a
    // valid one; no transport assigns those three directly -- set_ring() and
    // clear_ring() are the only writers -- so the bump above is also what retires a
    // request in front of a null one, and no separate validity hook is needed.
    // (`num` is the exception: a transport may write it directly, and then the
    // engine is relying on that transport to have quiesced this queue first, since
    // num bounds serve_chain's descriptor index and divides in vring_used_append.
    // Both current direct writers do -- a ring resize quiesces in its own handler,
    // and a session reset's callers quiesce every queue before they call it.)
    // Leaving a request uncompleted is the documented handover; `stopping`'s
    // declaration in utils.h carries why the next daemon cannot recover it from
    // the ring, and what the two virtio transports each do about that.
    if (stopping.load(std::memory_order_relaxed) ||
        gen != generation.load(std::memory_order_acquire))
        return;
    uint32_t written = 0;
    virtio_blk_serve_chain(backend, read_only,
                           write_through.load(std::memory_order_relaxed),
                           indirect_desc.load(std::memory_order_relaxed),
                           serial, tag, desc, head,
                           num, capacity.load(std::memory_order_relaxed),
                           hooks.translate, &written);
    // and again after: serve_chain yields inside preadv/pwritev, and complete_req
    // dereferences used and avail. Same two facts, for the same reason -- a ring
    // retired while this request was inside the backend is not one it may publish
    // into, whatever the transport's dispatch gate now says.
    if (stopping.load(std::memory_order_relaxed) ||
        gen != generation.load(std::memory_order_acquire))
        return;   // tearing down, or the ring this request came from is gone
    complete_req(head, written);
}

void VirtQueueServer::complete_req(uint16_t head, uint32_t written) {
    // vring_used_append overwrites used_idx, and §2.7.7.2's rule is about the
    // value BEFORE the increment -- the one that picked the slot this element
    // landed in. Keep it first.
    uint16_t old = used_idx;
    used_idx = vring_used_append(used, used_idx, num, head, written);
    if (should_notify(old))
        hooks.notify.fire();
}

void VirtQueueServer::drain() {
    while (in_flight.load())
        photon::thread_usleep(1000);
}

void VirtQueueServer::wake() {
    if (kickfd < 0)
        return;
    uint64_t one = 1;
    ssize_t w = ::write(kickfd, &one, sizeof(one));
    (void)w;
}

#endif  // __linux__

}  // namespace blk
}  // namespace photon
