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

// vduse-cli: the vduse diagnostic and recovery assistant -- four subcommands in
// one program, sharing the ABI plumbing (the lazy IOTLB, the vring refresh, the
// virtio-blk descriptor walk, the kernel message replies).
//
//   vduse-cli probe                   create a virtio-blk VDUSE device of its own
//                                     ("vdprobe"), attach it to the vdpa bus, serve
//                                     it, and dump the whole kernel<->daemon
//                                     interaction
//   vduse-cli rescue <name> [secs]    adopt an EXISTING registration nobody serves
//                                     and drain the backlog of EVERY queue it has
//                                     (default 60s)
//   vduse-cli destroy <name>          VDUSE_DESTROY_DEV, without serving it
//   vduse-cli vqprobe <n> <setup> <i>...   create a registration declaring <n>
//                                     virtqueues and ask VDUSE_VQ_GET_INFO about
//                                     each index <i>, one row per index
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
// MEASURED RECOVERY ORDER for a wedged device. Verified end to end on a 4-queue
// wedge; the drill recorded before that run is the one that failed, and how it
// failed is why this tool holds per-queue state now.
//   1. Serve EVERY queue of the registration. `vduse-cli rescue <name>` does that
//      only as of this change. Before it, rescue kept ONE set of file-scope ring
//      state and hardcoded vi.index = 0, so it mapped and served queue 0 and no
//      other -- and one instance per queue is not available either, because the
//      char device admits a single opener (a second open answers EBUSY, measured).
//      On that 4-queue wedge it adopted, mapped one vring, logged 5 lines in
//      total, and served nothing across a 1800 s continuous run plus 240 s of
//      polling, while two processes stayed in uninterruptible D state holding the
//      machine-wide genl_lock. The four rescues recorded as successful before
//      that all happened to have their pending work reachable from queue 0.
//      The fallback that did work, in 6 s, was adopting through blk/vduse.cpp's
//      start(), whose resync covers every queue. It is usable on a wedged box for
//      a reason worth remembering: vduse.cpp includes no netlink header, so its
//      control plane issues no genl command and cannot block on the genl_lock a
//      wedged `vdpa dev add` is holding.
//   2. `vdpa dev show` answering rc=0 instantly is the criterion that genl_lock is
//      free, i.e. that the machine has stopped being machine-wide wedged. Test it
//      before any other step that speaks netlink.
//   3. `vdpa dev del <name>`: rc=0 when something is serving the device, 124 only
//      when it is half-attached. Never WAIT on it -- first trap below.
//   4. SIGTERM the daemon, never -9: DESTROY_DEV is EBUSY while a daemon is still
//      connected, and SIGKILL leaves the registration UNSERVED, which re-wedges
//      its consumers.
//   5. `vduse-cli destroy <name>`: destroying the registration also takes a
//      half-attached vdpa device with it, so this is the step that clears the
//      residue.
// Clean state afterwards: `ls /dev/vduse` shows only `control` and `vdpa dev show`
// is empty. SIGKILL is not recoverable without this tool or the product adopter.
//
// TWO TRAPS THAT COST TIME ON THAT RUN, both general:
//   - A recovery script must never WAIT on `vdpa dev del`. `timeout N vdpa dev del`
//     does not bound it: when the child goes to D state, timeout signals it,
//     cannot reap it, and blocks -- so the script hangs before reaching its own
//     destroy step, and the hung timeout leaves a SECOND unkillable D process
//     behind. Run it detached and poll instead.
//   - `pkill -f "<pattern>"` self-matches: the invoking shell's own command line
//     contains the pattern, so pkill kills the session that ran it. Kill by pid,
//     taken from `pgrep -x <exact name>`.
//
// WHY vqprobe EXISTS, separately from probe. A daemon that ADOPTS a registration
// somebody else created never ran CREATE_DEV, so the queue count the kernel holds
// is not one it declared -- and the uapi has no readback for it: no get-vq_num and
// no GET_CONFIG, only VDUSE_VQ_GET_INFO, whose index is the caller's to supply.
// Whether that ioctl can therefore answer "how many queues does this registration
// have" turns entirely on a behaviour the header does not document ("Caller should
// set index field" is all it says): does the kernel bound the index against its own
// count, or answer any index? vqprobe measures it, on a registration of its own
// with a count of its own choosing, so the answer does not depend on anybody's
// device being in anybody's state. Unlike probe it never attaches a consumer, so
// there is no /dev/vdX to wedge and no IOTLB stall to wait out. rescue leans on
// that answer: discover_vqs() counts the indices VDUSE_VQ_GET_INFO replies to and
// stops at the first one it refuses, so the refusal -- not any field of a reply
// -- is the queue count. Which errno the refusal carries is not predictable from
// the header either: out of range was measured as EINVAL, and the same probe on
// an unbound orphan (a registration no consumer has ever attached to) as EPERM.
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
// serve_vq() also does not walk indirect descriptor tables, and the shared engine now
// does. That gap is deliberate and it is bounded by the transports rather than by this
// file: the only transport that offers VIRTIO_RING_F_INDIRECT_DESC is vhost-user, whose
// rings live in the frontend's memory and are not what this tool rescues, while vduse
// does not offer it -- so a backlog published with tables cannot reach here today. If
// vduse ever offers the bit, this walk starts skipping requests silently: it treats a
// descriptor carrying VRING_DESC_F_INDIRECT as an ordinary buffer, so the header it
// finds is table bytes, the type it reads is garbage, and the request it "serves" is
// one the driver never made. Fixing that is its own change, not a comment.
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
#define VQPROBE_NAME "vdvqprobe"

enum Mode {
    PROBE,      // our own device: measure and narrate everything
    RESCUE,     // somebody else's wedged device: adopt, drain, say as little as
                // possible -- and never destroy it (it is not ours)
};
static Mode mode = PROBE;

static int ctrl = -1, dev_fd = -1, kickfd = -1;

// The cap blk/utils.h's MAX_QUEUES puts on one device's queue count, repeated
// here because this assistant program includes no blk header. It has to be that
// cap rather than a number of our own: a rescue that can hold fewer queues than
// the registration has serves fewer than the registration has, which is exactly
// the defect the per-queue state below exists to remove.
static constexpr unsigned MAX_VQS = 64;

// ---- vring state, one set per virtqueue ----
// A registration this tool adopts was declared by somebody else, so its queue
// count is not one this tool chose and the uapi has no readback for it (see
// discover_vqs). A single set of globals would serve index 0 alone and leave
// every other queue's backlog unserved -- the wedge rescue exists to clear.
struct Vq {
    unsigned index;       // what VDUSE_VQ_GET_INFO and VDUSE_VQ_INJECT_IRQ take
    unsigned vq_num;      // ring size, as the kernel reports it
    struct vring_desc *desc;
    struct vring_avail *avail;
    struct vring_used *used;
    unsigned last_avail, used_idx;
    int live;             // rings mapped and safe to serve
};
static struct Vq vqs[MAX_VQS];
static unsigned nvqs;                  // indices the discovery probe answered for
static unsigned msgs, driver_ok_seen;  // device-wide, not per-queue

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
        "       vduse-cli rescue <name> [secs]   adopt a wedged registration and drain all its queues (default 60)\n"
        "       vduse-cli destroy <name>         VDUSE_DESTROY_DEV without serving\n"
        "       vduse-cli vqprobe <vq_num> <setup:0|1> <index>...\n"
        "                                        create a registration of <vq_num> virtqueues and ask\n"
        "                                        VDUSE_VQ_GET_INFO about each <index>, one row each\n"
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

// How many virtqueues the registration has, measured rather than declared: the
// count belongs to whoever ran VDUSE_CREATE_DEV and nothing in the uapi hands it
// back, so it is the number of indices VDUSE_VQ_GET_INFO replies to. The walk
// stops at the first index the ioctl refuses, whatever errno it refuses with --
// out of range answers EINVAL and an unbound orphan answers EPERM, and calling
// either one "out of range" would be a guess, so only the refusal itself counts.
static unsigned discover_vqs() {
    unsigned n;
    for (n = 0; n < MAX_VQS; n++) {
        struct vduse_vq_info vi;
        memset(&vi, 0, sizeof(vi));
        vi.index = n;
        if (ioctl(dev_fd, VDUSE_VQ_GET_INFO, &vi) < 0)
            break;
        vqs[n].index = n;
    }
    if (n == MAX_VQS)
        LOG_WARN("` indices answered VDUSE_VQ_GET_INFO, which is the cap this tool holds: queues beyond it are NOT served", MAX_VQS);
    else
        LOG_INFO("` virtqueue(s) discovered by probing VDUSE_VQ_GET_INFO", n);
    return n;
}

// Re-read one vq and map its three rings. resume_from_used picks the recovery
// point: a rescue adopts a ring whose daemon is gone, and the kernel's
// avail_index is only what that daemon last reported, so resume from used->idx --
// re-serving a completed request is safe (virtio-blk ops are idempotent), missing
// one is not. The probe measures the kernel's own value instead.
static bool vq_refresh(struct Vq &vq, bool resume_from_used) {
    struct vduse_vq_info vi;
    memset(&vi, 0, sizeof(vi));
    vi.index = vq.index;
    if (ioctl(dev_fd, VDUSE_VQ_GET_INFO, &vi) < 0)
        LOG_ERRNO_RETURN(0, false, "VDUSE_VQ_GET_INFO of vq ` failed", vq.index);
    if (mode == PROBE)
        LOG_INFO("VQ_GET_INFO: num `, ready `, desc 0x`, driver 0x`, device 0x`, avail_index ",
                 vi.num, vi.ready, HEX(vi.desc_addr), HEX(vi.driver_addr),
                 HEX(vi.device_addr), vi.split.avail_index);
    vq.live = 0;
    if (!vi.ready || !vi.desc_addr || !vi.driver_addr || !vi.device_addr)
        return false;
    vq.vq_num = vi.num;
    vq.desc  = (struct vring_desc*)map_iova(vi.desc_addr, vi.num * sizeof(struct vring_desc), false);
    vq.avail = (struct vring_avail*)map_iova(vi.driver_addr, sizeof(uint16_t) * (3 + vi.num), false);
    vq.used  = (struct vring_used*)map_iova(vi.device_addr, sizeof(uint16_t) * 3 +
                                           sizeof(struct vring_used_elem) * vi.num, false);
    if (!vq.desc || !vq.avail || !vq.used)
        return false;   // map_iova already said why
    vq.used_idx = vq.used->idx;      // live ring: resume where the device left off
    vq.last_avail = resume_from_used ? vq.used_idx : vi.split.avail_index;
    vq.live = 1;
    // named locals so this row prints the three names it always has -- it is the
    // probe's dump as much as the rescue's, and the caller is what names the queue
    unsigned vq_num = vq.vq_num, last_avail = vq.last_avail, used_idx = vq.used_idx;
    LOG_INFO("vring mapped, ", VALUE(vq_num), VALUE(last_avail), VALUE(used_idx));
    return true;
}

// Walk every available chain of ONE queue, answer it as a null virtio-blk device
// (reads return zeros, so a wedged partition scan sees invalid partitions and
// gives up cleanly) and publish that queue's used ring. Returns the number served.
static int serve_vq(struct Vq &vq) {
    if (!vq.live)
        return 0;
    __sync_synchronize();
    unsigned aidx = vq.avail->idx;
    int served = 0;
    while (vq.last_avail != aidx) {
        unsigned head = vq.avail->ring[vq.last_avail % vq.vq_num];
        struct virtio_blk_outhdr *hdr = nullptr;
        void *data = nullptr; unsigned dlen = 0;
        unsigned char *status = nullptr;
        unsigned d = head;
        for (int k = 0; k < 32; k++) {   // chain walk; no indirect descriptors
            struct vring_desc *de = &vq.desc[d];
            void *va = map_iova(de->addr, de->len ? de->len : 1, true);
            if (!va) {
                LOG_ERROR("vq ` unmappable descriptor: chain head `, desc `, iova 0x`, len ",
                          vq.index, head, d, HEX(de->addr), de->len);
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
        vq.used->ring[vq.used_idx % vq.vq_num].id = head;
        vq.used->ring[vq.used_idx % vq.vq_num].len = (status ? 1 : 0) + ((is_read && data) ? dlen : 0);
        vq.used_idx++;
        vq.last_avail++;
        served++;
        if (mode == PROBE)
            LOG_INFO("served head `, type `, sector `, dlen ", head, type, sector, dlen);
    }
    if (served) {
        __sync_synchronize();
        vq.used->idx = vq.used_idx;
        __sync_synchronize();
        // The interrupt names the queue it belongs to. A zero here would tell the
        // consumer of queue 0 about completions some OTHER queue published, so the
        // queue that was actually served leaves its waiter blocked on a folio only
        // this tool can complete -- the wedge rescue exists to clear, reproduced
        // by one constant.
        if (ioctl(dev_fd, VDUSE_VQ_INJECT_IRQ, &vq.index) < 0)
            LOG_ERROR("VDUSE_VQ_INJECT_IRQ of vq ` failed, ", vq.index, ERRNO());
        if (mode == PROBE)   // the probe's dump keeps the row it has always printed
            LOG_INFO("served ` request(s), used->idx ", served, vq.used_idx);
        else                 // a multiqueue rescue has to say which queue it served
            LOG_INFO("vq ` served ` request(s), used->idx ", vq.index, served, vq.used_idx);
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
                // A rescue can be started before any consumer has attached, and on
                // an unbound orphan this same probe was measured answering EPERM --
                // a refusal whose meaning for an IN-RANGE index is not measured. So
                // a count of zero taken that early means "not knowable yet" as much
                // as "no queues", and DRIVER_OK is the first moment the
                // registration is bound: ask again before refreshing.
                if (!nvqs)
                    nvqs = discover_vqs();
                for (unsigned i = 0; i < nvqs; i++)
                    vq_refresh(vqs[i], mode == RESCUE);
            }
        }
        if (req.s.status == 0) {
            LOG_INFO("device reset");
            for (unsigned i = 0; i < nvqs; i++)
                vqs[i].live = 0;
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
            for (unsigned i = 0; i < nvqs; i++)
                vqs[i].live = 0;   // unmap-all: every vring is gone
        reply(req.request_id, VDUSE_REQ_RESULT_OK, 0, 0);
        break;
    case VDUSE_GET_VQ_STATE: {
        unsigned i = req.vq_state.index;
        // The kernel asks about ONE queue by index. Answering with a different
        // queue's cursor hands the driver a resume point into a ring it is not
        // about to serve -- the same mistake as serving the wrong queue.
        if (i >= nvqs)
            LOG_ERROR("MSG #` GET_VQ_STATE for index ` of ` discovered queues", msgs, i, nvqs);
        unsigned la = i < nvqs ? vqs[i].last_avail : 0;
        LOG_INFO("MSG #` GET_VQ_STATE vq ` -> avail ", msgs, i, la);
        reply(req.request_id, VDUSE_REQ_RESULT_OK, i, la);
        break;
    }
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
    LOG_WARN("may block for minutes in IOTLB_GET_FD while 'vdpa dev add' waits; do NOT SIGKILL -- recover with 'vduse-cli rescue'");

    dev_fd = open("/dev/vduse/" PROBE_NAME, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (dev_fd < 0)
        LOG_ERRNO_RETURN(0, 1, "open /dev/vduse/` failed", PROBE_NAME);
    int fd2 = open("/dev/vduse/" PROBE_NAME, O_RDWR | O_CLOEXEC);
    LOG_INFO("second open returned ` [expect EBUSY: the char dev IS the lock], ", fd2, ERRNO());
    if (fd2 >= 0) close(fd2);

    struct vduse_vq_config vqc;
    memset(&vqc, 0, sizeof(vqc));
    vqc.index = 0; vqc.max_size = 128;
    if (ioctl(dev_fd, VDUSE_VQ_SETUP, &vqc) < 0)
        LOG_ERROR("VDUSE_VQ_SETUP failed, ", ERRNO());
    // One queue, index 0: CREATE_DEV above declared vq_num 1, so there is nothing
    // for discover_vqs to find out and every row this probe prints names index 0.
    nvqs = 1;
    vqs[0].index = 0;

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
            for (unsigned i = 0; i < nvqs; i++)
                serve_vq(vqs[i]);
        }
        if (pfd[0].revents & POLLIN)
            while (handle_one_msg()) ;
        // also poll-serve: kicks can be missed if the eventfd raced the setup,
        // and on this kernel the kickfd was observed never to fire at all
        if (kicks == 0 && driver_ok_seen)
            for (unsigned i = 0; i < nvqs; i++)
                serve_vq(vqs[i]);   // serve_vq skips a queue that is not live
        if (driver_ok_seen && now() > 18.0) break;   // IO done, wrap up
    }

    LOG_INFO("summary, ", VALUE(msgs), VALUE(kicks), "served_to=", vqs[0].used_idx);
    LOG_INFO("---- the background log ----");
    sh("cat /tmp/vdprobe.bg.log");
    cleanup();   // also runs on SIGTERM/SIGINT; idempotent
    return 0;
}

// ---------------------------------------------------------------------------
// rescue: adopt somebody else's wedged registration and drain the backlog of
// every queue it has
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
    // Every queue, not index 0 alone: the count belongs to whoever created the
    // registration, and a backlog left on a queue this tool never serves keeps
    // that queue's consumer in D state (see MEASURED RECOVERY ORDER above).
    nvqs = discover_vqs();
    for (unsigned i = 0; i < nvqs; i++) {
        if (!vq_refresh(vqs[i], true)) {
            LOG_INFO("vq ` is not live yet; messages only", i);
            continue;
        }
        LOG_INFO("vq ` adopted, resuming from used: last_avail `, used_idx `",
                 i, vqs[i].last_avail, vqs[i].used_idx);
    }
    if (!nvqs)
        LOG_INFO("no virtqueue answered VDUSE_VQ_GET_INFO; messages only");

    time_t t0 = time(nullptr);
    while (time(nullptr) - t0 < secs) {
        for (unsigned i = 0; i < nvqs; i++)
            serve_vq(vqs[i]);
        struct pollfd pfd = { dev_fd, POLLIN, 0 };
        int n = poll(&pfd, 1, 100);
        if (n <= 0) continue;
        while (handle_one_msg()) ;
    }
    close(dev_fd);
    LOG_INFO("rescue done, ` queue(s) served", nvqs);
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
            LOG_ERROR_RETURN(0, 1, "VDUSE_DESTROY_DEV ` failed: a daemon is still connected to /dev/vduse/<name>, or the vdpa dev still exists -- kill or detach it first, ", name, ERRNO());
        LOG_ERROR_RETURN(0, 1, "VDUSE_DESTROY_DEV ` failed, ", name, ERRNO());
    }
    LOG_INFO("destroyed ", name);
    close(c);
    return 0;
}

// ---------------------------------------------------------------------------
// vqprobe: what VDUSE_VQ_GET_INFO answers for an index, on a registration whose
// queue count this run chose
// ---------------------------------------------------------------------------

// Idempotent, and also the signal handler's teardown. What this subcommand
// creates is inert -- no consumer is ever attached, so there is no /dev/vdX and
// nothing that can wedge in D state -- but a leftover still answers EEXIST to
// the next run's CREATE_DEV and shows up in `ls /dev/vduse`.
static void vqprobe_cleanup() {
    static int done = 0;
    if (done) return;
    done = 1;
    if (dev_fd >= 0) { close(dev_fd); dev_fd = -1; }
    if (ctrl < 0) return;
    char nm[VDUSE_NAME_MAX];
    memset(nm, 0, sizeof(nm));
    snprintf(nm, sizeof(nm), "%s", VQPROBE_NAME);
    if (ioctl(ctrl, VDUSE_DESTROY_DEV, nm) < 0 && errno != EINVAL)
        LOG_ERROR("VDUSE_DESTROY_DEV ` failed, ", VQPROBE_NAME, ERRNO());
}

static void vqprobe_on_term(int sig) {
    vqprobe_cleanup();
    _exit(128 + sig);
}

// argv: <vq_num> <setup:0|1> <index>...
// One row per index, in the order given, so the caller decides how much of an
// escalation a single process is allowed to attempt.
static int cmd_vqprobe(int argc, char **argv) {
    if (argc < 3) { usage(); return 2; }
    unsigned vq_num = (unsigned)atoi(argv[0]);
    int do_setup = atoi(argv[1]);
    if (vq_num < 1)
        LOG_ERROR_RETURN(EINVAL, 2, "vq_num must be at least 1");

    ctrl = open("/dev/vduse/control", O_RDWR | O_CLOEXEC);
    if (ctrl < 0)
        LOG_ERRNO_RETURN(0, 1, "open /dev/vduse/control failed (modprobe vduse?)");
    uint64_t ver = 0;   // the only version this tool sets; the kernel reports its own
    if (ioctl(ctrl, VDUSE_SET_API_VERSION, &ver) < 0)
        LOG_ERROR("VDUSE_SET_API_VERSION failed, ", ERRNO());

    alignas(vduse_dev_config) char cbuf[sizeof(struct vduse_dev_config) + 512] = {};
    auto cc = (struct vduse_dev_config*)cbuf;
    strcpy(cc->name, VQPROBE_NAME);
    cc->vendor_id = 0x1af4;
    cc->device_id = VIRTIO_ID_BLOCK;
    cc->features = (1ULL << VIRTIO_F_ACCESS_PLATFORM) | (1ULL << VIRTIO_F_VERSION_1);
    cc->vq_num = vq_num;
    cc->vq_align = sysconf(_SC_PAGESIZE);
    cc->config_size = sizeof(struct virtio_blk_config);
    auto bc = (struct virtio_blk_config*)cc->config;
    bc->capacity = 4096;      // 2 MiB @512
    bc->blk_size = 512;
    bc->num_queues = (uint16_t)vq_num;
    if (ioctl(ctrl, VDUSE_CREATE_DEV, cbuf) < 0)
        LOG_ERRNO_RETURN(0, 1, "VDUSE_CREATE_DEV of ` with vq_num ` failed (a leftover of a run that died? 'vduse-cli destroy `')", VQPROBE_NAME, vq_num, VQPROBE_NAME);
    signal(SIGTERM, vqprobe_on_term);
    signal(SIGINT, vqprobe_on_term);

    dev_fd = open("/dev/vduse/" VQPROBE_NAME, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (dev_fd < 0) {
        int e = errno;
        vqprobe_cleanup();
        LOG_ERROR_RETURN(e, 1, "open /dev/vduse/` failed", VQPROBE_NAME);
    }
    // The variant that matches an adoption: whoever registered these queues also
    // set every one of them up, so an in-range index is a queue the kernel has
    // been told about rather than an untouched slot.
    if (do_setup)
        for (unsigned i = 0; i < vq_num; i++) {
            struct vduse_vq_config vqc;
            memset(&vqc, 0, sizeof(vqc));
            vqc.index = i;
            vqc.max_size = 128;
            if (ioctl(dev_fd, VDUSE_VQ_SETUP, &vqc) < 0)
                LOG_ERROR("VDUSE_VQ_SETUP of index ` failed, ", i, ERRNO());
        }

    for (int a = 2; a < argc; a++) {
        unsigned idx = (unsigned)atoi(argv[a]);
        struct vduse_vq_info vi;
        memset(&vi, 0, sizeof(vi));
        vi.index = idx;
        int rc = ioctl(dev_fd, VDUSE_VQ_GET_INFO, &vi);
        int e = errno;   // captured before anything below can set it again
        // stderr and flushed per row, deliberately not alog: the next index this
        // loop asks about is one nobody has ever asked the kernel about, and a row
        // still sitting in a buffer when the machine objects to it is a row the
        // measurement lost.
        fprintf(stderr,
                "VQPROBE vq_num=%u setup=%d index=%u rc=%d errno=%d(%s)"
                " vi.index=%u vi.num=%u vi.ready=%u"
                " desc=0x%llx driver=0x%llx device=0x%llx avail_index=%u\n",
                vq_num, do_setup, idx, rc, rc < 0 ? e : 0,
                rc < 0 ? strerror(e) : "-", vi.index, vi.num, vi.ready,
                (unsigned long long)vi.desc_addr, (unsigned long long)vi.driver_addr,
                (unsigned long long)vi.device_addr, vi.split.avail_index);
        fflush(stderr);
    }
    vqprobe_cleanup();
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    if (!strcmp(argv[1], "probe")) return cmd_probe();
    if (!strcmp(argv[1], "rescue")) return cmd_rescue(argc - 2, argv + 2);
    if (!strcmp(argv[1], "destroy")) return cmd_destroy(argc - 2, argv + 2);
    if (!strcmp(argv[1], "vqprobe")) return cmd_vqprobe(argc - 2, argv + 2);
    LOG_ERROR("unknown subcommand ", argv[1]);
    usage();
    return 2;
}
