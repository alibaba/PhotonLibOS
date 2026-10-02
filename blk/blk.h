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
#include <cstdint>
#include <string>
#include <vector>
#include <photon/common/callback.h>
#include <photon/common/object.h>
#include <photon/common/timeout.h>       // Timeout for TcmuHBA::wait_for_event
#include <photon/fs/filesystem.h>        // UNIMPLEMENTED for resize(), and fs::IFile
#include <photon/net/socket.h>           // NbdConfig::tcp_endpoint is a net::EndPoint BY VALUE
#include <photon/thread/thread.h>        // DEFAULT_STACK_SIZE for BlkConfig::stack_size

namespace photon {
class WorkPool;                          // BlkConfig::pool is only ever a pointer
namespace blk {

// Op support bits for BlkDevInfo::features
static constexpr uint64_t FEATURE_FLUSH         = 1ull << 0;
static constexpr uint64_t FEATURE_DISCARD       = 1ull << 1;
static constexpr uint64_t FEATURE_WRITE_ZEROES  = 1ull << 2;

// The single descriptor of an exported block device: what it is called and
// what it looks like. Used both as the element of list_xxx_orphans() and as
// BlkConfig::info -- so it is what a recovery loop carries from an orphan
// record back into a config.
struct BlkDevInfo {
    // The RECOVERY KEY: the transport-relative string naming the kernel-side
    // registration, stable across daemon restarts and unique within its
    // transport. Deliberately NOT a device node -- vhost-user's is a unix socket
    // path, ublk's a decimal dev_id, vduse's the /dev/vduse/<name> char device.
    // The block node, where the transport has one at all, comes from
    // IBlkDevice::get_device_node() instead.
    std::string identity;           // tcmu: backstore name / ublk: dev_id / vduse: device name /
                                    // vhost-user: socket path / nbd: export name

    uint64_t size = 0;              // bytes; always declared here, never fstat-ed from the backend
                                    // (backends may be sparse/layered virtual files)

    uint64_t features = 0;          // FEATURE_*

    uint8_t sector_size_shift = 9;  // 2^9 = 512 bytes
};

// A serving session of one exported virtual block device. Device ownership is
// kernel-side: this object owns only the user-space serving (queues, threads,
// fds); detach()/shutdown() differ in whether the kernel registration survives.
//
// start() contract, common to all transports: bring the device to serving
// state, reconciling kernel-side state with the config it was built from. If a
// registration under info().identity exists: validate the config against it
// (mismatch, i.e. config drift, is a hard EINVAL error) and attach, then
// harvest any backlog left by a previous process through the same dispatch path
// as fresh requests. Otherwise create the registration first. Returns 0 once
// serving. On failure returns -1 with errno set and rolls back kernel-side
// residue; the object returns to its virgin state and start() may be retried on
// it. ENOENT/EEXIST races are retried once internally; EBUSY means another live
// process is serving this identity. What is left for start() to reject is only
// what needs the backend or the kernel: a null backend (EINVAL), a second
// concurrent start (EALREADY), config drift, a foreign live daemon, a missing
// kernel feature.
//
// Identity and geometry are FIXED AT CONSTRUCTION: every new_xxx_device() takes
// its transport's config and validates it, so a constructed object is always
// config-valid and a bad config yields no object at all (nullptr + errno). A
// factory does no I/O and touches no kernel state -- it cannot block. Serving a
// different identity means constructing a different device.
class IBlkDevice : public Object {
public:
    // Serve `backend`. ownership = this object deletes it (on shutdown and in
    // the destructor); a FAILED start() hands it back to the caller either way.
    virtual int start(fs::IFile* backend, bool ownership = false) = 0;

    // The identity and geometry this device was built from, kept current by
    // resize(). What a recovery loop matches an orphan record against.
    virtual const BlkDevInfo& get_info() const = 0;

    // The local block device node this object created and can name (ublk's
    // /dev/ublkbN, nbd's /dev/nbdN, tcmu's tcm_loop /dev/sdX), or nullptr when
    // there is none -- which covers three cases a caller rarely needs to tell
    // apart: not started; started with no local node by design (vhost-user
    // always, nbd and tcmu with their loopback option off); or the node belongs
    // to an external consumer that attaches asynchronously and is not ours to
    // name (vduse). CHECK IT: feeding nullptr to a std::string, strcmp or a
    // gtest string comparison is undefined behaviour, not a clean failure.
    // The name is stable for the serving session, but a freshly appeared node can
    // still reject open() with ENXIO for a moment -- retry the open, not this.
    virtual const char* get_device_node() = 0;

    // Stop serving but keep the kernel-side registration, so that a later
    // start() (possibly in another process) can take over.
    // wait_pending=true: drain in-flight IO before returning (orderly handover).
    //   This wait has NO deadline, deliberately. What it waits on is the peer's
    //   IO arrival rate, which is not ours to bound: a busy guest, initiator or
    //   client keeps refilling, so detach(true) returns when the peer goes
    //   quiet, not when a timer expires. Contrast ublk's quiesce_timeout_ms and
    //   stop_timeout_ms, which bound waits on the KERNEL and so can have a
    //   deadline. A caller that cannot wait indefinitely must pass false.
    // wait_pending=false: return immediately, leaving pending requests
    // kernel-side for the next start() to harvest (crash-recovery style).
    virtual int detach(bool wait_pending) = 0;

    // detach() + destroy the kernel-side registration. Fails with EBUSY if the
    // initiator still holds the device (fs mounted / client connected); no
    // force option -- tearing down an in-use device is data corruption.
    virtual int shutdown() = 0;

    // Notify the initiator of a capacity change. Enlarge the backend first; to
    // shrink, resize() must succeed BEFORE shrinking the backend. Shrink is
    // rejected by default. Not supported by nbd (size fixed at handshake).
    UNIMPLEMENTED(int resize(uint64_t new_size));
};

struct BlkConfig {
    BlkDevInfo info;

    // The two contracts a non-null `pool` comes with.
    //
    // CONTRACT 1 -- lifetime. detach()/shutdown() every device that uses this pool
    // BEFORE destroying it. Breaking that does not crash: the serving coroutines
    // are asleep on their fds, so they are on the sleepq, and photon::fini()'s
    // wait_all() then never returns -- a hang with no log line. There is no
    // reference accounting to catch it, because WorkPool does not expose its
    // vcpus, so nothing can be counted against them.
    //
    // CONTRACT 2 -- engines. Every vcpu in the pool must be able to host the
    // serving coroutines. The event-engine half is verified: start() asks each
    // pool vcpu's master engine to name itself and compares that with the
    // caller's, so construct the pool with the same event engine you initialized
    // your own vcpu with. `WorkPool pool(n);` is NOT that -- its default
    // ev_engine installs no engine at all, every fd wait on such a vcpu fails at
    // once, and start() rejects the pool with EINVAL. A backend file opened with
    // the iouring engine additionally requires the pool's vcpus to run iouring as
    // their master engine, not merely to have been asked for it: init() keeps the
    // first engine that initializes, so a request naming several installs one.
    //
    // The io-engine half is NOT verified, and is yours alone: give the pool vcpus
    // an io_engine that covers your backend. An io engine is a per-vcpu init
    // function rather than an object, and photon keeps the mask a vcpu was inited
    // with to itself, so there is nothing start() could ask. Getting it wrong is
    // a crash rather than an EINVAL -- libaio's context is thread-local, and a
    // vcpu inited without INIT_IO_LIBAIO has none.
    WorkPool* pool = nullptr;     // Where the serving coroutines run. nullptr: on the
                                  // caller's own vcpu. Non-null: every coroutine this
                                  // device CREATES in order to serve a request is moved
                                  // onto a pool vcpu as it is created -- one per queue
                                  // for ublk, vduse and vhost-user, the ring pump for
                                  // tcmu, one per client connection for nbd -- and the
                                  // pool's own round-robin cursor spreads them, so more
                                  // queues than pool vcpus means queues share one. The
                                  // caller's vcpu, and whatever already runs on it, is
                                  // not touched. A pool holding no vcpus reads as nullptr.

    uint32_t queues = 0;          // serving parallelism; 0 = transport-chosen default.
                                  // Honored by ublk, vhost-user and vduse, all clamping it
                                  // to their maximum. tcmu and nbd ignore it by nature:
                                  // tcmu's kernel gives one command ring per device, and
                                  // nbd's parallelism is its client connection count --
                                  // neither has a queue count to declare

    uint32_t queue_depth = 0;     // per-queue in-flight limit; 0 = auto, clamped by kernel limits.
                                  // tcmu: SCSI command dispatch depth (coroutine pool capacity,
                                  // default 64); the kernel ring itself holds many more entries
                                  // nbd: bounds the request COUNT only. Outstanding request-buffer
                                  // BYTES are capped separately at 512 MiB, so with maximum-size
                                  // (32 MiB) requests at most 16 are in flight however high this
                                  // is set -- raising it helps small-request concurrency, not
                                  // large-block throughput
                                  // vhost-user: the in-flight cap is the lesser of this and the
                                  // ring the frontend chose in SET_VRING_NUM. 0 leaves that ring
                                  // size as the whole bound, so a caller who wants a limit has to
                                  // ask for one -- the frontend will not set it for you
                                  // vduse: also offered to the driver as the ring capacity
                                  // (VDUSE_VQ_SETUP's max_size; default 256, never above 1024), so
                                  // there it bounds the ring as well as the requests inside it

    uint32_t stack_size = DEFAULT_STACK_SIZE;
                                  // Stack of each serving coroutine named above, handed
                                  // straight to photon::thread_create -- so 0 asks for the
                                  // same thing this default already is. How MANY of them
                                  // there are follows the in-flight limits described above
                                  // rather than anything set here: one per request in
                                  // flight, plus one per nbd client connection. Each costs
                                  // a VMA and address space, which is what
                                  // vm.max_map_count runs out of first. Raise it for a
                                  // backend IFile that recurses deeply or keeps large
                                  // buffers on its own stack; lower it when the count above
                                  // is high and address space is what binds.

    uint32_t spin_us = 0;         // how long a serving loop keeps busy-polling after the
                                  // last completion before it blocks on the kernel's event
                                  // source. 0 = never poll, always block: no idle CPU cost.
                                  // UINT32_MAX = always poll, never block: lowest latency,
                                  // one vCPU burned. Anything in between = poll for that
                                  // many microseconds of idleness, then block.
                                  // Read by tcmu (its ring pump) and ublk (its per-queue
                                  // pump) only. vduse and vhost-user serve through the
                                  // shared virtqueue engine, whose wait is a blocking
                                  // kickfd wait with a fixed re-poll budget rather than a
                                  // caller-tuned poll, and nbd serves one coroutine per
                                  // connection; none of the three reads this field

    uint32_t timeout = 30;        // seconds; kernel-side tolerance for daemon unavailability;
                                  // must cover the restart window. Maps to tcmu cmd_time_out +
                                  // qfull_time_out, vduse msg_timeout, and the nbd kernel
                                  // device timeout in loopback mode (NBD_SET_TIMEOUT);
                                  // ublk/vhost-user have no such timer and ignore it

    bool read_only = false;       // export a read-only device; the write path fails:
                                  // tcmu SCSI WP (handler-level) / ublk UBLK_ATTR_READ_ONLY /
                                  // vduse + vhost-user VIRTIO_BLK_F_RO / nbd NBD_FLAG_READ_ONLY

    explicit BlkConfig(const BlkDevInfo& i) : info(i) {}
    BlkConfig() = default;
};

// ---------------------------------------------------------------------------
// One TcmuHBA per configfs HBA directory (target/core/user_N): it owns the
// TCM-USER genetlink subscription, the startup configfs scan and the reply
// channel. Devices are created THROUGH it (new_device), and that is load-bearing
// rather than stylistic: an event's dev_id exists only inside its netlink
// message, so the HBA is the only place a device can learn which reply it owes --
// it remembers every ADDED it hands out and gives the dev_id to the device built
// for that backstore name. There is no callback and no resolver: the caller maps
// dev_config to a backend and serves it itself.
//
// KERNEL FLOOR: upstream v4.13, RHEL/CentOS 7.6. Measured per release against
// tagged upstream sources (below) and per RHEL 7 minor against the shipped
// target_core_user.ko and its SRPM (after it), because the floors differ per
// feature -- and a vendor kernel's version number says nothing about its tcmu
// feature set:
//
//   v4.10   The TCM-USER genetlink family, its "config" multicast group and the
//           ADDED/REMOVED events (device name + uio minor) already exist, so an
//           HBA object constructs, subscribes and reports events. The
//           PER-BACKSTORE ATTRIBS do not: dev_config and dev_size arrive only in
//           v4.13, and start() writes them while validate_existing(), the startup
//           scan and list_orphans() read them. So on v4.10-v4.12 every event's
//           size reads 0 and no registration can be created OR adopted -- the
//           listener runs, but nothing can be served. (Those kernels take
//           dev_config/dev_size as an option string in the LIO-generic `control`
//           file instead; that path is not implemented here.)
//   v4.13   The dev_config / dev_size / emulate_write_cache attribs, the RECONFIG
//           event (carrying TCMU_ATTR_DEV_SIZE or DEV_CFG), TCMU_ATTR_DEVICE_ID,
//           and the whole command-reply protocol (SET_FEATURES +
//           SUPP_KERN_CMD_REPLY + the *_DONE commands). Below it there is no
//           dev_id, so every event means "no reply owed", deny() is a no-op,
//           resize() has nothing to write, and netlink_reply=true fails at
//           construction with a message saying so.
//   v4.15   The per-backstore nl_reply_supported opt-out -- what makes
//           netlink_reply=false safe beside a foreign reply-mode daemon. See
//           defensive_reply for the v4.13/v4.14 substitute.
//   v4.19   qfull_time_out, the restart-window half of BlkConfig::timeout; below
//           it only cmd_time_out applies and start() warns.
//
// VENDOR KERNELS: RHEL/CentOS 7 is 3.10-based yet backports tcmu wholesale, and
// the backport moved twice, so the minor version is what matters:
//
//   7.0-7.2 no target_core_user at all (verified in the 7.0/7.1 source trees):
//           new_tcmu_hba() fails while resolving the genetlink family.
//   7.3-7.5 a MIXED set, and the trap. The uapi is already the full v4.13+ one
//           (mailbox v2, CAP_OOOC, RECONFIG, the three *_DONE commands,
//           DEVICE_ID, SUPP_KERN_CMD_REPLY), so an HBA constructs, subscribes,
//           reports events with a usable dev_id and can engage the reply
//           protocol -- but the configfs side is still v4.12-shaped. 7.4 exposes
//           only cmd_time_out beside the passthrough geometry attribs, 7.5 adds
//           dev_size / qfull_time_out / max_data_area_mb, and NEITHER has
//           attrib/dev_config, emulate_write_cache or nl_reply_supported.
//           dev_config and dev_size are settable only as an option string in the
//           LIO-generic `control` file, which is not implemented here, so
//           start() fails ENOTSUP on the missing attrib. Having no
//           nl_reply_supported while HAVING the reply protocol puts these
//           releases squarely in the defensive_reply case -- and they also have
//           no reset_netlink escape hatch, which arrives with 7.6.
//   7.6+    the full set: dev_config, dev_size, emulate_write_cache,
//           cmd_time_out, qfull_time_out, nl_reply_supported, max_data_area_mb,
//           reset_ring, block_dev -- a v5.4-generation backport, verified line
//           by line against the 7.9 SRPM's target_core_user.c. Everything this
//           header describes works and nothing warns. The supported floor.
//
// Two RHEL hazards, both read out of the 7.9 source, both making the reply wait
// easier to wedge than upstream. Neither is detectable from userspace and
// neither changes what we do; they widen the blast radius of
// defensive_reply=false below.
//   - Its tcmu_netlink_event() converts a multicast -ESRCH (nobody listening)
//     into success and then waits regardless of which command it sent, so with a
//     foreign reply-mode flag up and no listener, the remove and resize writes
//     hang as well. Upstream waits only for ADDED and disarms the other two.
//   - It has no tcmu_destroy_genl_cmd_reply(), so a multicast failing with
//     anything other than -ESRCH leaves the command armed and every later event
//     on that device fails with -EBUSY ("netlink cmd N already executing").
//
// Every gap fails loudly, naming the feature and its version; none degrades
// silently.
// ---------------------------------------------------------------------------
class TcmuHBA : public Object {
public:
    struct Config : BlkConfig {
        bool loopback_lun = true;     // also create a tcm_loop LUN, so that a local
                                      // /dev/sdX appears for this backstore

        bool adopt_external = false;  // serve a backstore an EXTERNAL operator created
                                      // (targetcli/rtslib/overlaybd). Its dev_config is
                                      // theirs rather than this HBA's
                                      // "<dev_config_prefix><identity>", so that ownership
                                      // check is skipped for it. info.identity must still
                                      // name the backstore and info.size still match its
                                      // dev_size.

        std::string loopback_wwn;     // tcm_loop WWN of the loopback_lun target. REQUIRED
                                      // when loopback_lun is set, and never derived: the
                                      // tcm_loop WWN space is host-wide, so two
                                      // applications that derived one from the same
                                      // identity would hang their LUNs off a single target
                                      // (see the note at the top of this file). Must stay
                                      // stable across restarts.

        Config() = default;
        explicit Config(const BlkDevInfo& i) : BlkConfig(i) {}
    };

    enum class EventKind : uint8_t {
        ADDED,      // a backstore was enabled: serve it, or reply a failure
        REMOVED,    // it is being destroyed: stop serving, then the kernel proceeds
        RECONFIG    // the operator changed an attribute: which one is in attr, a
                    // requested dev_size in size
    };

    struct Event {
        uint64_t size = 0;          // ADDED: the backstore's dev_size, so a BlkDevInfo can
                                    // be built from the event alone. RECONFIG of dev_size:
                                    // the size the operator asked for -- the kernel commits
                                    // it only after the reply, so the attrib still reads the
                                    // old one

        uint32_t dev_id = 0;        // the kernel's dev_index -- the SAME in all three events
                                    // and the key a *_DONE reply is matched by; 0 = none owed

        EventKind kind;             // which of the three this is; see EventKind

        bool synthesized = false;   // from the startup configfs scan rather than a live
                                    // event: that configure already completed, so no reply
                                    // is owed (and dev_id is 0)

        char bs_name[256];          // backstore name under the HBA = the serving identity

        char dev_config[256];       // that backstore's dev_config attrib: this HBA's own
                                    // "<dev_config_prefix><identity>" for one it created,
                                    // the operator's string for an external one. Map it to
                                    // a backend.

        char uio_node[64];          // "/dev/uioN" of an ADDED device, "" otherwise

        char attr[16];              // RECONFIG only: the attribute changed, "dev_size" or
                                    // "dev_config" (the latter carries its new value in
                                    // dev_config and cannot be answered by resize())
    };

    // The next event, blocking up to tmo (default: forever). 0 = *out filled,
    // -1 + errno (ETIMEDOUT on expiry). Runs on the caller's photon vcpu; events
    // arriving while the caller is busy serving are queued, not dropped.
    virtual int wait_for_event(Event* out, Timeout tmo = {}) = 0;

    // Backstores this HBA created -- ones whose dev_config is its own
    // "<dev_config_prefix><identity>" -- that no live server holds the flock for:
    // crash recovery, with the identity as the recovery key. The startup scan
    // additionally SYNTHESIZES ADDED events for every unserved backstore under the
    // HBA, external ones included, so a single event loop covers both the backlog
    // and whatever arrives later.
    virtual std::vector<BlkDevInfo> list_orphans() = 0;

    // Remove one orphan -- the configfs backstore registration, and the tombstone
    // that stands for it. 0 once both are gone, after which list_orphans() no
    // longer reports the identity; -1 + errno otherwise.
    //
    // The identity is CALLER-SUPPLIED and names something to delete, so it goes
    // through the same name-mapping the device path uses -- every character
    // outside [alnum . _ -] becomes '_', and the result is capped at 64 bytes --
    // and is never spelled into a path directly. A '/' therefore cannot survive,
    // which is what keeps this from being an rmdir of any directory the caller
    // can name. That mapping does allow '.', so "." and ".." are rejected by name
    // as well: they are whole path components rather than backstore names, and
    // left alone they would aim the removal at this HBA's own directory and at its
    // parent. An identity longer than a backstore name can be is EINVAL -- it
    // would name the PREFIX's backstore, not the caller's -- and one this HBA has
    // no backstore for is ENOENT.
    //
    // EBUSY is the live-identity refusal, and it is what makes the call safe on a
    // BlkDevInfo taken from an earlier scan: a server may have adopted the
    // registration since, so the flock is probed again here rather than trusted
    // from the listing. The kernel's own refusal -- a LUN still references the
    // backstore -- surfaces as EBUSY too.
    //
    // The registration goes first, the order shutdown() tears down in. So a
    // tombstone that is not a file is reported as EISDIR *after* the registration
    // is already gone: the orphan is recovered, and the -1 says the lock directory
    // still needs a human. A directory there is operator state, and is never
    // removed on the operator's behalf.
    virtual int destroy_orphan(const BlkDevInfo& orphan) = 0;

    // A device of this HBA, built from cfg -- which is validated here, so a bad
    // config gives nullptr + errno instead of an object. The device answers the
    // kernel wherever an operator is waiting: ADDED_DEVICE_DONE from start() (0,
    // or the failure errno, which fails the operator's enable),
    // REMOVED_DEVICE_DONE from detach()/shutdown() -- after serving has stopped,
    // because the kernel unregisters the uio right after the wait -- and
    // RECONFIG_DEVICE_DONE from resize().
    //
    // The ADDED needs no argument. cfg.info.identity IS the backstore name, the
    // HBA remembers the dev_id of every ADDED it hands out, and construction
    // claims the one recorded under that name -- so the device that serves an
    // operator's backstore is necessarily the device that unblocks the operator's
    // `echo 1 > enable`; building one and forgetting the other is not expressible.
    // A REMOVED or a RECONFIG dev_id arrives later through the same registry,
    // which is why resize() takes no event either.
    //
    // tcmu adds nothing to IBlkDevice, but its resize() has transport-specific
    // semantics: it is declarative and doubles as the answer to an
    // operator-driven RECONFIG. If the HBA holds a pending dev_size event for
    // this device, resize(X) applies X and replies RECONFIG_DEVICE_DONE instead
    // of writing configfs -- a shrink or a misaligned X is vetoed with a
    // negative status, which fails the operator's write and leaves the old size
    // committed. With no pending event it writes dev_size itself (the kernel
    // commits it once the HBA has answered the resulting event). Both halves
    // need the dev_size attrib and the RECONFIG event -- upstream v4.13+,
    // RHEL/CentOS 7.5+ for the attrib alone, 7.6 for a device that can be
    // started at all (see KERNEL FLOOR); older kernels fail with ENOENT.
    virtual IBlkDevice* new_device(const Config& cfg) = 0;

    // Refuse an event -- the only answer no device method covers: an ADDED
    // nobody claims (without it the operator's `echo 1 > enable` blocks forever,
    // recoverable only via reset_netlink) and a dev_config RECONFIG (resize()
    // answers the dev_size ones). err is the reason, a positive errno: the
    // operator's configfs write fails with -err. Accepting is deliberately not
    // expressible here -- an ADDED is accepted by serving it (new_device + start,
    // which answers the kernel), so it is impossible to acknowledge a device
    // nobody serves. Refusing one also drops the HBA's memory of it, so a device
    // constructed later for the same name cannot answer that dev_id a second
    // time -- the kernel may have recycled it onto another backstore by then.
    // Below v4.13 events carry no dev_id at all, so nothing is
    // ever owed and this returns 0 having done nothing.
    virtual int deny(const Event& ev, int err) = 0;
};

// The first three arguments each name a host-wide namespace and none has a default
// -- see the note at the top of this file.
//
// subtype is the configfs HBA directory this instance claims, under target/core/.
// The kernel creates it on demand. It has to be a TCM-USER fabric directory, so its
// name has the form user_<N>, and the <N> has to be one no other application of
// yours on this host already uses: two HBAs on one directory see each other's
// backstores, and this one's startup scan then synthesizes ADDED events for them.
//
// dev_config_prefix is the ownership tag written into the dev_config attrib of every
// backstore this HBA creates, as "<dev_config_prefix><identity>". It is what
// list_orphans() and the startup scan match on to tell a backstore this HBA created
// from an operator's, and what a device's start() checks the registration it adopts
// against -- so it has to be the same string across restarts, and yours alone: an
// application sharing it sees this one's backstores as its own orphans. Bounded to
// 63 bytes so that the prefix plus the longest backstore name still fits the
// 256-byte attrib.
//
// lock_dir is the directory holding this HBA's tombstone files, one flock per
// backstore, which is how a live server marks a registration as served. It is this
// HBA's SCOPE: list_orphans() probes it AND the devices new_device() builds claim
// their tombstones in it. That is the same coupling UblkController states, and why
// TcmuHBA::Config carries no lock_dir of its own. Bounded as there, because a
// truncated path is a different directory.
//
// netlink_reply engages the kernel's command-reply protocol
// (TCMU_CMD_SET_FEATURES), which is module-GLOBAL: afterwards EVERY tcmu
// backstore on the host blocks the operator's configfs write until a userspace
// *_DONE. That buys veto power and "enable returns once the device is served",
// and the flag is restored when this object is deleted. Run at most ONE instance
// with it per host -- the kernel completes a waiting command on the first
// matching reply, so two would race -- and understand that such an instance
// answers for the WHOLE host: it refuses other HBAs' events so an operator's
// write cannot hang forever on a backstore nobody serves. Needs v4.13+; below
// that, construction fails (see KERNEL FLOOR above). A device whose enable was
// ALREADY blocked at that point cannot be answered (its dev_id is only in the
// missed event); recover it by writing 1 to both
// /sys/module/target_core_user/parameters/block_netlink and then reset_netlink --
// the reset refuses with "Netlink is not blocked" unless the block came first.
//
// defensive_reply is the caller's choice for kernels WITHOUT the per-backstore
// opt-out (upstream v4.13/v4.14, and RHEL/CentOS 7.3-7.5, which backported the
// whole reply protocol but not the nl_reply_supported attrib), where a foreign
// daemon's module-global flag cannot be declined per device. The kernel arms its
// wait whenever that flag is up -- whoever raised it -- and the flag has no
// getter, so we cannot detect it:
//   false (default)  stay silent unless we engaged the protocol ourselves. No
//                    noise, because an answer nobody waits for costs the kernel a
//                    pr_err ("could not find device with dev id N"). Risk: if
//                    ANOTHER daemon raised the flag, our own start() hangs inside
//                    `echo 1 > enable` forever and uninterruptibly (the kernel's
//                    wait_for_completion has no timeout) -- and on RHEL, where
//                    that wait is armed for every command rather than just ADDED,
//                    the remove and resize writes hang too. Recoverable only by
//                    the block_netlink + reset_netlink pair above.
//   true             answer every event of our OWN HBA whether or not we engaged
//                    the protocol, so a foreign flag can never hang us. Cost: one
//                    such pr_err per device-lifecycle event (enable, remove,
//                    reconfig) in the normal case where nobody is waiting.
// Other HBAs' events are ignored either way -- refusing those is the privilege of
// an instance that engaged the protocol, since it alone is the host's authority.
// On v4.15+ (RHEL/CentOS 7.6+) this is moot for backstores we CREATE (they are
// opted out per device with nl_reply_supported=-1, so no wait is ever armed for
// them); it still matters for adopted external ones, whose opt-out is the
// operator's to set.
//
// nullptr + errno on failure: a missing or empty required argument (EINVAL), an
// over-long one (ENAMETOOLONG), a subtype that is not a TCM-USER fabric name
// (EINVAL), the genetlink family missing, or SET_FEATURES failed.
TcmuHBA* new_tcmu_hba(const char* subtype,
                      const char* dev_config_prefix,
                      const char* lock_dir,
                      bool netlink_reply = false,
                      bool defensive_reply = false);

// ---------------------------------------------------------------------------
// One UblkController per lock directory -- the SCOPE, and the only place it is
// stated. Devices are created through it and orphans are listed through it, so a
// recovery loop cannot scan one directory and claim in another. That matters more
// here than anywhere else: for ublk the tombstone flock is the ONLY ownership test
// there is (the dev_id space is host-wide and flat, /dev/ublk-control is a
// singleton), so a scan and a claim that disagreed about the directory would both
// succeed and hand one device to two daemons.
//
// A registration whose tombstone lives in another directory is simply not ours --
// the scan skips it, because a MISSING lock file is not a free one -- which is how
// a host runs several independent ublk daemons side by side.
// ---------------------------------------------------------------------------
class UblkController : public Object {
public:
    struct Config : BlkConfig {
        uint32_t dev_id = UINT32_MAX; // ublk has no uuid; the dev_id IS the recovery identity;
                                      // UINT32_MAX = kernel auto-assign; otherwise requests
                                      // /dev/ublkb<N> (0 is a valid requestable id)

        uint32_t quiesce_timeout_ms = 3000; // bounded wait for the kernel's ASYNC quiesce
                                            // transition: detach() before returning, the
                                            // START_USER_RECOVERY EBUSY poll and the LIVE-daemon
                                            // probe in start(); 0 = do not wait

        uint32_t stop_timeout_ms = 2000;    // shutdown(): how long to retry TRY_STOP_DEV against
                                            // transient openers (partition scan, udev probes)
                                            // before reporting genuine EBUSY; 10ms granularity

        uint64_t flags = 0;           // raw UBLK_F_* bits (see linux/ublk_cmd.h);
                                      // UBLK_F_USER_RECOVERY | UBLK_F_USER_RECOVERY_REISSUE
                                      // recommended for daemon-restart resilience;
                                      // UBLK_F_UPDATE_SIZE required for resize()

        explicit Config(const BlkDevInfo& i) : BlkConfig(i) {}
        Config() = default;
    };

    // ublk adds nothing to IBlkDevice. get_device_node() is the /dev/ublkbN, whose
    // numeric suffix is the dev_id -- ublk's recovery identity (it has no uuid).
    // nullptr + errno if cfg is invalid; the identity is fixed here (see
    // IBlkDevice). The device claims its tombstone in THIS controller's lock_dir.
    virtual IBlkDevice* new_device(const Config& cfg) = 0;

    // Registered devices (QUIESCED or FAIL_IO, i.e. left behind by a detached or
    // crashed daemon) whose flock in this lock_dir is free. identity is the decimal
    // dev_id -- the recovery key: build a cfg from the returned BlkDevInfo, hand it
    // to new_device(), and start(backend).
    virtual std::vector<BlkDevInfo> list_orphans() = 0;

    // Remove one orphan: the kernel-side registration (DEL_DEV) and the tombstone
    // that stands for it. 0 once both are gone, after which list_orphans() no
    // longer reports the dev_id; -1 + errno otherwise.
    //
    // The identity is CALLER-SUPPLIED and names something to delete, so it must be
    // decimal digits throughout: leading white space and a leading '+' both parse
    // as the device they spell, and the permissive parse a scan can afford is not
    // one this call can, because the dev_id space is host-wide and flat -- parsing
    // "garbage" as 0 would aim DEL_DEV at somebody else's device. UINT32_MAX is
    // rejected too: it is Config::dev_id's "let the kernel choose" sentinel, not a
    // device. Both are EINVAL, with nothing deleted.
    //
    // EBUSY here means exactly one thing: another LIVE SERVER holds the tombstone.
    // The flock is CLAIMED and held across the DEL_DEV rather than merely probed,
    // because DEL_DEV is unconditional -- no EBUSY, no state check -- so a
    // probe-then-destroy pair leaves a window in which another daemon adopts the
    // device and this call destroys a live one. That is the same gate shutdown()
    // re-claims the lock for.
    //
    // An INITIATOR holding /dev/ublkbN is not a refusal and does not produce EBUSY.
    // DEL_DEV removes the node and then WAITS for the last opener to close before it
    // returns, and returns success: measured on a quiesced orphan with a process
    // holding the node open and issuing nothing to it, the call took 227s and came
    // back 0, and came back at once when that holder was killed. So this call is
    // UNBOUNDED -- it blocks the calling coroutine for as long as anything holds the
    // node. It does not block the vcpu: the wait is a kernel wait on an io_uring
    // worker while the caller's vcpu stays in its event loop, so other coroutines
    // there keep running. A caller that cannot afford to wait has to establish that
    // the node is unheld before calling: nothing in this signature bounds it, and a
    // timeout knob would not either, because there is nothing to retry -- the call is
    // already inside the kernel rather than waiting to be issued. The bounded loop
    // shutdown() drives with stop_timeout_ms sits around TRY_STOP_DEV, which is the
    // step that answers EBUSY, and this call has no reason to stop a device it is
    // about to delete.
    //
    // No such device and no tombstone either is ENOENT. A tombstone with no
    // registration is our own litter and is removed, returning 0 -- unless a live
    // server holds it, which means it is between claiming the dev_id and ADD_DEV, and
    // that is EBUSY: removing the file would leave the device it then creates with no
    // tombstone, and list_orphans() skips an entry whose tombstone is missing, so it
    // would stop being reported by every later scan while still being in the kernel.
    virtual int destroy_orphan(const BlkDevInfo& orphan) = 0;
};

// lock_dir is the directory holding this controller's tombstone files -- one flock
// per dev_id, which is how a live server marks a registration as served, and the
// SCOPE described above. There is no default, because it names a host-wide namespace
// (see the note at the top of this file). It is bounded to 255 bytes so that what the
// controller stores is what it uses: an over-long path would be truncated into a
// DIFFERENT directory, silently reintroducing the divergence this class exists to
// prevent. nullptr/"" + errno (EINVAL), or too long + errno (ENAMETOOLONG).
UblkController* new_ublk_controller(const char* lock_dir);

// ---------------------------------------------------------------------------
// One VhostUserController per SOCKET directory. vhost-user has no lock dir and no
// ownership tombstone -- a dead socket file is an ops artifact, not a recovery key
// (recovery is a blind re-listen via start()) -- so its scope means something
// narrower than ublk's and vduse's: the directory we LISTEN in and the directory
// we SCAN are the same one. new_device() therefore requires cfg.sock_path to sit
// inside it, for BOTH roles: a CLIENT-role socket belongs to the initiator
// (QEMU/libvirt), and the directory holding it is exactly the one worth scanning
// for listeners that have gone away. The containment test is textual, so a
// relative or symlinked spelling of the same directory does not match --
// resolving it would mean I/O in a factory, and a factory must not block.
// ---------------------------------------------------------------------------
class VhostUserController : public Object {
public:
    enum class SockRole : uint8_t {
        SERVER,     // this process listens on sock_path; a path held by a LIVE backend is
                    // EBUSY, a socket whose listener is CONFIRMED gone is unlinked and
                    // re-bound. A probe that was denied or that timed out is not
                    // confirmation, and neither is a node that is not a socket (EINVAL) --
                    // start() refuses both rather than remove what it cannot prove is dead.
        CLIENT      // the initiator (e.g. QEMU with server=on) holds the listener; this process
                    // connects to it -- needed when the socket dir is owned/privileged (libvirt)
    };

    struct Config : BlkConfig {
        SockRole sock_role = SockRole::SERVER;   // which end of sock_path this process is;
                                                 // see SockRole

        uint32_t sock_mode = 0600;  // permission bits chmod'd onto the socket node in the
                                    // SERVER role, i.e. who may connect to it. 0600 admits
                                    // the owner's uid alone (root excepted), so a caller
                                    // whose guest process (qemu) runs as a different user
                                    // has to widen it. Not derived from umask.

        std::string sock_path;      // the unix socket this device listens on (SERVER) or
                                    // connects to (CLIENT). Must sit inside this
                                    // controller's sock_dir.

        Config() = default;
        explicit Config(const BlkDevInfo& i) : BlkConfig(i) {}
    };

    // vhost-user adds nothing to IBlkDevice, and get_device_node() is always
    // nullptr: nothing here touches the kernel, so the only block device that ever
    // exists is the guest's. nullptr + errno if cfg is invalid -- which includes a
    // sock_path outside this controller's directory (EINVAL). The identity is fixed
    // here (see IBlkDevice).
    virtual IBlkDevice* new_device(const Config& cfg) = 0;

    // Unix sockets in this directory whose listener is gone (a connect probe gets
    // ECONNREFUSED). identity is the socket path; the rest of the descriptor stays
    // zero, because vhost-user keeps no kernel-side registry and the device's
    // geometry is only known once a guest connects and negotiates.
    virtual std::vector<BlkDevInfo> list_orphans() = 0;

    // Remove one orphan -- here the socket itself, which is the whole of a
    // vhost-user registration. 0 once it is gone, after which list_orphans() no
    // longer reports it; -1 + errno otherwise, and nothing is removed.
    //
    // The identity is CALLER-SUPPLIED and names something to delete, so it gets
    // the containment check new_device() applies to a sock_path: a path outside
    // this controller's directory is EINVAL. So is a path that is not a socket,
    // and a probe error is propagated. A socket whose listener is still live is
    // EBUSY -- list_orphans() filters those out, but a caller's BlkDevInfo may
    // predate another daemon re-binding the path, which is the window this
    // exists to close. Nothing there at all is ENOENT.
    //
    // stat() follows symlinks, matching list_orphans(); that cannot let a link
    // delete something outside the directory, because unlink() removes the link
    // itself and never its target.
    virtual int destroy_orphan(const BlkDevInfo& orphan) = 0;
};

// sock_dir is the directory this controller BINDS its listening sockets in and SCANS
// for listeners that have gone away -- one directory playing both roles, which is why
// new_device() requires cfg.sock_path to sit inside it. There is no default: it names
// a host-wide namespace (see the note at the top of this file). Bounded to 255 bytes,
// so that what the controller stores is what it uses.
//
// A SERVER start also leaves one small non-socket file per socket in this
// directory: the identity lock that keeps two daemons from taking over the same
// path at once. It is created at start and is not removed at shutdown, and
// list_orphans() does not report it, because that scan reads this directory as a
// set of sockets.
// nullptr/"" + errno (EINVAL), or too long + errno (ENAMETOOLONG).
VhostUserController* new_vhost_user_controller(const char* sock_dir);

// ---------------------------------------------------------------------------
// One VduseController per lock directory, for the same reason as ublk's: the
// vduse name space is host-wide and flat (/dev/vduse/control is a singleton), so
// the tombstone flock is what makes a registration ours -- and the scan and the
// claim have to agree on where it lives.
// ---------------------------------------------------------------------------
class VduseController : public Object {
public:
    // vduse adds nothing to IBlkDevice, and get_device_node() is always nullptr:
    // the /dev/vdX is created by an EXTERNAL consumer (`vdpa dev add mgmtdev
    // vduse`, or QEMU's vhost-vdpa) asynchronously and outside our control, so it
    // is not ours to name -- a caller that needs it must discover it.
    // nullptr + errno if cfg is invalid; the identity is fixed here (see
    // IBlkDevice). The device claims its tombstone in THIS controller's lock_dir.
    //
    // Takes a plain BlkConfig: vduse has no transport-specific knob, so a Config
    // of its own would be an empty derivation contributing only a name.
    virtual IBlkDevice* new_device(const BlkConfig& cfg) = 0;

    // Devices that are OURS and unheld, which takes two tests. Ownership: a
    // tombstone in this lock_dir whose flock is free -- it scopes the scan the way
    // the HBA directory scopes tcmu's. Liveness: the char device admits a single
    // opener, so a successful probe open still catches a foreign daemon that takes
    // no tombstone. identity is the device name; the capacity is not recoverable
    // (the kernel never learns it), so the record carries size 0 plus best-effort
    // features and the recovery cfg must declare the size itself.
    virtual std::vector<BlkDevInfo> list_orphans() = 0;

    // Remove one orphan: the vduse registration (DESTROY_DEV) and the tombstone
    // that stands for it. 0 once both are gone, after which list_orphans() no
    // longer reports the name; -1 + errno otherwise.
    //
    // The identity is the device NAME, and it is CALLER-SUPPLIED and names something
    // to delete. Unlike tcmu there is no character mapping here: both the
    // registration path and the tombstone name take it VERBATIM, so the two checks
    // new_device() runs on a name are run again on this one -- 1..255 chars, and no
    // '/'. A '/' would aim the registration probe at some other directory. "." and
    // ".." are rejected too: they are whole path components rather than device
    // names, and both resolve -- to /dev/vduse and to /dev -- so the probe would
    // succeed against a directory that is no device at all and the call would go on
    // to report success about nothing. All three are EINVAL with nothing deleted.
    // The geometry checks new_device() also runs are deliberately NOT repeated: a
    // record from list_orphans() carries size 0 because the capacity is not
    // recoverable, so requiring it would make every orphan undestroyable.
    //
    // EBUSY is the live refusal and it has three sources, checked in this order:
    // a live server holds the tombstone; a FOREIGN daemon is connected to the
    // single-opener char device, which the tombstone cannot tell us about; or a vdpa
    // consumer is still attached, which is what /sys/bus/vdpa/devices/<name> is for.
    // That entry does not bracket the kernel-side vdev exactly -- see the next
    // paragraph. The claim is taken and held across the DESTROY_DEV rather than
    // merely probed, so that no other server of this implementation adopts the
    // device in between.
    //
    // The consumer is refused by that check BEFORE DESTROY_DEV is issued, which is
    // where shutdown() refuses it as well and what its own busy case witnesses. The
    // check is a fast path and not the authority: a `vdpa dev del` that wedges part
    // way through removal leaves the sysfs entry gone while the kernel side is still
    // bound, and measured in exactly that state the pre-check passed, nothing held
    // the char device, and DESTROY_DEV still answered EBUSY. So the ioctl's EBUSY is
    // handled too, for that and for a consumer that attaches in between. What is NOT
    // claimed here is anything about waiting: ublk's DEL_DEV was measured to WAIT for
    // the last opener rather than refuse it, and no equivalent measurement was made
    // of vduse's.
    //
    // No such device and no tombstone either is ENOENT. A tombstone with no
    // registration is our own litter and is removed, returning 0 -- unless a live
    // server holds it, which means it is between claiming the name and CREATE_DEV,
    // and that is EBUSY.
    virtual int destroy_orphan(const BlkDevInfo& orphan) = 0;
};

// lock_dir is the directory holding this controller's tombstone files -- one flock
// per device name -- and so its SCOPE, bounded and default-free exactly as for ublk
// (see the note at the top of this file).
// nullptr/"" + errno (EINVAL), or too long + errno (ENAMETOOLONG).
VduseController* new_vduse_controller(const char* lock_dir);

// nbd has no controller to nest this in -- there is no scope to hold, because an
// export leaves no persistent kernel-side state (see the note below) -- so its
// config stays at namespace scope.
struct NbdConfig : BlkConfig {
    // UDS, TCP and the loopback device can be enabled simultaneously, and at least
    // one of the three must be.
    bool enable_tcp = false;        // also serve on tcp_endpoint

    bool loopback_device = true;    // also attach the export to a free local /dev/nbdN
                                    // kernel device, with this library playing the role
                                    // of nbd-client; read the node back via
                                    // get_device_node(). LINUX-ONLY: on any other
                                    // platform start() fails with ENOSYS.

    net::EndPoint tcp_endpoint;     // the TCP endpoint to serve on when enable_tcp is
                                    // set. port 0 lets the kernel choose; the listener
                                    // get_server_sockets() returns then carries it.

    std::string unix_path;          // the unix socket path to serve on; empty = no UDS.
                                    // start() refuses the path rather than clear it when a
                                    // live server holds it (EBUSY), when what is there is
                                    // not a socket (EINVAL), or when probing it reached no
                                    // verdict -- as for VhostUserController::SockRole.

    uint32_t stall_timeout = 30;    // seconds; how long an INCOMPLETE message may stall
                                    // before its connection is dropped. Two reads are
                                    // bounded by it: the handshake, which holds a
                                    // connection slot and that connection's coroutine
                                    // stack, and the payload of a WRITE whose header has
                                    // already arrived, which holds a queue-depth slot and
                                    // its share of the byte budget -- so a client that
                                    // sends a header and then nothing would otherwise
                                    // hold both until it felt like finishing, and
                                    // queue_depth would bound honest clients only.
                                    // `timeout` does not cover this: that one is the
                                    // kernel's request timeout for the loopback device
                                    // and releases nothing on this side of the socket.
                                    // Idle time BETWEEN requests is not a stall and is
                                    // not bounded. 0 = no deadline.

    NbdConfig() = default;
    explicit NbdConfig(const BlkDevInfo& i) : BlkConfig(i) {}
};

class NbdDevice : public IBlkDevice {
public:
    struct SocketServers {
        net::ISocketServer* uds = nullptr;   // the unix_path listener, or nullptr

        net::ISocketServer* tcp = nullptr;   // the enable_tcp listener, or nullptr
    };

    // The listening server sockets after start(). With tcp_endpoint.port == 0
    // the kernel picks the port: read it back via getsockname().
    virtual SocketServers get_server_sockets() = 0;

    // Connections of the currently connected nbd clients (the loopback
    // device's connection counts as one)
    virtual std::vector<net::ISocketStream*> get_client_connections() = 0;

    // get_device_node() is the /dev/nbdN attached for cfg.loopback_device
    // (e.g. "/dev/nbd3"); nullptr when loopback is off or before start()
};

// No list_nbd_orphans(): an export leaves no persistent kernel-side state, so
// nothing outlives the process that served it (a loopback /dev/nbdN is left
// disconnected, not orphaned).

// nullptr + errno if cfg is invalid; the identity is fixed here (see IBlkDevice)
NbdDevice* new_nbd_device(const NbdConfig& cfg);

} // namespace blk
} // namespace photon
