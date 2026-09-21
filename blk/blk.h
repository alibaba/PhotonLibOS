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
#include <photon/common/timeout.h>   // Timeout for TcmuHBA::wait_for_event
#include <photon/fs/filesystem.h>
#include <photon/net/socket.h>    // net::IPAddr / net::EndPoint for NbdConfig / NbdDevice

namespace photon {
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

enum class PollPolicy : uint8_t {
    SLEEP,      // block on the kernel event source (uio fd / uring / kick fd); zero idle cost
    SPIN,       // busy-poll for the lowest latency; burns a vCPU
    ADAPTIVE,   // spin while busy, fall back to SLEEP after BlkConfig::spin_us idle
};

struct BlkConfig {
    BlkDevInfo info;

    uint32_t queues = 0;          // serving parallelism; 0 = transport-chosen default.
                                  // tcmu ignores it: the kernel provides one command ring per device

    uint32_t queue_depth = 0;     // per-queue in-flight limit; 0 = auto, clamped by kernel limits.
                                  // tcmu: SCSI command dispatch depth (coroutine pool capacity,
                                  // default 64); the kernel ring itself holds many more entries
                                  // nbd: bounds the request COUNT only. Outstanding request-buffer
                                  // BYTES are capped separately at 512 MiB, so with maximum-size
                                  // (32 MiB) requests at most 16 are in flight however high this
                                  // is set -- raising it helps small-request concurrency, not
                                  // large-block throughput

    uint32_t stack_size = 0;      // coroutine stack for the per-request / per-tag serving
                                  // coroutines; 0 = the module default (DEFAULT_REQ_STACK,
                                  // 256 KiB), NOT photon's 8 MiB. How many of these exist is
                                  // the peer's choice -- one per virtqueue entry, per tcmu ring
                                  // command, per nbd connection -- and each costs a VMA, so
                                  // 8 MiB apiece exhausts vm.max_map_count. Raise it if your
                                  // backend IFile recurses deeply or keeps large buffers on
                                  // its own stack

    uint32_t vcpus = 0;           // vCPUs serving the queues (round-robin); 0 = auto.
                                  // tcmu: 0/1 = serve on the caller's vcpu; >=2 = run the pump on
                                  // a dedicated vcpu (the ring is single, so more do not multiply)

    uint32_t spin_us = 0;         // PollPolicy::ADAPTIVE only: how long to keep busy-polling
                                  // after the last completion before sleeping; 0 = impl default

    uint32_t timeout = 30;        // seconds; kernel-side tolerance for daemon unavailability;
                                  // must cover the restart window. Maps to tcmu cmd_time_out +
                                  // qfull_time_out, vduse msg_timeout, and the nbd kernel
                                  // device timeout in loopback mode (NBD_SET_TIMEOUT);
                                  // ublk/vhost-user have no such timer and ignore it

    bool read_only = false;       // export a read-only device; the write path fails:
                                  // tcmu SCSI WP (handler-level) / ublk UBLK_ATTR_READ_ONLY /
                                  // vduse + vhost-user VIRTIO_BLK_F_RO / nbd NBD_FLAG_READ_ONLY

    PollPolicy poll = PollPolicy::SLEEP;

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
        // Field order is padding-driven, do not tidy it: BlkConfig's content ends
        // at offset 82 (its sizeof is 88), and the ABI lets a derived class place
        // members in the base's tail padding -- so both bools land at 82/83 and
        // the string then needs no hole before it. 120 bytes; putting the string
        // first costs 8 more.
        bool loopback_lun = true;     // also create a tcm_loop LUN so a local /dev/sdX appears
        bool adopt_external = false;  // serve a backstore an EXTERNAL operator created
                                      // (targetcli/rtslib/overlaybd): its dev_config is theirs
                                      // rather than "photon/<identity>", so that check is
                                      // skipped. info.identity must still name the backstore
                                      // and info.size still match its dev_size.
        std::string loopback_wwn;     // tcm_loop WWN; empty = derived deterministically from
                                      // info.identity (must stay stable across restarts)
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
        // Field order is padding-driven, do not tidy it: the four char arrays are
        // 592 bytes of align-1, so putting them last lets the wide members pack
        // from offset 0 with no hole. 608 bytes; leading with `kind` costs 8 more
        // (the arrays end at 593 and `size` then has to skip to 600).
        uint64_t size = 0;          // ADDED: the backstore's dev_size, so a BlkDevInfo can
                                    // be built from the event alone. RECONFIG of dev_size:
                                    // the size the operator asked for -- the kernel commits
                                    // it only after the reply, so the attrib still reads the
                                    // old one
        uint32_t dev_id = 0;        // the kernel's dev_index -- the SAME in all three events
                                    // and the key a *_DONE reply is matched by; 0 = none owed
        EventKind kind;
        bool synthesized = false;   // from the startup configfs scan rather than a live
                                    // event: that configure already completed, so no reply
                                    // is owed (and dev_id is 0)
        char bs_name[256];          // backstore name under the HBA = the serving identity
        char dev_config[256];       // the operator's dev_config string; map it to a backend
        char uio_node[64];          // "/dev/uioN" of an ADDED device, "" otherwise
        char attr[16];              // RECONFIG only: the attribute changed, "dev_size" or
                                    // "dev_config" (the latter carries its new value in
                                    // dev_config and cannot be answered by resize())
    };

    // The next event, blocking up to tmo (default: forever). 0 = *out filled,
    // -1 + errno (ETIMEDOUT on expiry). Runs on the caller's photon vcpu; events
    // arriving while the caller is busy serving are queued, not dropped.
    virtual int wait_for_event(Event* out, Timeout tmo = {}) = 0;

    // Photon-created backstores (dev_config == "photon/<identity>") that no live
    // server holds the flock for -- crash recovery, with the identity as the
    // recovery key. The startup scan additionally SYNTHESIZES ADDED events for
    // every unserved backstore under the HBA, external ones included, so a single
    // event loop covers both the backlog and whatever arrives later.
    virtual std::vector<BlkDevInfo> list_orphans() = 0;

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

// subtype is the configfs HBA directory this instance claims, under
// target/core/ (e.g. "user_0"); lock_dir is the flock directory (nullptr =
// "/run/photon-blk") and is this HBA's SCOPE: list_orphans() probes it AND the
// devices new_device() builds claim their tombstones in it. That is the same
// coupling UblkController states, and why TcmuHBA::Config carries no lock_dir of
// its own. Bounded as there, because a truncated path is a different directory.
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
// nullptr + errno on failure (e.g. the genetlink family is missing, or
// SET_FEATURES failed).
TcmuHBA* new_tcmu_hba(const char* subtype = "user_0",
                      bool netlink_reply = false,
                      const char* lock_dir = nullptr,
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
        // Field order is padding-driven, and here it is deliberately ASCENDING by
        // width -- do not "fix" it. BlkConfig's content ends at offset 82, which is
        // not a multiple of 8, so a leading uint64_t would have to skip to 88 and
        // waste 6 bytes; leading with the uint32_t wastes only 2 (82 -> 84) and the
        // uint64_t then lands at 96 with nothing after it. 104 bytes vs 112.
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
};

// lock_dir is the flock directory (nullptr/"" = "/run/photon-blk"). It is bounded
// so that what the controller stores is what it uses: an over-long path would be
// truncated into a DIFFERENT directory, silently reintroducing the divergence this
// class exists to prevent. nullptr + errno (ENAMETOOLONG) if it is too long.
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
                    // EBUSY, a stale socket (dead listener) is unlinked and re-bound
        CLIENT      // the initiator (e.g. QEMU with server=on) holds the listener; this process
                    // connects to it -- needed when the socket dir is owned/privileged (libvirt)
    };

    struct Config : BlkConfig {
        // Field order is padding-driven, do not tidy it: `sock_role` fits in
        // BlkConfig's tail padding at offset 82 and `sock_mode` at 84, so the
        // string starts at 88 with no hole. 120 bytes; leading with the string
        // costs 8 more.
        SockRole sock_role = SockRole::SERVER;
        uint32_t sock_mode = 0;       // unix socket permission bits (SERVER role); 0 = 0666 &
                                      // ~umask; widen the group/other bits when the guest process
                                      // (qemu) runs as a different user
        std::string sock_path;
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
};

// sock_dir must be non-empty -- there is no default socket directory -- and is
// bounded like the others. nullptr + errno (EINVAL / ENAMETOOLONG) otherwise.
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
};

// lock_dir is the flock directory (nullptr/"" = "/run/photon-blk"), bounded as for
// ublk. nullptr + errno (ENAMETOOLONG) if it is too long.
VduseController* new_vduse_controller(const char* lock_dir);

// nbd has no controller to nest this in -- there is no scope to hold, because an
// export leaves no persistent kernel-side state (see the note below) -- so its
// config stays at namespace scope.
struct NbdConfig : BlkConfig {
    // UDS, TCP and loopback device can be enabled simultaneously.
    // Field order is padding-driven, do not tidy it. BlkConfig's content ends at
    // offset 82, and net::EndPoint is 18 bytes of align-1 -- so the two bools go
    // at 82/83 and the endpoint fills 84..102, which lets the 8-aligned string
    // start at 104 instead of leaving a hole. 136 bytes; putting the string
    // before the endpoint costs 8 more.
    bool enable_tcp = false;
    bool loopback_device = true;  // whether attach the export to a free local /dev/nbdN kernel
                                  // device, and this library plays the role of nbd-client;
                                  // read back the node via get_device_node().
    net::EndPoint tcp_endpoint;   // serve on this TCP endpoint if enable_tcp is set
    std::string unix_path;        // serve on this unix socket if non-empty
    NbdConfig() = default;
    explicit NbdConfig(const BlkDevInfo& i) : BlkConfig(i) {}
};

class NbdDevice : public IBlkDevice {
public:
    struct SocketServers {
        net::ISocketServer* uds = nullptr;   // unix_path mode
        net::ISocketServer* tcp = nullptr;   // enable_tcp mode
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
