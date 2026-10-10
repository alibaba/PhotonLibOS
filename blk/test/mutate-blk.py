#!/usr/bin/env python3
"""Mutation harness for the multiqueue guards in blk/utils.cpp, blk/ublk.cpp,
blk/tcmu.cpp and blk/nbd.cpp.

The companion to blk/test/mutate-vhost-user.py, and the second half of ONE
ledger. The multiqueue workstream ran seventeen mutants by hand, editing source
with sed against anchors grepped from the live tree, and none of the seventeen
is reproducible from the repository today -- `git log --all -S nohome` is empty.
This file carries the eight whose target is utils.cpp, ublk.cpp, tcmu.cpp or
nbd.cpp. The other nine are refused BY NAME rather than dropped silently; see
GATED below for the split and the reason for each half.

Unlike the protocol and EVENT_IDX mutants the companion owns, almost nothing
here is a guard a mock can trip. Six of the eight are PLACEMENT mutants: the
oracle is test::RecordingFile's set of vcpus, so a detection reads as a vcpu
count changing or a ran_on(caller) flipping -- not as a protocol error. The two
exceptions both live in the shared primitives rather than in a transport:
nomigrateguard kills by killing the process, and noenginecheck kills on a return
value. Read the ledger column accordingly.

What every mutation here does establish is the same narrow thing the companion
establishes, and it is worth having: that the assertion actually fires, rather
than passing for a reason that has nothing to do with the guard it is supposed
to cover. Every mutation is an anchored replacement that fails loudly unless the
anchor matches EXACTLY ONCE, so a mutation can never silently degrade into a
no-op. An anchor that matched nothing would leave an unmutated binary in place,
and the green that binary then reports is the worst possible outcome -- it books
a kill that never happened.

An anchor that embeds a C++ comment rots when that comment is reworded, which is
an edit nobody thinks of as touching this file. One of them did rot that way. So
run `check` after editing any file an anchor names: it fails loudly, but only
when someone runs it, and nothing else in the build notices.

A third outcome is possible and is worse than a survival: a mutation that does
not compile. It is neither killed nor survived, and it poisons the next round,
because the build leaves the previous binary in place and that binary still
prints PASSED. The build is `-Werror -Wall -Wno-error=pragmas`
(CMake/photon-helpers.cmake), so the three shapes to watch are a deleted
declaration a later check still reads (-Werror=unused-variable), a deleted
function whose only caller went with it (-Werror=unused-function), and an
`if (x) ;` left behind by emptying a body (-Wempty-body). noenginecheck is the
one mutant here that needs three coordinated deletions to stay compilable, and
its entry says why. None of the eight is void.

Summary table. "Ledger" is the round that actually ran the mutant; the verdicts
and the observed failure values are transcribed from SPEC-multiqueue.md 10.4 and
from the per-task reports it cites, and every anchor below was re-grepped from
the CURRENT tree rather than copied out of them.

    mutant          file    ledger            verdict
    --------------  ------  ----------------  --------------------------------
    nomigrate       ublk    10a #1            KILLED
    lateMigrate     ublk    10a #2            KILLED
    nohome          ublk    Task 4 + 10a #3   SURVIVED x2, adjudicated
    tcmunomigrate   tcmu    10a #4            KILLED
    tcmunohome      tcmu    Task 4 fix1 + 10a #8   KILLED
    nbdnomigrate    nbd     Task 5 Step 6(a)  KILLED
    nomigrateguard  utils   Task 3 Mutant 1   KILLED (process death)
    noenginecheck   utils   Task 3 Mutant 3 + 10b #9a/#9b   KILLED x3

blk/ublk.cpp -- the per-queue owner hop and the teardown hop back:

    nomigrate       # delete `migrate_to_pool(cfg.pool, th)` from start_serving's
                    # owner loop, so every queue's owner coroutine -- and the
                    # ring, pump and tag coroutines it creates -- stays on the
                    # caller's vcpu. Killed by UblkTest.pool_placement_n_less_than_m
                    # on `EXPECT_EQ(2u, rec.vcpu_count())` reading 1 and
                    # `EXPECT_FALSE(rec.ran_on(caller))` reading true. Two
                    # siblings go red for the same reason and are not booked
                    # separately: pool_placement_n_greater_than_m asserts the same
                    # two things, and two_devices_share_one_pool asserts that
                    # device B's vcpus are disjoint from device A's, which one
                    # shared caller vcpu violates. "Single-path" in the ledger is
                    # about CHANNELS, not case count: the placement assertion is
                    # the only channel, so nothing independently proves the mutant
                    # was in that binary. lateMigrate below is the same mutation
                    # with a second channel added, and that is why it is the
                    # stronger of the pair.

    lateMigrate     # move the same call to just after `oa.done.wait(1)`, i.e.
                    # after the owner has already run queue_setup and signalled.
                    # Killed by the same case, but on TWO paths, and the second
                    # one is what makes this the stronger of the pair: the owner
                    # is no longer READY, so photon::thread_migrate refuses it and
                    # migrate_to_pool logs one
                    #   failed to migrate a serving coroutine into the work pool
                    # per queue. That WARN cannot be emitted by anything else, and
                    # its count tracks cfg.queues, so it is direct evidence the
                    # mutation reached the binary. The ledger notes the line
                    # number it saw beside that message; it is deliberately not
                    # repeated here, because it drifts with utils.cpp.

    nohome          # delete the `thread_migrate(th, q->home)` in
                    # run_queue_teardown, so the teardown coroutine runs on the
                    # caller's vcpu instead of on the one that owns the queue's
                    # ring. EXPECTED NOT TO BE DETECTED, and recorded as a
                    # SURVIVAL rather than as a gap to paper over: run twice,
                    # green both times (Task 4 saw no effect across the full suite
                    # plus five repeats; 10a #3 saw GREEN x4 with no hang and no
                    # crash). Adjudicated as T10-17, which adopted the
                    # structural reading and permanently rejected adding a
                    # test-only observation point inside blk/ublk.cpp.
                    #
                    # The reason is structural, and it is worth stating plainly
                    # because the plan predicted a hang or a crash and that
                    # premise is false for photon: thread_interrupt has a
                    # first-class cross-vcpu path, and thread_join is
                    # vcpu-independent. What is left is that queue_teardown
                    # creates no coroutine and issues no backend IO -- it flips
                    # `stopping`, interrupts and joins the pump and the tags,
                    # calls iouring_abandon (whose entire body is `m_generation++`)
                    # and then deletes the ring, after those joins. The suite's
                    # only placement probe is test::RecordingFile, which records
                    # the vcpu of the coroutines that CALL THE BACKEND, and an
                    # interrupted tag resumes on the vcpu it already belonged to.
                    # So the recorded set is {q->home} under both shapes, and no
                    # placement assertion can discriminate.
                    #
                    # A third mechanism this entry used to carry -- that the hop
                    # protects `iouring_abandon(q->ce)` and `delete q->ce` from
                    # being run by a foreign OS thread -- was WITHDRAWN by the
                    # same adjudication and is not restated here as fact:
                    # abandon() only bumps a plain uint64_t, and the delete is
                    # sequenced after the pump join and every tag join.
                    #
                    # The real hazard is two hang windows that both need teardown
                    # genuinely concurrent with serving, reachable only through
                    # detach(false) or the destructor, and non-deterministic. A
                    # case built on them would be a flaky oracle, not a kill. The
                    # tcmu-style "teardown under load" shape is structurally
                    # unavailable here: stop_serving's flush spins on
                    # in_flight_all() BEFORE teardown, so continuous load
                    # livelocks it.
                    #
                    # Why leaving this one green is acceptable rather than
                    # negligent: the placement assertions in pool_placement_*
                    # are already in the tree, so if teardown ever does emit
                    # backend IO, nohome goes red with no new scaffolding; and a
                    # non-IO failure mode presents as a hang, which the timeout
                    # catches. Code-review-covered, booked as such.

blk/tcmu.cpp -- the pump hop and the serve_stop hop:

    tcmunomigrate   # delete `migrate_to_pool(pool, pump_th)` from serve_start, so
                    # the pump -- and therefore the ring it drains -- stays on the
                    # caller's vcpu. Killed by
                    # TcmuTest.pool_placement_pump_off_the_caller_vcpu, on three
                    # assertions drawn from one RecordingFile: ran_on(caller)
                    # reading true, and vcpu_count reading 1 against an expected 2
                    # both at the first placement check and at the one after the
                    # restart. The restart half is the interesting one: a device
                    # that never left the caller's vcpu cannot add a vcpu by being
                    # restarted. Single-path, like nomigrate.

    tcmunohome      # delete the `thread_migrate(th, home)` in run_serve_stop, so
                    # serve_stop's flush path runs drain_ring on the caller's vcpu
                    # instead of on the one that served the ring. Killed by
                    # TcmuTest.pool_serving_stop_under_load, and by nothing else --
                    # the plan named pool_placement_pump_off_the_caller_vcpu too
                    # and the ledger measured it GREEN under this mutant, because
                    # that case detaches after device_io has fully finished:
                    # in_flight is 0, the ring is drained, drain_ring finds
                    # parse_pos == head and dispatches nothing, so there is no
                    # backend IO left to record a vcpu. Failure shape:
                    # EXPECT_FALSE(rec.ran_on(caller)) reading true and
                    # EXPECT_EQ(1u, rec.vcpu_count()) reading 2, the set being
                    # {pool, caller}. The case's positive-count guards still pass
                    # under the mutant -- all five writers cross the stop and
                    # flushed >= 5 -- which is what proves the flush really did
                    # dispatch the backlog, on the wrong vcpu, rather than
                    # dispatching nothing.

blk/nbd.cpp -- the per-connection hop:

    nbdnomigrate    # delete `migrate_to_pool(cfg.pool, th)` from spawn_serve_conn,
                    # so every connection's serve_conn worker stays on the
                    # caller's vcpu. Killed by
                    # NbdTest.connections_spread_over_the_pool, on
                    # rec.vcpu_count() reading 1 against an expected 2 and
                    # rec.ran_on(caller) reading true -- the failure values the
                    # ledger recorded are byte-identical to the ones that case
                    # produced when it was first written red. The ledger recorded
                    # "only that one red, the other eight green because
                    # pool == nullptr makes the migration a no-op". That was true
                    # of the nine-case nbd suite it ran against and is NOT true of
                    # the tree now: the suite has since grown more pooled-connection
                    # placement cases, and any case that asserts the caller's vcpu
                    # is absent from the recorded set is a killer by construction.
                    # Expect more than one red and do not read the extra ones as a
                    # surprise. What has not changed is that the accept coroutine is
                    # never migrated, in either shape, which is what lets these
                    # cases assert the caller's absence at all.

blk/utils.cpp -- the two shared primitives every transport calls:

    nomigrateguard  # delete `pool->get_vcpu_num() == 0` from migrate_to_pool's
                    # short circuit, leaving `if (!pool || !th)`. Killed by
                    # blk_pool.empty_pool_does_not_divide_by_zero -- and killed in
                    # the shape a plain exit code cannot express, so do not read
                    # the rc alone. Observed: the process dies INSIDE that case,
                    # rc 139, and there is no `[==========] N tests ran` line
                    # anywhere in the output. That absence is the evidence; a
                    # summary line would mean the suite finished.
                    #
                    # The signal is worth being careful about, and the two
                    # recorded accounts of it disagree. The guard's own in-tree
                    # comment and the plan both say SIGFPE, and the source agrees
                    # with them: migrate_to_pool deliberately passes an
                    # out-of-range index (-1ULL, so WorkPool hands out its shared
                    # round-robin cursor), and get_vcpu_in_pool resolves one with
                    # `vcpu_index++ % size` BEFORE it subscripts vcpus, so with
                    # size == 0 the division is what fires. The ledger nonetheless
                    # MEASURED SIGSEGV, 139 not 136, and explained it as the
                    # empty-vcpus lookup faulting before the modulo is reached --
                    # an order the source contradicts. No mechanism is asserted
                    # here for that measurement. Accept either signal; what is
                    # required is the missing summary line and the death landing
                    # in exactly that case.
                    #
                    # Three sibling cases die the same way and are not booked.
                    # They are not "later cases" in this suite -- they are in
                    # three other binaries: ublk's, vduse's and vhost-user's
                    # empty_pool_falls_back_to_the_caller_vcpu each construct
                    # WorkPool(0) and hand it to a real start(), which reaches
                    # the same deleted term. blk_pool's is the only one that
                    # calls migrate_to_pool on a WorkPool(0) directly. The vduse
                    # sibling is not observable at all from this script, since
                    # running test-blk-vduse is what VDUSE_REASON refuses.
                    # null_thread_is_not_an_error is unaffected: it passes a
                    # null th, and `!th` still short-circuits.

    noenginecheck   # delete check_pool_engines' event-engine refusal, so a pool
                    # whose vcpus have no master event engine is accepted instead
                    # of refused with EINVAL. Killed at two levels. Primitive:
                    # blk_pool.engines_reject_a_pool_with_no_event_engine, on
                    # EXPECT_EQ(-1, check_pool_engines(&pool)) reading 0 and
                    # EXPECT_EQ(EINVAL, errno) reading 0 -- the ledger measured
                    # exactly that case red and nothing else in the suite.
                    # Wiring: the five transports' pool_without_an_event_engine_is_refused
                    # cases, on EXPECT_EQ(-1, dev->start(&rec)) reading 0. Those
                    # five are what make the mutant worth running per transport:
                    # every other case in every suite uses test::TestPool, which
                    # is built with the caller's own engine mask, so only these
                    # can see the guard at all. Two of the five live in suites
                    # whose production file this script never touches, and the
                    # vduse one is gated for the reason GATED gives -- mutating
                    # utils.cpp is safe, running test-blk-vduse is a separate
                    # decision.
                    #
                    # THREE deletions, not one, and the reason is -Werror. The
                    # ledger's first attempt deleted only the refusal and the
                    # build failed on `unused variable 'need_ev'`; the run that
                    # then printed 9 PASSED was a stale binary and was discarded,
                    # which is the standing rule -- never trust a test run after a
                    # failed build. Its second attempt deleted the declaration
                    # with the check. That is still not enough today: `b7d5d76`
                    # replaced the flag comparison with a name comparison and
                    # added show_engine, an anonymous-namespace helper whose only
                    # two callers are inside the refusal being deleted, so leaving
                    # it behind trips -Werror=unused-function. All three go
                    # together. What survives is the probe loop itself, which
                    # still creates the coroutine, still targets vcpus[i] by
                    # index and still waits -- only the verdict on the name it
                    # comes back with is gone. p.ev is then written and never
                    # read; that is a struct member, not a local, so no warning.
                    #
                    # Run this one under a timeout, and expect the transport-level
                    # cases to differ from each other. utils.h documents the
                    # per-transport measurement beside check_pool_engines' own
                    # declaration, and the two extremes matter here: ublk does
                    # not fail cleanly at all -- its per-queue pump cannot reap,
                    # so no request is ever fetched, the initiator's write times
                    # out, shutdown() returns -1 and the teardown after it never
                    # completes, because the teardown coroutine is starved by a
                    # pump that no longer yields. That is a HANG, not an
                    # assertion. nbd goes the other way: start() succeeds, the
                    # endpoint listens, and only the client handshake fails, with
                    # no diagnostic at all. The primitive-level case is the one
                    # with a deterministic shape.

Recorded by the ledger but deliberately NOT carried as a mutant here, so the
next reader does not think it was overlooked:

  - Task 3 Mutant 2 swapped check_pool_engines' targeted probe
    `thread_migrate(th, (size_t)i)` for a cursor draw `thread_migrate(th, -1ULL)`.
    Verdict NOT OBSERVABLE, green, and the reason is structural: in
    engines_reject_a_pool_with_no_event_engine every vcpu of the pool is equally
    bad, so any draw finds the same defect. Discriminating it would need a pool
    with one good vcpu and one bad, and inventing that case was out of scope for
    the round that measured it. It has no name in the ledger, and this script
    does not invent one.
  - Task 1's three photon-side mutants (get_event_engine / get_io_engine) were
    3/3 KILLED, but the cases that killed them are no longer in the tree, so
    they cannot be re-run and are not encoded.
  - handle_mem_table's per-queue `was_enabled` is booked UNOBSERVABLE rather
    than tested: it needs a frontend that changes the memory table while some
    queues are enabled and others are not, and neither the mock nor QEMU does
    that. It lives in vhost-user.cpp, so it belongs to the companion script in
    any case.

Usage, on the VM against ~/PhotonLibOS (a copy, not the repository):

    ./mutate-blk.py backup
    ./mutate-blk.py <mutant>
    ./mutate-blk.py restore
    ./mutate-blk.py check          # verify every anchor, write nothing

`backup` before the first mutant and `restore` after EVERY one -- a mutant left
in place silently poisons each later run. `check` is the re-anchoring discipline
made runnable: it applies each mutant to an in-memory copy through the same code
path a real mutation uses and reports whether each anchor matched exactly once,
so a tree that has drifted can be found without mutating anything.

PHOTON_BLK_ROOT overrides the tree root, which defaults to ~/PhotonLibOS. It
exists so `check` can be run against any checkout -- including the repository
itself -- without editing this file. Backups and the stamp always stay in $HOME
whichever root is in use.
"""
import hashlib
import json
import os
import pathlib
import sys

# NOT inside the rsync tree: the VM copy is refreshed with `rsync --delete`, which
# removes anything the repository does not have, so a sibling backup would vanish
# mid-experiment and leave `restore` with nothing to restore from.
ROOT = pathlib.Path(os.environ.get("PHOTON_BLK_ROOT")
                    or (pathlib.Path.home() / "PhotonLibOS"))

# `utils.cpp.orig` is deliberately the SAME path the companion script uses. Two
# different .orig files for one source is exactly the "which backup is current"
# ambiguity the stamp exists to remove, so the two scripts share the file and are
# made mutually exclusive instead -- see SIBLING_STAMP.
SRCS = {
    "utils": (ROOT / "blk/utils.cpp", pathlib.Path.home() / "utils.cpp.orig"),
    "ublk": (ROOT / "blk/ublk.cpp", pathlib.Path.home() / "ublk.cpp.orig"),
    "tcmu": (ROOT / "blk/tcmu.cpp", pathlib.Path.home() / "tcmu.cpp.orig"),
    "nbd": (ROOT / "blk/nbd.cpp", pathlib.Path.home() / "nbd.cpp.orig"),
}

# `restore` overwrites every source this script knows about from its .orig, so a
# .orig left behind by an earlier experiment reverts the tree to that
# experiment's shape -- silently, and across files the current mutation never
# touched. That has happened: a stale pair once reverted TWO files, one of them
# untouched by the mutation in flight, and the test run that followed reported a
# green belonging to a stale binary. The stamp makes the sequence self-checking:
# only `backup` may create it, a mutation and a `restore` both require it, and
# `restore` consumes it.
#
# Its existence alone is not enough, so it carries three things. ROOT closes
# PHOTON_BLK_ROOT: backups stay in $HOME whichever tree is in use, so a
# `restore` run against a second checkout would otherwise write the first
# tree's bytes into the second, consume the stamp, and report success while
# leaving the first one mutated. The DIGEST of each .orig closes a tree that
# moved between `backup` and `restore` -- an rsync or a checkout in that window
# makes the restore revert four files to an older revision, again with a success
# message for each. APPLIED closes stacking: two mutants with independent
# anchors in one source apply silently, and `restore` still recovers
# byte-for-byte, so what stacking loses is the experiment -- a red test no
# longer names one change. One mutant at a time is the documented discipline,
# and this is what enforces it.
STAMP = pathlib.Path.home() / "mutate-blk.inflight"

# blk/utils.cpp is in BOTH scripts' SRCS. A `backup` taken by one while the other
# is mid-mutation would capture the mutated bytes as pristine and then restore
# them, and nothing about the resulting tree would say so. The stamps are
# per-script because the experiments are, so each one also checks the other's.
SIBLING_STAMP = pathlib.Path.home() / "mutate-vhost-user.inflight"

VDUSE_REASON = (
    "targets blk/vduse.cpp, which is supervision-gated. Running test-blk-vduse "
    "while carrying a vduse mutant is the exact combination the ledger records as "
    "having wedged the machine into an unkillable D state: an orderly reboot hung "
    "because sync blocks behind the wedged block device, `vdpa dev del` blocked "
    "too, and only a user-authorized sysrq reset recovered it. Never touched "
    "unsupervised. Run it by hand with someone watching, after grepping the "
    "anchor from the live tree")

VHU_REASON = (
    "targets blk/vhost-user.cpp, which blk/test/mutate-vhost-user.py owns; its "
    "SRCS already backs that file up and restores it, and a second script "
    "writing the same source would make neither .orig trustworthy")

# The nine of the ledger's seventeen this script refuses rather than drops. Asking
# for one of these gets the reason, not a KeyError.
GATED = {
    "vdnomigrate": VDUSE_REASON,
    "vdstate0": VDUSE_REASON,
    "vdirq0": VDUSE_REASON,
    "vdrefresh0": VDUSE_REASON,
    "vhunomigrate": VHU_REASON,
    "qidx0": VHU_REASON,
    "mqalways": VHU_REASON,
    "qnumoff1": VHU_REASON,
    "noboundidx": VHU_REASON,
}

MUTANTS = {
    "nomigrate": "ublk",
    "lateMigrate": "ublk",
    "nohome": "ublk",
    "tcmunomigrate": "tcmu",
    "tcmunohome": "tcmu",
    "nbdnomigrate": "nbd",
    "nomigrateguard": "utils",
    "noenginecheck": "utils",
}

# The ledger's seventeen. `check` proves two things about the split above and
# only two: that the two halves are disjoint, and that together they number
# seventeen, so a name silently dropped from BOTH halves is caught. It does not
# compare the names against the ledger's -- that list lives in the plan document,
# and copying it here would turn a rename into a bookkeeping failure rather than
# leaving it a mutation one.
LEDGER_SEVENTEEN = frozenset(MUTANTS) | frozenset(GATED)
LEDGER_COUNT = 17

# ---- blk/ublk.cpp ----
# The owner hop: create, enable_join, migrate, wait, join. enable_join has to
# stay -- it writes a flag in the owner's own struct, and after the migration that
# thread is running on another OS thread, so writing it later would race the
# scheduler that owns it. Both mutants below share this anchor and differ only in
# what replaces it, which is why it carries three lines instead of one.
UBK_OWNER_HOP = """            photon::thread_enable_join(th);
            migrate_to_pool(cfg.pool, th);
            oa.done.wait(1);
"""

UBK_OWNER_NO_MIGRATE = """            photon::thread_enable_join(th);
            oa.done.wait(1);
"""

UBK_OWNER_LATE_MIGRATE = """            photon::thread_enable_join(th);
            oa.done.wait(1);
            migrate_to_pool(cfg.pool, th);
"""

# The whole braced block, not just the migrate call: emptying the body would leave
# `if (x) ;`, which trips -Wempty-body under -Werror, and a mutant that does not
# compile is void. q->home is still read by the guard at the top of
# run_queue_teardown, so deleting this does not orphan it.
UBK_TEARDOWN_HOP = """        if (photon::thread_migrate(th, q->home) < 0) {
            LOG_WARN("ublk: cannot move the teardown of queue ` back to its vcpu, ", q->qid, ERRNO());
        }
"""

# ---- blk/tcmu.cpp ----
# Only the call goes. The seven-line comment above it is shared with enable_join,
# which this mutant does not touch, so deleting the comment would take the
# rationale for a line that survives.
TCM_PUMP_HOP = """        photon::thread_enable_join(pump_th);
        migrate_to_pool(pool, pump_th);
"""

TCM_PUMP_NO_MIGRATE = """        photon::thread_enable_join(pump_th);
"""

# Brace-less `if` plus its LOG_WARN, deleted as one statement. `home` is a member
# and is still read by the DEFER that clears it and by the guard above, so no
# unused-variable hazard.
TCM_STOP_HOP = """        if (photon::thread_migrate(th, home) < 0)
            LOG_WARN("tcmu: cannot move the teardown back to the serving vcpu, ", ERRNO());
"""

# ---- blk/nbd.cpp ----
# The call plus the comment that exists only to justify it, plus the closing brace
# of spawn_serve_conn so the anchor is pinned to the end of that function. Unlike
# tcmu's, this comment says nothing about any surviving line -- it is entirely
# about why the migration is safe -- and the ledger's own run removed it with the
# call. Leaving it would describe code that is no longer there.
NBD_CONN_HOP = """        // The connection coroutine is the fan-out unit here: nbd has no queue count
        // to declare, its parallelism is however many clients connect. Safe to move
        // because nothing in the socket path caches a vcpu -- wait_for_fd_readable
        // resolves the engine from the CURRENT vcpu on every call, so the stream
        // this accept produced keeps working on the vcpu it lands on.
        migrate_to_pool(cfg.pool, th);
    }
"""

NBD_CONN_NO_MIGRATE = """    }
"""

# ---- blk/utils.cpp ----
# Deliberately excludes the guard's own comment. Embedding the comment made this anchor
# rot the first time that comment was reworded: f3f01d9 grew it from three lines to six,
# and `check` then reported CHECK_ANCHOR for a mutant whose target code was untouched.
# What this mutant changes is the CODE -- it drops the empty-pool term -- so the anchor
# names only code. The consequence is that the mutant leaves a comment explaining a check
# it just removed; that reads oddly and is inert.
UTILS_MIGRATE_GUARD = """    if (!pool || !th || pool->get_vcpu_num() == 0)
        return;"""

UTILS_MIGRATE_GUARD_NO_EMPTY = """    if (!pool || !th)
        return;"""

# The three pieces noenginecheck has to remove together; see its entry above for
# the -Werror chain that makes one or two of them a build failure.
UTILS_ENGINE_NEED = """    // Derived from the caller's own vcpu, and like the probe above it needs one:
    // master_event_engine is per-vcpu state, not a process-wide setting.
    const std::string_view need = photon::get_vcpu()->master_event_engine->get_engine_name();
"""

UTILS_ENGINE_SHOW = """// The empty name means no engine is installed at all, which would log as nothing.
std::string_view show_engine(std::string_view name) {
    return name.empty() ? std::string_view("<none>") : name;
}

"""

UTILS_ENGINE_REFUSAL = """        if (p.ev.empty() || p.ev != need)
            LOG_ERROR_RETURN(EINVAL, -1,
                "work pool vcpu ` cannot host blk serving coroutines: event engine `, need ` (a pool built with the default ev_engine has none, and every fd wait on it fails at once)",
                i, show_engine(p.ev), show_engine(need));
"""


# utils.cpp and tcmu.cpp both carry non-ASCII bytes, so the codec is pinned rather
# than left to the locale: read_text() would decode with whatever LANG happens to
# say on the box running the round, and a POSIX locale would fail the read outright.
# Going through bytes also skips newline translation on the way back out, so the
# only bytes a mutation changes are the ones the anchor covers -- which is what
# lets a round verify itself with a `git diff` (or an md5, as the ledger's runs
# did) instead of taking the script's word for it.
def read_src(path):
    return path.read_bytes().decode("utf-8")


def write_src(path, text):
    # temp + rename rather than truncate + write: an interrupt half-way through
    # would otherwise leave a truncated product source behind, with `restore` as
    # the only way back.
    tmp = path.with_name(path.name + ".mutating")
    tmp.write_bytes(text.encode("utf-8"))
    os.replace(tmp, path)


def die(msg):
    print("MUTATE-FAILED: " + msg, file=sys.stderr)
    sys.exit(1)


def sub_once(text, old, new, what):
    n = text.count(old)
    if n != 1:
        die("anchor for %s matched %d times, expected 1 -- re-grep it from the "
            "live tree, do not guess a nearby substitute" % (what, n))
    return text.replace(old, new)


def require_stamp(what):
    if not STAMP.exists():
        die("%s needs a backup taken in this experiment; run `backup` first "
            "(a .orig of unknown age would revert the tree silently)" % what)


def require_no_sibling(what):
    if SIBLING_STAMP.exists():
        die("%s refused: mutate-vhost-user.py is mid-experiment (%s exists) and "
            "both scripts back up blk/utils.cpp to the same .orig. That script "
            "does not check this stamp, so the shared .orig may already hold a "
            "mutated copy: restoring from it would install the mutation as "
            "pristine, and backing up now would capture it the same way. Finish "
            "that experiment first" % (what, SIBLING_STAMP))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stamp_read(what):
    # require_stamp has already proved the file exists. One that will not parse
    # is a stamp an older version wrote, or a truncated one, and guessing at it
    # is exactly what the digests below are here to prevent.
    try:
        st = json.loads(STAMP.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        die("%s refused: %s does not hold a stamp this version wrote (%s); "
            "recover the four sources from git and start the round again"
            % (what, STAMP, e))
    if not isinstance(st, dict) or st.get("root") != str(ROOT):
        die("%s refused: the experiment in flight backed up %s, but "
            "PHOTON_BLK_ROOT now selects %s. Restoring would write one tree's "
            "bytes into the other, consume the stamp, and leave the first tree "
            "still carrying its mutant"
            % (what, st.get("root") if isinstance(st, dict) else None, ROOT))
    return st


def stamp_write(st):
    STAMP.write_text(json.dumps(st, sort_keys=True), encoding="utf-8")


def mutate(mode, text):
    """Apply one mutant to `text` and return the result.

    Pure, and deliberately the only place a mutation is expressed: `check` runs
    this same function on a string it never writes back, so the self-check cannot
    drift away from the thing it is checking."""
    if mode == "nomigrate":
        return sub_once(text, UBK_OWNER_HOP, UBK_OWNER_NO_MIGRATE,
                        "ublk start_serving's migrate_to_pool")
    if mode == "lateMigrate":
        return sub_once(text, UBK_OWNER_HOP, UBK_OWNER_LATE_MIGRATE,
                        "ublk start_serving's migrate_to_pool")
    if mode == "nohome":
        return sub_once(text, UBK_TEARDOWN_HOP, "",
                        "ublk run_queue_teardown's thread_migrate back to q->home")
    if mode == "tcmunomigrate":
        return sub_once(text, TCM_PUMP_HOP, TCM_PUMP_NO_MIGRATE,
                        "tcmu serve_start's migrate_to_pool")
    if mode == "tcmunohome":
        return sub_once(text, TCM_STOP_HOP, "",
                        "tcmu run_serve_stop's thread_migrate back to home")
    if mode == "nbdnomigrate":
        return sub_once(text, NBD_CONN_HOP, NBD_CONN_NO_MIGRATE,
                        "nbd spawn_serve_conn's migrate_to_pool")
    if mode == "nomigrateguard":
        return sub_once(text, UTILS_MIGRATE_GUARD, UTILS_MIGRATE_GUARD_NO_EMPTY,
                        "migrate_to_pool's empty-pool short circuit")
    if mode == "noenginecheck":
        text = sub_once(text, UTILS_ENGINE_REFUSAL, "",
                        "check_pool_engines' event-engine refusal")
        text = sub_once(text, UTILS_ENGINE_NEED, "",
                        "the `need` declaration only that refusal read")
        return sub_once(text, UTILS_ENGINE_SHOW, "",
                        "show_engine, whose only callers were in that refusal")
    die("no mutation body for %r, though MUTANTS lists it" % mode)


def check():
    """Re-grep every anchor against the live tree and write nothing.

    This is the discipline the ledger's hand-sed runs did not have: an anchor that
    has drifted is found here, before a round is spent building a binary that was
    never mutated."""
    both = frozenset(MUTANTS) & frozenset(GATED)
    if both:
        die("%s appear in both MUTANTS and GATED, so the split is not a split"
            % sorted(both))
    if len(LEDGER_SEVENTEEN) != LEDGER_COUNT:
        die("MUTANTS and GATED name %d mutants between them, not the ledger's %d: "
            "one was dropped from both halves, or the ledger has moved and "
            "LEDGER_COUNT with it" % (len(LEDGER_SEVENTEEN), LEDGER_COUNT))
    for src in MUTANTS.values():
        if src not in SRCS:
            die("mutant targets %r, which SRCS does not know" % src)
    bad = 0
    for mode in sorted(MUTANTS):
        src, _ = SRCS[MUTANTS[mode]]
        if not src.exists():
            print("CHECK_MISSING %s %s" % (mode, src))
            bad += 1
            continue
        text = read_src(src)
        try:
            mutated = mutate(mode, text)
        except SystemExit:
            print("CHECK_ANCHOR %s %s" % (mode, src))
            bad += 1
            continue
        if mutated == text:
            print("CHECK_NOOP %s %s" % (mode, src))
            bad += 1
            continue
        if mutated.count("{") - mutated.count("}") != text.count("{") - text.count("}"):
            print("CHECK_BRACES %s %s" % (mode, src))
            bad += 1
            continue
        print("CHECK_OK %s %s" % (mode, src))
    if bad:
        die("%d of %d mutants failed the anchor check" % (bad, len(MUTANTS)))
    print("CHECK_ALL_OK %d mutants, %d gated, nothing written"
          % (len(MUTANTS), len(GATED)))


def main():
    if len(sys.argv) != 2:
        die("usage: mutate-blk.py {backup|restore|check|%s}" % "|".join(sorted(MUTANTS)))
    mode = sys.argv[1]

    if mode == "check":
        check()
        return

    if mode == "backup":
        require_no_sibling("backup")
        # An in-flight stamp means a mutant may still be in one of these sources.
        # Backing up then would capture the mutation as pristine, and the restore
        # that follows would faithfully put it back -- silently, and with a stamp
        # saying the tree is clean. Restore first.
        if STAMP.exists():
            die("backup refused: this script is already mid-experiment (%s exists), "
                "so a source may still carry a mutant; run `restore` first" % STAMP)
        # Validate every path before writing any .orig: a half-written backup set
        # is exactly the unknown-age state the stamp exists to make impossible.
        for src, _ in SRCS.values():
            if not src.exists():
                die("no source at %s (set PHOTON_BLK_ROOT if this is not the tree "
                    "you meant)" % src)
        for src, bak in SRCS.values():
            bak.write_bytes(src.read_bytes())
            print("BACKUP_OK", bak)
        # Written last, so an interrupted backup leaves no stamp and every later
        # operation refuses rather than restoring from a partial set.
        stamp_write({"root": str(ROOT),
                     "digest": {n: digest(b) for n, (_, b) in SRCS.items()},
                     "applied": []})
        return
    if mode == "restore":
        require_stamp("restore")
        # Guarded for a different reason than `backup` is, and it is the reason
        # this call exists: the sibling's own backup is not guarded against THIS
        # stamp, so it can have overwritten the shared utils.cpp.orig with a
        # mutated copy. Restoring from that installs the mutation as pristine
        # and then reports success.
        require_no_sibling("restore")
        st = stamp_read("restore")
        recorded = st.get("digest") or {}
        # Validate the whole set before overwriting anything. Checking inside the
        # write loop would die after some sources were already reverted, leaving
        # a tree part-restored and part-mutated, with a RESTORE_OK printed for
        # each of the former. The stamp survives a death here, so a re-run is
        # the recovery and it re-validates from the start.
        for name, (src, bak) in SRCS.items():
            if not bak.exists():
                die("no backup at %s for %s; this script cannot restore the tree "
                    "-- recover %s from git" % (bak, name, src))
            if digest(bak) != recorded.get(name):
                die("the backup at %s is not the one this experiment took, so it "
                    "belongs to another tree or another round; refusing to apply "
                    "it. Recover %s from git" % (bak, src))
        for name, (src, bak) in SRCS.items():
            src.write_bytes(bak.read_bytes())
            print("RESTORE_OK", src)
        STAMP.unlink()
        return

    if mode in GATED:
        die("%s refused: %s" % (mode, GATED[mode]))
    if mode not in MUTANTS:
        die("unknown mutation %r; the ledger's seventeen are %s"
            % (mode, "|".join(sorted(LEDGER_SEVENTEEN))))
    require_stamp(mode)
    require_no_sibling(mode)
    st = stamp_read(mode)
    if st.get("applied"):
        die("%s refused: %s is already applied and this script mutates one at a "
            "time. Two mutants with independent anchors in one source apply "
            "silently, and the test that then goes red names neither. Run "
            "`restore` first" % (mode, st["applied"][0]))
    src = SRCS[MUTANTS[mode]][0]
    mutated = mutate(mode, read_src(src))
    write_src(src, mutated)
    # Recorded after the write, so a mutant that died on its anchor is not booked
    # as applied: a stamp claiming a mutation that never landed would only block
    # the next one.
    st["applied"] = [mode]
    stamp_write(st)
    print("MUTATED", mode, src)


if __name__ == "__main__":
    main()
