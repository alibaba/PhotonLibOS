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

// vduse-cli: the vduse diagnostic and recovery assistant -- three subcommands in
// one program, sharing the ABI plumbing (the lazy IOTLB, the vring refresh, the
// virtio-blk descriptor walk, the kernel message replies).
//
//   vduse-cli probe                   create a virtio-blk VDUSE device of its own
//                                     ("vdprobe"), attach it to the vdpa bus, serve
//                                     it, and dump the whole kernel<->daemon
//                                     interaction
//   vduse-cli rescue <name> [secs]    adopt an EXISTING registration nobody serves
//                                     and drain its backlog (default 60s)
//   vduse-cli destroy <name>          VDUSE_DESTROY_DEV, without serving it
//
// <name> is a vduse registration (an entry of `ls /dev/vduse`), not a /dev/vdX.
//
// WHY THESE EXIST. The vduse uapi is thinly documented and its behaviour (message
// ordering, the lazy IOTLB, what a reset does to the ring counters) had to be
// measured rather than assumed: probe was written while blk/vduse.cpp was being
// designed -- re-run it against a new kernel BEFORE changing vduse.cpp's protocol
// code. And a registration with no daemon serving it WEDGES anything that touches
// its /dev/vdX in unkillable D state: the partition scan waits on a folio only the
// daemon can complete, and the kernel has no timeout for it. rescue + destroy are
// the drill for that, for the case where no daemon can start at all --
// blk/vduse.cpp's own recovery path (start() on an orphan) is the productized
// rescue.
//
// MEASURED RECOVERY ORDER for a wedged device:
//   1. vduse-cli rescue <name> &   adopt and serve the backlog; the D-state
//                                  consumers (vdpa dev add, udev-worker) clear
//   2. vdpa dev del <name>         detach the consumer -- on a half-attached device
//                                  this can TIME OUT (rc=124), which is fine
//   3. kill the rescue daemon (DESTROY_DEV is EBUSY while a daemon is still
//      connected), then `vduse-cli destroy <name>`: destroying the registration
//      also takes the half-attached vdpa device with it, so this is the step that
//      actually clears the residue
// Clean state afterwards: `ls /dev/vduse` shows only `control` and `vdpa dev show`
// is empty. SIGKILL is not recoverable without the rescue subcommand.
//
// HAZARD (probe): it can block for MINUTES inside VDUSE_IOTLB_GET_FD for a
// request's DATA buffer while `vdpa dev add` sits in D state waiting for that very
// request (measured ~200s on kernel 7.0.0-30, with kicks=0 every time -- the
// kickfd never fired, the same anomaly blk/vduse.cpp's 5ms fallback poll exists
// for; that stall is the environment, not a regression in this file). SIGTERM and
// SIGINT are caught and tear the device down, but SIGKILL leaves the registration
// UNSERVED and the VM wedged. Give it a generous timeout and never -9 it.
//
// Assistant program, deliberately NOT a build target: built by hand, from the repo
// root, against a photon build (only alog is used, and alog needs no
// photon::init() -- it stamps its own lines from photon's clock and writes to
// stdout):
//     g++ -O2 -Wall -I include -o vduse-cli blk/test/vduse-cli.cc build/output/libphoton.a -lpthread -ldl
// Any build dir's libphoton.a works; linking the shared libphoton.so instead needs
// -L that dir plus an rpath. Run as root, with both modules loaded as SEPARATE
// commands (`modprobe a b` passes b as a PARAMETER of a) and the iproute2 vdpa
// tool present:
//     modprobe vduse; modprobe virtio_vdpa
//
// As a root-only diagnostic, serve_vq() below is DELIBERATELY laxer than
// blk/utils.cpp's virtio_blk_serve_chain: it caps the descriptor walk at 32 steps
// but never bounds the indices against the ring size, and it drains the avail ring
// with no in-flight cap. The library had to fix both -- a guest-written index
// reads past the mapping, and an uncapped drain fans out one coroutine per chain.
// Here the peer is the local kernel's virtio_vdpa driver and the operator is root,
// so a malformed ring means the diagnosis was needed in the first place. Do not
// copy this walk into the library.
//
// C++, unlike the three .c files it replaces. <linux/virtio_ring.h> cannot be
// included from C++ (its inline vring_init() assigns void* to typed pointers), so
// the split-ring structs are copied verbatim below, exactly as blk/utils.h does;
// <linux/vduse.h>, <linux/virtio_blk.h> and <linux/virtio_config.h> are C++-safe.
// No `#define _GNU_SOURCE`: g++ already defines it on the command line.

#include <photon/common/alog.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/vduse.h>
#include <linux/virtio_blk.h>
#include <linux/virtio_config.h>

// ---------------------------------------------------------------------------
// split vring (verbatim from <linux/virtio_ring.h>, which C++ cannot include)
// ---------------------------------------------------------------------------

struct vring_desc {
    uint64_t addr;    // guest/IOVA address -- resolved through VDUSE_IOTLB_GET_FD
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
#define VRING_DESC_F_NEXT     1
#define VRING_DESC_F_WRITE    2
#define VRING_DESC_F_INDIRECT 4

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};

struct vring_used_elem {
    uint32_t id;     // descriptor chain head
    uint32_t len;    // bytes written into device-writable buffers
};
struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
};

#define PROBE_NAME "vdprobe"

enum Mode {
    PROBE,      // our own device: measure and narrate everything
    RESCUE,     // somebody else's wedged device: adopt, drain, say as little as
                // possible -- and never destroy it (it is not ours)
};
static Mode mode = PROBE;

static int ctrl = -1, dev_fd = -1, kickfd = -1;

// ---- vring state (a single vq) ----
static unsigned vq_num;
static struct vring_desc *desc;
static struct vring_avail *avail;
static struct vring_used *used;
static unsigned last_avail, used_idx;
static int vq_live;
static unsigned msgs, driver_ok_seen;

// seconds since the first call: the probe's own deadline base (alog timestamps
// the trace, so this is only for the loop's budget)
static double now() {
    static long long t0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    long long t = ts.tv_sec * 1000000000LL + ts.tv_nsec;
    if (!t0) t0 = t;
    return (t - t0) / 1e6;
}

// system() is warn_unused_result; the probe's shell steps are best-effort and
// report through their own captured output, so swallow the status here instead of
// sprinkling casts (which gcc ignores for this attribute anyway)
static void sh(const char *cmd) {
    if (system(cmd)) { }
}

// the one thing that is not a log: the help text, unadorned on stderr
static void usage() {
    fprintf(stderr,
        "usage: vduse-cli probe                  create, serve and dump its own \"" PROBE_NAME "\" device\n"
        "       vduse-cli rescue <name> [secs]   adopt a wedged registration and drain it (default 60)\n"
        "       vduse-cli destroy <name>         VDUSE_DESTROY_DEV without serving\n"
        "       <name> is a vduse registration (ls /dev/vduse), not a /dev/vdX\n");
}

// ---------------------------------------------------------------------------
// the shared ABI plumbing
// ---------------------------------------------------------------------------

// Resolve an IOVA through the lazy IOTLB and map it. The mapping is leaked on
// purpose: both subcommands are throwaway, and a request's buffer can arrive
// while the consumer is blocked waiting for it, so there is no safe moment to
// unmap.
static void *map_iova(unsigned long long iova, size_t need, bool quiet) {
    struct vduse_iotlb_entry e;
    memset(&e, 0, sizeof(e));
    e.start = iova; e.last = iova + need - 1;
    int fd = ioctl(dev_fd, VDUSE_IOTLB_GET_FD, &e);
    if (fd < 0) {
        if (!quiet)
            LOG_ERROR("IOTLB GET_FD [0x`, 0x`) failed, ", HEX(iova), HEX(iova + need), ERRNO());
        return nullptr;
    }
    size_t sz = e.last - e.start + 1;
    int prot = PROT_READ | ((e.perm & VDUSE_ACCESS_WO) ? PROT_WRITE : 0);
    void *base = mmap(nullptr, sz, prot, MAP_SHARED, fd, e.offset);
    close(fd);
    if (base == MAP_FAILED) {
        if (!quiet)
            LOG_ERROR("mmap of the IOTLB fd failed, ", VALUE(sz), ERRNO());
        return nullptr;
    }
    if (!quiet)
        LOG_INFO("IOTLB GET_FD iova 0x` -> map [0x`, 0x`] off 0x` perm `, va ", HEX(iova),
                 HEX(e.start), HEX(e.last), HEX(e.offset), e.perm,
                 (void*)((char*)base + (iova - e.start)));
    return (char *)base + (iova - e.start);
}

// Re-read the vq and map its three rings. resume_from_used picks the recovery
// point: a rescue adopts a ring whose daemon is gone, and the kernel's
// avail_index is only what that daemon last reported, so resume from used->idx --
// re-serving a completed request is safe (virtio-blk ops are idempotent), missing
// one is not. The probe measures the kernel's own value instead.
static bool vq_refresh(bool resume_from_used) {
    struct vduse_vq_info vi;
    memset(&vi, 0, sizeof(vi));
    vi.index = 0;
    if (ioctl(dev_fd, VDUSE_VQ_GET_INFO, &vi) < 0)
        LOG_ERRNO_RETURN(0, false, "VDUSE_VQ_GET_INFO failed");
    if (mode == PROBE)
        LOG_INFO("VQ_GET_INFO: num `, ready `, desc 0x`, driver 0x`, device 0x`, avail_index ",
                 vi.num, vi.ready, HEX(vi.desc_addr), HEX(vi.driver_addr),
                 HEX(vi.device_addr), vi.split.avail_index);
    vq_live = 0;
    if (!vi.ready || !vi.desc_addr || !vi.driver_addr || !vi.device_addr)
        return false;
    vq_num = vi.num;
    desc  = (struct vring_desc*)map_iova(vi.desc_addr, vi.num * sizeof(struct vring_desc), false);
    avail = (struct vring_avail*)map_iova(vi.driver_addr, sizeof(uint16_t) * (3 + vi.num), false);
    used  = (struct vring_used*)map_iova(vi.device_addr, sizeof(uint16_t) * 3 +
                                         sizeof(struct vring_used_elem) * vi.num, false);
    if (!desc || !avail || !used)
        return false;   // map_iova already said why
    used_idx = used->idx;      // live ring: resume where the device left off
    last_avail = resume_from_used ? used_idx : vi.split.avail_index;
    vq_live = 1;
    LOG_INFO("vring mapped, ", VALUE(vq_num), VALUE(last_avail), VALUE(used_idx));
    return true;
}

// Walk every available chain, answer it as a null virtio-blk device (reads
// return zeros, so a wedged partition scan sees invalid partitions and gives up
// cleanly) and publish the used ring. Returns the number served.
static int serve_vq() {
    if (!vq_live)
        return 0;
    __sync_synchronize();
    unsigned aidx = avail->idx;
    int served = 0;
    while (last_avail != aidx) {
        unsigned head = avail->ring[last_avail % vq_num];
        struct virtio_blk_outhdr *hdr = nullptr;
        void *data = nullptr; unsigned dlen = 0;
        unsigned char *status = nullptr;
        unsigned d = head;
        for (int k = 0; k < 32; k++) {   // chain walk; no indirect descriptors
            struct vring_desc *de = &desc[d];
            void *va = map_iova(de->addr, de->len ? de->len : 1, true);
            if (!va) {
                LOG_ERROR("unmappable descriptor: chain head `, desc `, iova 0x`, len ",
                          head, d, HEX(de->addr), de->len);
                return served;
            }
            if (!(de->flags & VRING_DESC_F_WRITE)) {
                if (!hdr && de->len >= sizeof(*hdr)) hdr = (struct virtio_blk_outhdr*)va;
            } else if (de->len == 1 && !(de->flags & VRING_DESC_F_NEXT)) {
                status = (unsigned char*)va;
            } else if (!data) {
                data = va; dlen = de->len;   // single data buffer only
            }
            if (!(de->flags & VRING_DESC_F_NEXT)) break;
            d = de->next;
        }
        unsigned type = hdr ? hdr->type : ~0u;
        unsigned long long sector = hdr ? hdr->sector : 0;
        bool is_read = hdr && type == VIRTIO_BLK_T_IN;
        if (is_read && data) {
            memset(data, 0, dlen);
            if (mode == PROBE)   // the probe's signature, read back by its dd
                for (unsigned i = 0; i < dlen && i < 8; i++)
                    ((char*)data)[i] = "VDPROBE!"[i];
        }
        // OUT (writes) and FLUSH need no action here
        if (status) *status = VIRTIO_BLK_S_OK;
        used->ring[used_idx % vq_num].id = head;
        used->ring[used_idx % vq_num].len = (status ? 1 : 0) + ((is_read && data) ? dlen : 0);
        used_idx++;
        last_avail++;
        served++;
        if (mode == PROBE)
            LOG_INFO("served head `, type `, sector `, dlen ", head, type, sector, dlen);
    }
    if (served) {
        __sync_synchronize();
        used->idx = used_idx;
        __sync_synchronize();
        unsigned idx = 0;
        if (ioctl(dev_fd, VDUSE_VQ_INJECT_IRQ, &idx) < 0)
            LOG_ERROR("VDUSE_VQ_INJECT_IRQ failed, ", ERRNO());
        LOG_INFO("served ` request(s), used->idx ", served, used_idx);
    }
    return served;
}

static int reply(uint32_t rid, uint32_t result, uint32_t vqidx, uint16_t avail_index) {
    struct vduse_dev_response resp;
    memset(&resp, 0, sizeof(resp));   // reserved MUST be zero (the kernel checks)
    resp.request_id = rid;
    resp.result = result;
    resp.vq_state.index = vqidx;
    resp.vq_state.split.avail_index = avail_index;
    ssize_t w = write(dev_fd, &resp, sizeof(resp));
    if (w != (ssize_t)sizeof(resp))
        LOG_ERROR_RETURN(0, -1, "reply to request ` failed, ", rid, ERRNO());
    return 0;
}

// Read ONE kernel message and answer it; false ends the caller's drain loop
// (EAGAIN, or a read error worth logging).
static bool handle_one_msg() {
    struct vduse_dev_request req;
    ssize_t r = read(dev_fd, &req, sizeof(req));
    if (r != (ssize_t)sizeof(req)) {
        if (r < 0 && errno != EAGAIN)
            LOG_ERROR("reading a vduse message failed, ", ERRNO());
        return false;
    }
    msgs++;
    switch (req.type) {
    case VDUSE_SET_STATUS:
        LOG_INFO("MSG #` SET_STATUS 0x`", msgs, HEX(req.s.status));
        if (req.s.status & VIRTIO_CONFIG_S_DRIVER_OK) {
            // the probe measures the FIRST handshake only; a rescue must
            // re-adopt after a consumer reset+reinit, so it refreshes every time
            if (mode == RESCUE || !driver_ok_seen) {
                driver_ok_seen = 1;
                if (mode == PROBE) {
                    unsigned long long f = 0;
                    ioctl(dev_fd, VDUSE_DEV_GET_FEATURES, &f);
                    LOG_INFO("negotiated features 0x`", HEX(f));
                }
                vq_refresh(mode == RESCUE);
            }
        }
        if (req.s.status == 0) {
            LOG_INFO("device reset");
            vq_live = 0;
            driver_ok_seen = 0;
        }
        reply(req.request_id, VDUSE_REQ_RESULT_OK, 0, 0);
        break;
    case VDUSE_UPDATE_IOTLB:
        LOG_INFO("MSG #` UPDATE_IOTLB [0x`, 0x`]", msgs,
                 HEX(req.iova.start), HEX(req.iova.last));
        if (mode == PROBE && msgs <= 4)   // dump the first few mappings in full
            map_iova(req.iova.start, req.iova.last - req.iova.start + 1, false);
        if (req.iova.start == 0 && req.iova.last == 0)
            vq_live = 0;   // unmap-all: the vring is gone
        reply(req.request_id, VDUSE_REQ_RESULT_OK, 0, 0);
        break;
    case VDUSE_GET_VQ_STATE:
        LOG_INFO("MSG #` GET_VQ_STATE vq ` -> avail ", msgs, req.vq_state.index, last_avail);
        reply(req.request_id, VDUSE_REQ_RESULT_OK, req.vq_state.index, last_avail);
        break;
    default:
        LOG_INFO("MSG #` type ` -> OK", msgs, req.type);
        reply(req.request_id, VDUSE_REQ_RESULT_OK, 0, 0);
    }
    return true;
}

// ---------------------------------------------------------------------------
// probe: create our own device, attach it to the vdpa bus, narrate everything
// ---------------------------------------------------------------------------

// Idempotent teardown, also run from the signal handler: this probe can block for
// MINUTES inside VDUSE_IOTLB_GET_FD (see HAZARD above), so an external timeout's
// SIGTERM arrives while it is blocked. Dying without this leaves the registration
// UNSERVED, and anything touching its /dev/vdX then wedges in unkillable D state.
// Order matters: consumer off first, then close the char dev (DESTROY_DEV is
// EBUSY while a daemon is still connected), then destroy.
static void cleanup() {
    static int done = 0;
    if (done) return;
    done = 1;
    // Block our own signals for the duration: a SIGTERM pending while main is
    // already in here would otherwise re-enter the handler, see `done`, and
    // _exit() -- cutting the teardown short and leaving the registration behind
    // (observed in a real run).
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGINT);
    sigprocmask(SIG_BLOCK, &block, nullptr);
    // Bounded: on a wedged device `vdpa dev del` can itself block. If it times
    // out, DESTROY_DEV below still succeeds and takes the half-attached vdpa
    // device with it; if even that returns EBUSY, serve the backlog with
    // `vduse-cli rescue` first, then retry.
    sh("timeout 10 vdpa dev del " PROBE_NAME " 2>/dev/null; true");
    if (dev_fd >= 0) { close(dev_fd); dev_fd = -1; }
    if (ctrl < 0) return;
    if (ioctl(ctrl, VDUSE_DESTROY_DEV, PROBE_NAME) < 0)
        LOG_ERROR("VDUSE_DESTROY_DEV failed, ", ERRNO());
    else
        LOG_INFO("DESTROY_DEV ok");
}

// system()/ioctl() are not async-signal-safe, but this is a diagnostic tool and
// the alternative (leaving a wedged VM) is worse; SIGKILL cannot be caught, so
// never SIGKILL the probe -- recover with `vduse-cli rescue` instead.
static void on_term(int sig) {
    cleanup();
    _exit(128 + sig);
}

static int cmd_probe() {
    mode = PROBE;
    now();   // start the deadline clock before anything can block
    ctrl = open("/dev/vduse/control", O_RDWR | O_CLOEXEC);
    if (ctrl < 0)
        LOG_ERRNO_RETURN(0, 1, "open /dev/vduse/control failed (modprobe vduse?)");
    unsigned long long ver = 0;
    ioctl(ctrl, VDUSE_GET_API_VERSION, &ver);
    LOG_INFO("kernel vduse api version `, we set 0", ver);
    ver = 0;
    if (ioctl(ctrl, VDUSE_SET_API_VERSION, &ver) < 0)
        LOG_ERROR("VDUSE_SET_API_VERSION failed, ", ERRNO());

    // vduse_dev_config ends in a flexible `config[]` for the virtio config space,
    // which C++ will not embed in a struct -- so the whole request is one buffer
    alignas(vduse_dev_config) char cbuf[sizeof(struct vduse_dev_config) + 512] = {};
    auto cc = (struct vduse_dev_config*)cbuf;
    strcpy(cc->name, PROBE_NAME);
    cc->vendor_id = 0x1af4;
    cc->device_id = VIRTIO_ID_BLOCK;
    cc->features = (1ULL << VIRTIO_F_ACCESS_PLATFORM) | (1ULL << VIRTIO_F_VERSION_1) |
                   (1ULL << VIRTIO_BLK_F_FLUSH) | (1ULL << VIRTIO_BLK_F_BLK_SIZE);
    cc->vq_num = 1;
    cc->vq_align = sysconf(_SC_PAGESIZE);
    cc->config_size = sizeof(struct virtio_blk_config);
    auto bc = (struct virtio_blk_config*)cc->config;
    bc->capacity = 4096;      // 2 MiB @512
    bc->blk_size = 512;
    if (ioctl(ctrl, VDUSE_CREATE_DEV, cbuf) < 0)
        LOG_ERRNO_RETURN(0, 1, "VDUSE_CREATE_DEV failed");
    LOG_INFO("CREATE_DEV ok (ACCESS_PLATFORM|VERSION_1|FLUSH|BLK_SIZE), features 0x`",
             HEX(cc->features));
    // from here on the registration exists: make sure even a signal tears it
    // down, because an unserved vduse device wedges its users in D state
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    LOG_WARN("may block for minutes in IOTLB_GET_FD while 'vdpa dev add' waits; "
             "do NOT SIGKILL -- recover with 'vduse-cli rescue'");

    dev_fd = open("/dev/vduse/" PROBE_NAME, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (dev_fd < 0)
        LOG_ERRNO_RETURN(0, 1, "open /dev/vduse/" PROBE_NAME " failed");
    int fd2 = open("/dev/vduse/" PROBE_NAME, O_RDWR | O_CLOEXEC);
    LOG_INFO("second open returned ` [expect EBUSY: the char dev IS the lock], ", fd2, ERRNO());
    if (fd2 >= 0) close(fd2);

    struct vduse_vq_config vqc;
    memset(&vqc, 0, sizeof(vqc));
    vqc.index = 0; vqc.max_size = 128;
    if (ioctl(dev_fd, VDUSE_VQ_SETUP, &vqc) < 0)
        LOG_ERROR("VDUSE_VQ_SETUP failed, ", ERRNO());

    kickfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    struct vduse_vq_eventfd ev;
    memset(&ev, 0, sizeof(ev));
    ev.index = 0; ev.fd = kickfd;
    if (ioctl(dev_fd, VDUSE_VQ_SETUP_KICKFD, &ev) < 0)
        LOG_ERROR("VDUSE_VQ_SETUP_KICKFD failed, ", ERRNO());

    // background: attach to the vdpa bus, find OUR new block dev, do IO
    sh("ls /sys/block | sort > /tmp/vdprobe.before");
    LOG_INFO("background: vdpa dev add + dd");
    sh("(vdpa dev add mgmtdev vduse name " PROBE_NAME "; echo ADD_RC=$?; "
       "for i in $(seq 50); do ls /sys/block | sort > /tmp/vdprobe.after; "
       "new=$(comm -13 /tmp/vdprobe.before /tmp/vdprobe.after | head -1); "
       "[ -n \"$new\" ] && break; sleep 0.2; done; echo NEWDEV=$new; "
       "sleep 0.5; if [ -n \"$new\" ] && [ -b /dev/$new ]; then "
       "echo '--- dd write:'; timeout 5 dd if=/dev/zero of=/dev/$new bs=4k count=4 conv=fsync 2>&1 | tail -1; "
       "echo '--- dd read:'; timeout 5 dd if=/dev/$new of=/tmp/vdprobe.read bs=4k count=4 2>&1 | tail -1; "
       "head -c 8 /tmp/vdprobe.read | od -c | head -1; fi; "
       "echo '--- cleanup: vdpa dev del'; vdpa dev del " PROBE_NAME "; echo DEL_RC=$?) "
       "> /tmp/vdprobe.bg.log 2>&1 &");

    // main loop: answer kernel messages + serve the vring on kicks
    double deadline = now() + 25.0;
    int kicks = 0;
    while (now() < deadline) {
        struct pollfd pfd[2] = { {dev_fd, POLLIN, 0}, {kickfd, POLLIN, 0} };
        int n = poll(pfd, 2, 200);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (pfd[1].revents & POLLIN) {
            uint64_t v;
            while (read(kickfd, &v, 8) == 8) kicks += v;
            serve_vq();
        }
        if (pfd[0].revents & POLLIN)
            while (handle_one_msg()) ;
        // also poll-serve: kicks can be missed if the eventfd raced the setup,
        // and on this kernel the kickfd was observed never to fire at all
        if (vq_live && kicks == 0 && driver_ok_seen) serve_vq();
        if (driver_ok_seen && now() > 18.0) break;   // IO done, wrap up
    }

    LOG_INFO("summary, ", VALUE(msgs), VALUE(kicks), "served_to=", used_idx);
    LOG_INFO("---- the background log ----");
    sh("cat /tmp/vdprobe.bg.log");
    cleanup();   // also runs on SIGTERM/SIGINT; idempotent
    return 0;
}

// ---------------------------------------------------------------------------
// rescue: adopt somebody else's wedged registration and drain the backlog
// ---------------------------------------------------------------------------

static int cmd_rescue(int argc, char **argv) {
    if (argc < 1) { usage(); return 2; }
    mode = RESCUE;
    int secs = argc > 1 ? atoi(argv[1]) : 60;
    char path[300];
    snprintf(path, sizeof(path), "/dev/vduse/%s", argv[0]);
    // the char device admits a single opener: EBUSY here means a live daemon
    // already serves it and there is nothing to rescue
    dev_fd = open(path, O_RDWR | O_NONBLOCK);
    if (dev_fd < 0)
        LOG_ERRNO_RETURN(0, 1, "open ` failed", path);
    LOG_INFO("adopting ` for ` s; SIGTERM stops it and the registration is NOT destroyed",
             argv[0], secs);
    if (!vq_refresh(true))
        LOG_INFO("vq not live yet; messages only");

    time_t t0 = time(nullptr);
    while (time(nullptr) - t0 < secs) {
        serve_vq();
        struct pollfd pfd = { dev_fd, POLLIN, 0 };
        int n = poll(&pfd, 1, 100);
        if (n <= 0) continue;
        while (handle_one_msg()) ;
    }
    close(dev_fd);
    LOG_INFO("rescue done");
    return 0;
}

// ---------------------------------------------------------------------------
// destroy: VDUSE_DESTROY_DEV without serving -- the last step of the drill
// ---------------------------------------------------------------------------

static int cmd_destroy(int argc, char **argv) {
    if (argc < 1) { usage(); return 2; }
    int c = open("/dev/vduse/control", O_RDWR | O_CLOEXEC);
    if (c < 0)
        LOG_ERRNO_RETURN(0, 1, "open /dev/vduse/control failed (modprobe vduse?)");
    uint64_t ver = 0;   // the only version today; the kernel reports its own
    if (ioctl(c, VDUSE_SET_API_VERSION, &ver) < 0)
        LOG_WARN("VDUSE_SET_API_VERSION failed (continuing), ", ERRNO());

    char name[VDUSE_NAME_MAX];
    memset(name, 0, sizeof(name));
    snprintf(name, sizeof(name), "%s", argv[0]);
    if (ioctl(c, VDUSE_DESTROY_DEV, name) < 0) {
        if (errno == EBUSY)
            LOG_ERROR_RETURN(0, 1, "VDUSE_DESTROY_DEV ` failed: a daemon is still connected "
                             "to /dev/vduse/<name>, or the vdpa dev still exists -- kill or "
                             "detach it first, ", name, ERRNO());
        LOG_ERROR_RETURN(0, 1, "VDUSE_DESTROY_DEV ` failed, ", name, ERRNO());
    }
    LOG_INFO("destroyed ", name);
    close(c);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    if (!strcmp(argv[1], "probe")) return cmd_probe();
    if (!strcmp(argv[1], "rescue")) return cmd_rescue(argc - 2, argv + 2);
    if (!strcmp(argv[1], "destroy")) return cmd_destroy(argc - 2, argv + 2);
    LOG_ERROR("unknown subcommand ", argv[1]);
    usage();
    return 2;
}
