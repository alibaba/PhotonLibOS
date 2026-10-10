#!/usr/bin/env python3
"""Mutation harness for blk/vhost-user.cpp and blk/utils.cpp.

Three groups, each with a different relationship to the real frontend, so do not read
a detection claimed here as a QEMU detection unless it says so.

The five protocol mutants guard transport-level behaviours. Do not book this group
as "found by QEMU": that is substantiated for nocallsignal only -- it is a defect
the real frontend catches at its qvirtio_wait_queue_isr and a polling mock
structurally cannot. overread and nobound go the other way, and have long been
caught in-repo by pipelined_messages and oversized_payload. snapall is in-repo only,
killed by adopt_resumes_from_the_frontends_base_not_used_idx; nothing establishes a
real-frontend detection for it, and it does not claim one.

The eight EVENT_IDX mutants are a different case, and each is annotated where it is
listed below: several of them have no real-frontend detection at all, so do not read
this file as claiming one.

The fifteen INDIRECT_DESC mutants split across both files -- eleven on the table walk
in the shared engine, four on the one transport that offers the bit. nooffer is the
only mutant here with a real-frontend detection that is certain rather than argued,
because QEMU's indirect case asserts bit 28 before it sets features. Several of the
others have teeth at the engine layer only and say so where they are listed.

What every mutant here does
establish is narrower and still worth having -- that the new mock-side assertion
actually fires, rather than passing for a reason
that has nothing to do with the guard it is supposed to cover. Every mutation is
an anchored replacement that fails loudly if the anchor is not found exactly once,
so a mutation can never silently degrade into a no-op and report a false "the test
still passes".

Two source files, because the guards live at two levels: the protocol guards and the
INDIRECT_DESC offer are in the transport (blk/vhost-user.cpp), and the
VIRTIO_RING_F_EVENT_IDX and table-walk guards are in the shared virtqueue engine
(blk/utils.cpp), which both transports sit on.
`backup` and `restore` cover both; each mutant declares which file it acts on.
`backup` before the first mutant and `restore` after every single one -- a
mutant left in place silently poisons every later run.

    ./mutate-vhost-user.py backup
    ./mutate-vhost-user.py <mutant>
    ./mutate-vhost-user.py restore

blk/vhost-user.cpp -- protocol layer:

    overread        # revert recv_msg to one recvmsg of sizeof(vhost_user_msg).
                    # CURRENTLY VOID, and not because of a stale anchor: the anchor
                    # matches exactly once and the mutation applies, but the result
                    # does not compile. recv_msg gained a SECOND bound after this
                    # replacement text was written -- `payload_min(req)` -- and the
                    # pre-fix body carries only the older sizeof bound, so applying
                    # it deletes payload_min's only caller and
                    # -Werror=unused-function fails the build. Repairing it is a
                    # design choice rather than a resync, because a faithful pre-fix
                    # body also drops the DEFER that closes half-received fds and
                    # that second bound, so it would conflate three mutants into one
                    # run. Left broken and loud rather than quietly narrowed to
                    # something that compiles and proves less than its name claims.
    nobound         # drop the oversized-payload check
    noprotofeature  # stop offering device feature bit 30, the gate on protocol
                    # negotiation
    nocallsignal    # stop signalling the callfd when SET_VRING_CALL installs it

blk/utils.cpp -- VIRTIO_RING_F_EVENT_IDX, one annotation per mutant below:

    alwaynotify     # should_notify always says notify, so the §2.7.7.2 SHOULD
                    # NOT half is gone. Caught by interrupt_suppressed_by_used_event's
                    # first assertion, EXPECT_FALSE(fe.callfd_readable(50)) -- and that
                    # is the only DETERMINISTIC catch anywhere. Do not book the QEMU idx
                    # case as a second one: its no_isr helper asserts ISR absence only
                    # while the status byte still reads 0xff, and we write the status
                    # byte before we signal, so the spurious interrupt lands at the
                    # instant the helper stops looking. A miss then stays pending and
                    # satisfies the later wait_used_elem, masking itself; what is left
                    # is a coin flip between two catch points.
    nevenotify      # should_notify never says notify, so the §2.7.7.2 MUST half and
                    # the flags fallback are both gone. Caught by two: that case's
                    # second assertion ASSERT_TRUE(fe.callfd_readable(1000)), and
                    # first_completion_always_notifies. Only two and not all 25 --
                    # collect() checks the used ring BEFORE it looks at the callfd,
                    # so a backend that never raises an interrupt still completes
                    # every request: "never notified" degrades into "slower", not
                    # "failed". (KICK_FALLBACK_US is not the reason here; that
                    # timeout is the kick direction, loop()'s wait on kickfd, while
                    # should_notify gates the callfd, which is the interrupt
                    # direction.) That polling tolerance is the blind spot, not
                    # coverage. A real frontend has no such tolerance, so this one has
                    # DETERMINISTIC detectors outside the repo as well -- two cases,
                    # and between them both notification paths:
                    #   idx     fails deterministically at its first wait_used_elem,
                    #           because the qvirtio_wait_queue_isr just before it has
                    #           already drained the SET_VRING_CALL one-shot, so no
                    #           stale interrupt can satisfy the wait.
                    #   basic   necessarily fails, but at its first OR second
                    #           wait_used_elem, and which one is timing: basic has no
                    #           qvirtio_wait_queue_isr (that call occurs exactly once
                    #           in the whole file, inside idx), so the one-shot is
                    #           still pending as a single read-and-clear legacy ISR
                    #           bit, and wait_used_elem's "isr && get_buf" test
                    #           consumes it on whichever poll first sees it. If the
                    #           first completion has landed by then the wait returns
                    #           and the failure moves to the second; if not, the bit
                    #           is consumed and lost and the first spins out.
                    # The credit is not "a named line must time out" -- it is that
                    # under nevenotify no second interrupt can ever exist, so one of
                    # basic's two waits must. That makes this the BROADEST
                    # real-frontend coverage any mutant here has (two cases, both
                    # paths); wrongevent below has the finest (one slot offset).
    nofirstnotify   # drop the unconditional first decision on a ring, the handover
                    # guard for a resumed ring whose used_event was left behind by a
                    # previous daemon. Caught by first_completion_always_notifies,
                    # which plants used->idx 10 against used_event 100 so the
                    # equality rule alone would not fire. The QEMU suite does NOT
                    # catch this one and cannot: with used_event 0 the rule fires on
                    # its own at the first completion, and the cases that never
                    # negotiate bit 29 do not reach the clause at all.
    noavailevent    # stop re-publishing avail_event per consumed head, so §2.7.10.1's
                    # equality never tracks. Caught by avail_event_published and
                    # event_idx_wrap, both of which read the uint16 directly. The 21
                    # IO cases stay green: avail_event pins at 0, kick_if_needed()
                    # then only kicks at idx == 0, and OUR OWN loop() carries the
                    # rest -- it waits on kickfd with Timeout(KICK_FALLBACK_US) and
                    # then dispatches unconditionally, so it re-polls every 5 ms
                    # whichever frontend is attached. That means no real frontend
                    # detects this by timing out either; the two direct assertions
                    # are the only detection anywhere. (The wraparound question is
                    # now closed, and closed as "a real frontend cannot answer it":
                    # no case in the QEMU suite is wraparound-scale -- idx issues
                    # three requests against a 128-entry ring -- so event_idx_wrap
                    # below is the only wraparound coverage that exists anywhere.)
    wrongneed       # off-by-one in the event predicate: (new_idx - event_idx) instead
                    # of (new_idx - event_idx - 1). Caught ONLY by
                    # interrupt_suppressed_by_used_event's second assertion; its first
                    # half reads the same value either way (65437 < 1 and 65438 < 1 are
                    # both false), and event_idx_wrap cannot see it at all because
                    # collect() polls the used ring. §4.2 works the arithmetic.
                    # The QEMU suite does not catch it deterministically either, and
                    # the reason is worth knowing because it looks like it should:
                    # under idx it OVER-notifies at completion #2 and UNDER-notifies
                    # at #3, and the ISR left pending by #2 satisfies #3's
                    # wait_used_elem precondition, so the two failures mask each other.
    wrongevent      # read used_event from ring[num - 1] instead of ring[num]. Same
                    # in-repo attribution as wrongneed, for the same reason: the first
                    # half reads a memset-0 slot, which still suppresses, so it passes.
                    # Unlike wrongneed, the QEMU idx case DOES catch this one
                    # deterministically, at its second wait_used_elem: ring[num - 1] is
                    # never written so used_event reads 0, completion #3 suppresses when
                    # it must notify, and because #2 did not over-notify there is no
                    # stale ISR to mask it -- the wait spins its full 30 s. That makes
                    # this the FINEST real-frontend credit here: a one-slot offset that
                    # still answers correctly at completion #2, invisible to every
                    # polling oracle and to the mock's first assertion, caught only at
                    # one wait. nevenotify above is broader; this one is sharper.
    nofence         # drop the drain loop's store-load barrier. EXPECTED NOT TO BE
                    # DETECTED, and not by a real frontend either -- do not book a
                    # QEMU run as covering it. Two reasons, and the first is the
                    # one that is easy to get backwards: the masking is OURS, not
                    # the peer's. loop() waits on kickfd with
                    # Timeout(KICK_FALLBACK_US) and then calls dispatch_avail()
                    # unconditionally, so we re-poll every 5 ms whichever frontend
                    # is attached; a peer that never polls would not expose this
                    # any better than one that does. Second, the defect is only
                    # PROBABILISTIC even inside that window -- it needs the
                    # driver's read of avail_event and ours of avail->idx to be
                    # reordered by the store buffer. It rests on §3.3's ordering
                    # argument alone, with no test anywhere having teeth on it.
                    # Recorded as a non-detection, honestly.
    noprogress      # drop the drain loop's no-progress break. Caught by the
                    # pre-existing dispatch_cap_recovery, which publishes VQ_NUM + 8
                    # avail entries before one kick and so reaches the in-flight cap
                    # on every green run. Its detection shape is NOT one assertion
                    # taking two values: without the break the serving vcpu livelocks
                    # in a loop that never yields, so the mock's timeout returns to a
                    # semaphore nobody consumes, no assertion is ever recorded, and
                    # the process never exits. Run it alone under `sudo timeout` and
                    # read the exit code -- guarded is 0 with a PASSED line, mutated
                    # is 124 with the output stopping at [ RUN ]. The QEMU suite does
                    # not catch it either: idx issues three requests against a
                    # 128-entry ring and so never reaches the in-flight cap.

blk/utils.cpp -- VIRTIO_RING_F_INDIRECT_DESC, the table walk:

    nogate          # drop the refusal when the transport did not negotiate bit 28,
                    # leaving the walk itself in place. Caught by
                    # an_indirect_descriptor_is_refused_when_the_feature_was_not_negotiated.
                    # Deleting the block rather than emptying its body is deliberate:
                    # `if (x) ;` trips -Wempty-body under -Werror, the build fails, and a
                    # mutant that does not compile is void -- while a run that is red
                    # because of a compile error looks exactly like a real detection.
    noxtnext        # drop the refusal of a descriptor carrying INDIRECT and NEXT
                    # together, so whatever the driver chained behind the table is
                    # silently discarded. Caught by
                    # an_indirect_descriptor_chained_with_next_is_refused, on its status
                    # assertion: the wrong shape returns S_OK. That case's sentinel
                    # assertion does NOT discriminate between the two shapes, so it is
                    # not the detection and is not booked as one.
    partialent      # keep only the zero-length half of the table length check, so a
                    # length that is not a whole number of entries gets walked. Caught by
                    # a_table_length_that_is_not_a_whole_number_of_entries_is_refused.
                    # Split from zerolen because the two halves guard different things:
                    # this one reads outside the buffer the driver declared, that one
                    # treats an empty table as a legitimate chain.
    zerolen         # keep only the divisibility half, so a zero-length table gets
                    # walked. Caught by
                    # a_zero_length_table_is_refused_before_any_entry_is_translated, on
                    # its ASSERT_EQ(0u, g.asked_addr.size()) -- not on the status, which
                    # is IOERR under both shapes.
    nocab           # drop the MAX_INDIRECT_ENTRIES cap on a table's entry count. Caught
                    # by an_absurd_table_is_refused_before_it_is_walked, on its
                    # ASSERT_EQ(0u, g.asked_addr.size()): 0 against 66. The status is the
                    # SAME under both shapes, so an IOERR assertion here is not a
                    # detection. Nor does the vhost-user case that refuses an absurd
                    # table count as a second one: on that transport a translate is a
                    # handful of integer comparisons, so even a quarter of a million of
                    # them finishes in milliseconds and the mock's collection budget
                    # never expires. One detection, not two.
    capoffbyone     # >= instead of > on that cap, refusing a table of exactly
                    # MAX_INDIRECT_ENTRIES. Caught by
                    # a_table_of_exactly_the_entry_cap_is_served. This is the half of the
                    # cap arithmetic the published seg_max does not cover: 62 and 64 are
                    # the two ends of one equation and this moves one end. The real
                    # frontend cannot catch it -- QEMU's indirect case builds a table of
                    # exactly two entries.
    tblwrite        # translate the table itself as writable instead of read-only. Caught
                    # by the_table_is_translated_read_only_and_first, on its
                    # EXPECT_EQ(0, g.asked_writable[0]). Teeth at the engine layer only:
                    # vhost-user's translate does not consult the writable argument, so
                    # no transport-level case can see it, and on vduse the walk is
                    # unreachable because that transport does not offer the bit.
    tblasdata       # push the table descriptor's own bytes into a data stream as well as
                    # walking it. Caught by
                    # the_table_descriptor_itself_carries_no_data, on its S_OK assertion:
                    # the stray table bytes make the role split reject the request. The
                    # indirect write case goes red too but by a different mechanism -- it
                    # reads the table's first entry as a request header -- so only the one
                    # case is booked.
    tblbound        # bound a table entry's index by ring_num instead of by the table's
                    # own entry count, conflating two index spaces. Caught by
                    # an_out_of_range_table_next_is_refused_instead_of_read_past_the_table,
                    # on its sentinel assertion -- not on the status, which is IOERR
                    # either way because the wrong bound still lands somewhere invalid.
                    # That is what the 513-byte decoy in that case is for.
    tblarray        # read table entries out of the main ring's descriptor array instead
                    # of out of the table. Caught by
                    # normal_descriptors_before_a_trailing_indirect_table_are_all_served,
                    # and the attribution matters: it goes red because at entry 1 it
                    # re-reads the ring descriptor carrying INDIRECT and trips the
                    # nested-table refusal, not because a header comparison fails.
    nestok          # drop the refusal of a nested indirect descriptor, so one table
                    # entry may name another table. Caught by
                    # a_nested_indirect_table_is_refused, and by nothing else. The real
                    # frontend cannot catch it: QEMU's indirect helper hard-codes a table
                    # of two entries and never builds a nested one.

blk/vhost-user.cpp -- the INDIRECT_DESC offer and the seg_max it publishes:

    nooffer         # stop offering device feature bit 28. Caught by
                    # indirect_desc_is_offered_and_negotiated on its offer precondition.
                    # This is the only mutant in the file whose real-frontend detection
                    # is certain rather than argued: QEMU's indirect case asserts bit 28
                    # is present before it calls qvirtio_set_features, so that case stays
                    # not ok.
    gateonoffer     # gate the table walk on what we offered instead of on what the peer
                    # negotiated. Caught by
                    # an_indirect_request_is_refused_when_the_feature_was_declined, which
                    # exists precisely because this mock CAN decline a bit. The sibling
                    # event-idx guard has no equivalent mutant, and that is a gap in this
                    # file's coverage rather than a judgement that the guard is unneeded:
                    # that mock cannot express a decline, so there is nothing to mutate
                    # against.
    noreset         # drop the indirect_desc clear in vq_reset. DETECTED, by
                    # a_session_that_never_sends_set_features_does_not_inherit_the_last_one_s_indirect_desc.
                    # Measured: 78 tests run, 77 pass, that one fails on three
                    # assertions at once -- the status byte reads 0 where the sentinel
                    # 0xff was expected, the used length reads 4097 where 0 was
                    # expected, and the offered read buffer comes back filled instead
                    # of all-zero. Restoring the line returns the suite to 78/78.
                    #
                    # This entry previously read EXPECTED NOT TO BE DETECTED. That was
                    # right about the case it named and wrong to leave there. The
                    # structural half of the old argument still holds, and one detail of
                    # it was wrong: vq_reset runs only from stop_session and rollback
                    # -- NOT from the destructor, which reaches it through shutdown()
                    # then detach() -- and a frontend RECONNECT tears down with vq_stop,
                    # not vq_reset, so
                    # a_reconnect_that_declines_the_feature_stops_walking_tables never
                    # reaches the deleted line. That case's guarantee comes from
                    # SET_FEATURES re-deriving the flag, not from this clear.
                    # What was actually missing was the mock path the old entry named,
                    # and negotiate() now has one: skip_set_features omits that single
                    # message and leaves the rest of the sequence intact, so a session
                    # can bring a ring up on SET_VRING_ENABLE alone and be served with no
                    # feature word ever settled.
                    #
                    # Why that is a real defect and not a curiosity: per-queue state
                    # crosses a detach()/start() boundary because the Vq objects do --
                    # built in the constructor, freed only in the destructor, so that a
                    # device stays able to start again -- and nothing on the way to a
                    # dispatch asks whether SET_FEATURES ever arrived.
                    #
                    # Only test-blk-vhost-user was run under this mutant, and that is the
                    # only suite that can see it: test-blk-vq links vhost-user.cpp through
                    # libphoton like every suite here does, but it drives the shared engine
                    # directly and never constructs a VhostUserController, so it cannot
                    # reach this line either way. The 52/52 the earlier version of this
                    # entry recorded alongside the 77/77 was a green that proved nothing.
                    #
                    # The clears beside it are not all alike, which is worth keeping in
                    # view now that this one has a witness: event_idx is redundant in the
                    # same way -- SET_FEATURES re-derives it every session, and its own
                    # comment says so -- while notify_valid is NOT re-derived by
                    # SET_FEATURES and therefore depends on this path with nothing behind
                    # it. The flag this mutant restores gates a peer-supplied length, so
                    # the clear is not decorative even now that a test covers it.
    segmaxfull      # publish MAX_INDIRECT_ENTRIES as seg_max instead of that count minus
                    # the two framing descriptors. Caught by
                    # seg_max_is_published_as_the_cap_minus_the_two_framing_descriptors,
                    # which reads 64 where it expects 62. State the strength of that tooth
                    # plainly: it is literal against literal, so it kills any other
                    # number, but nothing in this repository can prove 62 is the RIGHT
                    # number. Deleting the assignment outright has the same effect as not
                    # offering SEG_MAX and goes red on the same assertion reading 0, so no
                    # second mutant is needed.
    snapall         # discard ANY stale-cursor BASE, not only one the ring cannot vouch
                    # for. The guard's real test is `in_flight > q->srv.num`; this makes
                    # it `in_flight > 0`, so every BASE with anything outstanding is
                    # thrown away and the queue resumes from used_idx -- the "restored
                    # from used_idx and replayed" regime. Killed by
                    # adopt_resumes_from_the_frontends_base_not_used_idx on BOTH oracles:
                    # the settled used->idx reads 8 where it expects 6, and the two
                    # re-armed sentinel status bytes read 0 where they expect 0xff. That
                    # case owns this mutant alone -- no other caller of
                    # restart_with_base constructs 0 < last_avail - used_idx <= num.
                    #
                    # NOT the same as ignoring SET_VRING_BASE, and the difference matters:
                    # deleting the handler's last_avail store is indistinguishable in that
                    # case BY CONSTRUCTION, because the BASE it plants equals the cursor
                    # the ring naturally had after the requests it drove, and any coherent
                    # crash fiction forces that equality. It should die in event_idx_wrap
                    # instead (BASE 65534 against a natural 0). Reasoned, NOT measured --
                    # measure it before booking it as killed.

Run on the VM against ~/PhotonLibOS (a copy, not the repository). The sequence is
`backup`, then one mutation at a time with a build and a run between it and the next
`backup`, then `restore`; `restore` and every mutation refuse to run without the
stamp `backup` leaves, and `backup` refuses to run while that stamp still exists --
because `restore` copies .orig over the source, an .orig of unknown age reverts the
tree silently, and a `backup` taken mid-mutation makes the mutant itself the .orig.

    ./mutate-vhost-user.py check     # verify every anchor, write nothing

`check` is the re-anchoring discipline made runnable: it applies each mutant to an
in-memory copy through the same `mutate()` a real mutation uses and reports whether
each anchor matched exactly once, so a tree that has drifted is found before a round
is spent building a binary that was never mutated. It needs no stamp and writes
nothing. Run it after editing any file an anchor names: the descriptor-spelling change
in `4423d05` rotted five anchors and took the seven mutants referencing them out of
service, and four anchors embed C++ comments, so rewording one of those rots it just
as dead. `sub_once` reports the damage only at mutation time, after `backup` has
stamped the tree.

PHOTON_VHU_ROOT overrides the tree root, which defaults to ~/PhotonLibOS. It exists
so `check` can be run against any checkout, including the repository itself, without
editing this file. The .orig backups and the stamp always stay in $HOME whichever
root is in use, for the rsync reason below.
"""
import os
import pathlib
import sys

# NOT inside the rsync tree: the VM copy is refreshed with `rsync --delete`, which
# removes anything the repository does not have, so a sibling backup would vanish
# mid-experiment and leave `restore` with nothing to restore from.
ROOT = pathlib.Path(os.environ.get("PHOTON_VHU_ROOT")
                    or (pathlib.Path.home() / "PhotonLibOS"))

SRCS = {
    "vhost-user": (ROOT / "blk/vhost-user.cpp",
                   pathlib.Path.home() / "vhost-user.cpp.orig"),
    "utils": (ROOT / "blk/utils.cpp",
              pathlib.Path.home() / "utils.cpp.orig"),
}

# `restore` overwrites BOTH sources from their .orig files, so a .orig left behind by
# an earlier experiment reverts the tree to that experiment's shape -- silently, and
# across every file this script knows about rather than just the one mutated. A stale
# pair once turned the sources back three weeks; the build then failed and the previous
# binary still printed PASSED, so the damage read as a green run. The stamp makes the
# sequence self-checking: only `backup` may create it, a mutation and a `restore` both
# require it, and `restore` consumes it, so a `.orig` of unknown age can never be
# applied to a tree that is not mid-experiment.
STAMP = pathlib.Path.home() / "mutate-vhost-user.inflight"


def require_stamp(what):
    if not STAMP.exists():
        die("%s needs a backup taken in this experiment; run `backup` first "
            "(a .orig of unknown age would revert the tree silently)" % what)

MUTANTS = {
    "overread": "vhost-user",
    "nobound": "vhost-user",
    "noprotofeature": "vhost-user",
    "nocallsignal": "vhost-user",
    "alwaynotify": "utils",
    "nevenotify": "utils",
    "nofirstnotify": "utils",
    "noavailevent": "utils",
    "wrongneed": "utils",
    "wrongevent": "utils",
    "nofence": "utils",
    "noprogress": "utils",
    # VIRTIO_RING_F_INDIRECT_DESC: eleven in the shared engine, four in the one
    # transport that offers the bit.
    "nogate": "utils",
    "noxtnext": "utils",
    "partialent": "utils",
    "zerolen": "utils",
    "nocab": "utils",
    "capoffbyone": "utils",
    "tblwrite": "utils",
    "tblasdata": "utils",
    "tblbound": "utils",
    "tblarray": "utils",
    "nestok": "utils",
    "nooffer": "vhost-user",
    "gateonoffer": "vhost-user",
    "noreset": "vhost-user",
    "segmaxfull": "vhost-user",
    "snapall": "vhost-user",
}

RECV_MSG_MARK = "    int recv_msg(int fd, vhost_user_msg* m, int* fds, int* nfds) {\n"

# the pre-fix version, verbatim: one recvmsg asking for the whole struct
OLD_RECV_MSG = """    int recv_msg(int fd, vhost_user_msg* m, int* fds, int* nfds) {
        *nfds = 0;
        for (;;) {
            if (photon::wait_for_fd_readable(fd) < 0) {
                if (stopping) return -1;
                if (errno == EINTR) continue;
                LOG_ERRNO_RETURN(0, -1, "vhost-user recv wait failed");
            }
            iovec iov{m, sizeof(*m)};
            char cbuf[CMSG_SPACE(sizeof(int) * VHU_MSG_MAX_FDS)];
            msghdr mh;
            memset(&mh, 0, sizeof(mh));
            memset(cbuf, 0, sizeof(cbuf));
            mh.msg_iov = &iov;
            mh.msg_iovlen = 1;
            mh.msg_control = cbuf;
            mh.msg_controllen = sizeof(cbuf);
            ssize_t r = ::recvmsg(fd, &mh, MSG_DONTWAIT);
            if (r < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                LOG_ERRNO_RETURN(0, -1, "vhost-user recvmsg failed");
            }
            if (r == 0)
                return -1;   // the frontend closed
            if ((size_t)r < offsetof(vhost_user_msg, payload) + m->size)
                LOG_ERROR_RETURN(EPROTO, -1, "vhost-user short message: ` bytes", r);
            for (cmsghdr* cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
                if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                    int n = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                    memcpy(fds + *nfds, CMSG_DATA(cm), (size_t)n * sizeof(int));
                    *nfds += n;
                }
            }
            return 0;
        }
    }
"""

# The check alone, not the two locals declared above it: `req` and `sz` are read
# again by the payload_min bound a few lines further down, so deleting them along
# with the check would fail the build -- and a mutant that does not compile is
# void, not a detection.
BOUND_BLOCK = """        if (sz > sizeof(m->payload))
            LOG_ERROR_RETURN(EPROTO, -1, "vhost-user request ` declares a ` byte payload, the largest this protocol has is `", req, sz, (uint32_t)sizeof(m->payload));
"""


OFFER_FEATURES = """        offer_features = (1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_BLK_F_BLK_SIZE) |
                         (1ULL << VIRTIO_BLK_F_SEG_MAX) |
                         (1ULL << VIRTIO_RING_F_EVENT_IDX) | (1ULL << VIRTIO_RING_F_INDIRECT_DESC) |
                         (1ULL << VHOST_USER_F_PROTOCOL_FEATURES);   // we always answer
                                                                     // GET_PROTOCOL_FEATURES
"""

OFFER_FEATURES_NO_BIT30 = """        offer_features = (1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_BLK_F_BLK_SIZE) |
                         (1ULL << VIRTIO_BLK_F_SEG_MAX) |
                         (1ULL << VIRTIO_RING_F_EVENT_IDX) | (1ULL << VIRTIO_RING_F_INDIRECT_DESC);
"""

# deleting just the call leaves an empty else-if body, which still compiles.
# vq_start now sits between that body and the break, and it is kept in the anchor
# so the match cannot drift onto some other closing brace.
CALL_SIGNAL = """                vq_notify(idx);
            }
            vq_start(idx);
"""

CALL_SIGNAL_MUTATED = """            }
            vq_start(idx);
"""

# ---- blk/utils.cpp: the shared virtqueue engine, VIRTIO_RING_F_EVENT_IDX ----

SHOULD_NOTIFY_MARK = "bool VirtQueueServer::should_notify(uint16_t old_used_idx) {\n"

SHOULD_NOTIFY_ALWAYS = """bool VirtQueueServer::should_notify(uint16_t old_used_idx) {
    return true;
}
"""

SHOULD_NOTIFY_NEVER = """bool VirtQueueServer::should_notify(uint16_t old_used_idx) {
    return false;
}
"""

NOTIFY_VALID_BLOCK = """    if (!notify_valid.load(std::memory_order_relaxed)) {
        notify_valid.store(true, std::memory_order_relaxed);
        return true;
    }
"""

# anchored on the call plus the two braces that close dispatch_avail's loop and
# the function itself, so it can only ever land on the per-head publish at the
# end of that loop body. The transports publish too (vq_start, SET_VRING_BASE);
# this mutant must not touch those.
PUBLISH_PER_HEAD = """        publish_avail_event();
    }
}
"""

PUBLISH_PER_HEAD_MUTATED = """    }
}
"""

NEED_EVENT = "    return (uint16_t)(new_idx - event_idx - 1) < (uint16_t)(new_idx - old);\n"

NEED_EVENT_NO_MINUS1 = "    return (uint16_t)(new_idx - event_idx) < (uint16_t)(new_idx - old);\n"

USED_EVENT = "    return __atomic_load_n(&avail->ring[num], __ATOMIC_ACQUIRE);\n"

USED_EVENT_PREV_SLOT = "    return __atomic_load_n(&avail->ring[num - 1], __ATOMIC_ACQUIRE);\n"

# the drain loop only: should_notify has its own fence, at a different indent
DRAIN_FENCE = """            for (;;) {
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
                if (vring_avail_idx(avail) == last_avail)
"""

DRAIN_FENCE_MUTATED = """            for (;;) {
                if (vring_avail_idx(avail) == last_avail)
"""

DRAIN_PROGRESS = """                uint16_t before = last_avail;
                dispatch_avail();
                if (last_avail == before)
                    break;
"""

DRAIN_PROGRESS_MUTATED = """                dispatch_avail();
"""

# ---- blk/utils.cpp: the shared virtqueue engine, VIRTIO_RING_F_INDIRECT_DESC ----
# Every anchor below is a slice of virtio_blk_serve_chain's indirect branch. The
# deletion anchors carry their own LOG_ERROR text, because the branch is full of
# `bad = true; break;` and a bare one would match several times.

INDIRECT_GATE = """            if (!allow_indirect) {
                LOG_ERROR("virtio-blk `: indirect descriptor, unsupported", tag);
                bad = true;
                break;
            }
"""

INDIRECT_WITH_NEXT = """            if (de.flags & VRING_DESC_F_NEXT) {
                LOG_ERROR("virtio-blk `: indirect descriptor at ` also carries NEXT", tag, d);
                bad = true;
                break;
            }
"""

TBL_LEN_CHECK = "            if (de.len == 0 || de.len % sizeof(vring_desc)) {\n"

TBL_LEN_ONLY_ZERO = "            if (de.len == 0) {\n"

TBL_LEN_ONLY_PARTIAL = "            if (de.len % sizeof(vring_desc)) {\n"

TBL_CAP_BLOCK = """            if (n > MAX_INDIRECT_ENTRIES) {
                LOG_ERROR("virtio-blk `: indirect table at ` declares ` entries, the limit is `",
                          tag, d, n, MAX_INDIRECT_ENTRIES);
                bad = true;
                break;
            }
"""

# nocab deletes the block above; capoffbyone changes only the comparison, so its
# anchor is the single line. Both start with `if (n >`, which is why the block
# anchor has to carry the log text to stay unique.
TBL_CAP_TEST = "            if (n > MAX_INDIRECT_ENTRIES) {\n"

TBL_CAP_TEST_OFFBYONE = "            if (n >= MAX_INDIRECT_ENTRIES) {\n"

TBL_TRANSLATE = "            const vring_desc* tbl = (const vring_desc*)translate(de.addr, de.len, false);\n"

TBL_TRANSLATE_WRITABLE = "            const vring_desc* tbl = (const vring_desc*)translate(de.addr, de.len, true);\n"

# tblwrite only; the cast is needed because push() takes void* and tbl is const.
# Inserted BEFORE the `if (!tbl)` refusal, hence the guard.
TBL_AS_DATA = (TBL_TRANSLATE +
               "            if (tbl) ((de.flags & VRING_DESC_F_WRITE) ? wr : rd).push((void*)tbl, de.len);\n")

TBL_INDEX_BOUND = "                if (t >= n) {\n"

TBL_INDEX_BOUND_RING = "                if (t >= ring_num) {\n"

# The descriptor is a COPY, not a pointer: the walk snapshots each entry out of guest
# memory before reading any of its fields, so a field cannot answer one way to the
# check and another to the use. Both spellings below therefore drop the `*` and the
# `&` -- that is not a typo, and an anchor written the old way matches nothing.
TBL_ARRAY = "                const vring_desc te = tbl[t];\n"

TBL_ARRAY_RING = "                const vring_desc te = desc[t];\n"

TBL_NESTED = """                if (te.flags & VRING_DESC_F_INDIRECT) {
                    // \u00a72.7.5.3.1: "The driver MUST NOT set the VIRTQ_DESC_F_INDIRECT flag
                    // within an indirect descriptor (ie. only one table per descriptor)."
                    // That is a DRIVER requirement -- \u00a72.7.5.3.2 gives the device no
                    // matching MUST -- so refusing here is our own strictness, and the
                    // reason is the bound: nesting turns one step budget into a product
                    // of budgets, with the depth the peer's.
                    LOG_ERROR("virtio-blk `: nested indirect descriptor at table entry `", tag, t);
                    bad = true;
                    break;
                }
"""

# ---- blk/vhost-user.cpp: the INDIRECT_DESC offer and the seg_max it publishes ----

OFFER_INDIRECT = ("                         (1ULL << VIRTIO_RING_F_EVENT_IDX) | "
                  "(1ULL << VIRTIO_RING_F_INDIRECT_DESC) |\n")

OFFER_NO_INDIRECT = "                         (1ULL << VIRTIO_RING_F_EVENT_IDX) |\n"

GATE_ON_NEGOTIATED = ("                vqs[i]->srv.indirect_desc.store(!!(negotiated & "
                      "(1ULL << VIRTIO_RING_F_INDIRECT_DESC)),\n")

GATE_ON_OFFER = ("                vqs[i]->srv.indirect_desc.store(!!(offer_features & "
                 "(1ULL << VIRTIO_RING_F_INDIRECT_DESC)),\n")

RESET_INDIRECT = """            // Same hazard, and a more concrete one: a true value left here means a NEW
            // session that never negotiated bit 28 still walks tables, i.e. the
            // offer-instead-of-negotiated mistake comes back on the reset path.
            q->srv.indirect_desc.store(false, std::memory_order_relaxed);
"""

SEG_MAX_ADVERTISED = "        bc->seg_max = VIRTIO_BLK_SEG_MAX_ADVERTISED;\n"

SEG_MAX_FULL = "        bc->seg_max = MAX_INDIRECT_ENTRIES;\n"

# vq_start's stale-cursor guard. `num` is what makes the test "can this ring vouch for
# that BASE"; `0` discards every BASE with anything outstanding.
STALE_BASE_GUARD = "        if (in_flight > q->srv.num)\n"

STALE_BASE_GUARD_ANY = "        if (in_flight > 0)\n"


def die(msg):
    print("MUTATE-FAILED: " + msg, file=sys.stderr)
    sys.exit(1)


def sub_once(text, old, new, what):
    n = text.count(old)
    if n != 1:
        die("anchor for %s matched %d times, expected 1" % (what, n))
    return text.replace(old, new)


def span_of(text, mark, what):
    """the text from `mark` up to and including the next line that is exactly
    '    }' -- a function body at this indent has no such line of its own"""
    if text.count(mark) != 1:
        die("anchor for %s matched %d times, expected 1" % (what, text.count(mark)))
    start = text.index(mark)
    end = text.index("\n    }\n", start)
    if end < 0:
        die("no closing brace after %s" % what)
    return start, end + len("\n    }\n")


def span_of_toplevel(text, mark, what):
    """like span_of, but for a namespace-scope function in blk/utils.cpp, whose
    closing brace sits at column 0. span_of here always yields a wrong span, and
    which way it goes wrong depends only on the target's shape:

    - The target has an inner four-space-indented brace, so span_of stops at the
      FIRST one. should_notify has such a brace (the notify_valid block), so the
      span comes up short and leaves an orphaned tail.
    - The target has no such brace, so span_of runs on into whatever function
      follows. publish_avail_event is four lines long and has none, so the span
      swallows dozens of later functions and leaves a stray closing brace.

    Both are loud, and it is worth saying so plainly because the tempting story
    is that the second would slip through: it does not. Sweeping all 34
    column-0 functions in utils.cpp, the 23 that over-read every come out at
    brace delta +1 -- there is no case that over-reads and stays balanced. That
    is structural, not luck: the span always starts at a function's opening
    brace and stops at an INNER closing brace of a later one, so that later
    function's own column-0 brace survives as an extra, however many functions
    were swallowed on the way.

    So the cost of using span_of here is not a silently wrong mutant. It is a
    build error pointing at code the mutation never intended to touch, which by
    the ledger's own rule voids that mutant for the round and sends you hunting
    for a defect that is not there. This helper exists so the span is right the
    first time.

    Sizes are deliberately not quoted: they drift with utils.cpp. For scale only,
    as of 2026-09-17 the publish_avail_event over-read spanned about 5.0k
    characters and 88 lines against a 121-character function."""
    if text.count(mark) != 1:
        die("anchor for %s matched %d times, expected 1" % (what, text.count(mark)))
    start = text.index(mark)
    end = text.index("\n}\n", start)
    return start, end + len("\n}\n")


def main():
    if len(sys.argv) != 2:
        die("usage: mutate-vhost-user.py {backup|restore|check|%s}" % "|".join(MUTANTS))
    mode = sys.argv[1]

    if mode == "check":
        check()
        return

    if mode == "backup":
        # An in-flight stamp means a mutant may still be in one of these sources.
        # Backing up then would capture the mutation as pristine, and the restore
        # that follows would faithfully put it back -- silently, and with a stamp
        # saying the tree is clean. Restore first.
        #
        # This guard buys more here than in a script with private backups, because
        # `utils.cpp.orig` is shared with mutate-blk.py by design: a backup that
        # captures a mutant also poisons the copy that script restores from. Its
        # restore digests each .orig and would die on the mismatch; this script's
        # restore has no such check, so whatever it copies back is trusted on sight.
        # The sibling direction -- backing up while mutate-blk.py is the one
        # mid-experiment -- is a separate question and is deliberately not closed
        # here; that script checks this one's stamp, this one does not check its.
        if STAMP.exists():
            die("backup refused: this script is already mid-experiment (%s exists), "
                "so a source may still carry a mutant; run `restore` first" % STAMP)
        for src, bak in SRCS.values():
            bak.write_bytes(src.read_bytes())
            print("BACKUP_OK", bak)
        STAMP.write_text("")
        return
    if mode == "restore":
        require_stamp("restore")
        for src, bak in SRCS.values():
            if not bak.exists():
                die("no backup at %s" % bak)
            src.write_bytes(bak.read_bytes())
            print("RESTORE_OK", src)
        STAMP.unlink()
        return

    if mode not in MUTANTS:
        die("unknown mutation %r" % mode)
    require_stamp(mode)
    src = SRCS[MUTANTS[mode]][0]
    if not src.exists():
        die("no source at %s (set PHOTON_VHU_ROOT if this is not the tree the "
            "experiment runs against)" % src)
    text = mutate(mode, src.read_text())
    src.write_text(text)
    print("MUTATED", mode, src)


def mutate(mode, text):
    """Apply one mutant to `text` and return the result.

    Pure, and deliberately the only place a mutation is expressed: `check` runs this
    same function on a string it never writes back, so the self-check cannot drift
    away from the thing it is checking."""
    if mode == "overread":
        s, e = span_of(text, RECV_MSG_MARK, "recv_msg")
        text = text[:s] + OLD_RECV_MSG + text[e:]
    elif mode == "nobound":
        text = sub_once(text, BOUND_BLOCK, "", "the oversized-payload check")
    elif mode == "noprotofeature":
        text = sub_once(text, OFFER_FEATURES, OFFER_FEATURES_NO_BIT30,
                        "device feature bit 30 in offer_features")
    elif mode == "nocallsignal":
        text = sub_once(text, CALL_SIGNAL, CALL_SIGNAL_MUTATED,
                        "the SET_VRING_CALL callfd signal")
    elif mode == "alwaynotify":
        s, e = span_of_toplevel(text, SHOULD_NOTIFY_MARK, "should_notify")
        text = text[:s] + SHOULD_NOTIFY_ALWAYS + text[e:]
    elif mode == "nevenotify":
        s, e = span_of_toplevel(text, SHOULD_NOTIFY_MARK, "should_notify")
        text = text[:s] + SHOULD_NOTIFY_NEVER + text[e:]
    elif mode == "nofirstnotify":
        text = sub_once(text, NOTIFY_VALID_BLOCK, "",
                        "the unconditional first notification")
    elif mode == "noavailevent":
        text = sub_once(text, PUBLISH_PER_HEAD, PUBLISH_PER_HEAD_MUTATED,
                        "the per-head avail_event publish in dispatch_avail")
    elif mode == "wrongneed":
        text = sub_once(text, NEED_EVENT, NEED_EVENT_NO_MINUS1,
                        "the - 1 in vring_need_event")
    elif mode == "wrongevent":
        text = sub_once(text, USED_EVENT, USED_EVENT_PREV_SLOT,
                        "the used_event slot in vring_used_event")
    elif mode == "nofence":
        text = sub_once(text, DRAIN_FENCE, DRAIN_FENCE_MUTATED,
                        "the drain loop's store-load barrier")
    elif mode == "noprogress":
        text = sub_once(text, DRAIN_PROGRESS, DRAIN_PROGRESS_MUTATED,
                        "the drain loop's no-progress break")
    elif mode == "nogate":
        text = sub_once(text, INDIRECT_GATE, "",
                        "the allow_indirect refusal on an indirect descriptor")
    elif mode == "noxtnext":
        text = sub_once(text, INDIRECT_WITH_NEXT, "",
                        "the refusal of an indirect descriptor that also carries NEXT")
    elif mode == "partialent":
        text = sub_once(text, TBL_LEN_CHECK, TBL_LEN_ONLY_ZERO,
                        "the whole-number-of-entries half of the table length check")
    elif mode == "zerolen":
        text = sub_once(text, TBL_LEN_CHECK, TBL_LEN_ONLY_PARTIAL,
                        "the non-zero half of the table length check")
    elif mode == "nocab":
        text = sub_once(text, TBL_CAP_BLOCK, "",
                        "the MAX_INDIRECT_ENTRIES cap on a table's entry count")
    elif mode == "capoffbyone":
        text = sub_once(text, TBL_CAP_TEST, TBL_CAP_TEST_OFFBYONE,
                        "the comparison operator on the table entry cap")
    elif mode == "tblwrite":
        text = sub_once(text, TBL_TRANSLATE, TBL_TRANSLATE_WRITABLE,
                        "the read-only translate of the table itself")
    elif mode == "tblasdata":
        text = sub_once(text, TBL_TRANSLATE, TBL_AS_DATA,
                        "the table descriptor, which carries no data")
    elif mode == "tblbound":
        text = sub_once(text, TBL_INDEX_BOUND, TBL_INDEX_BOUND_RING,
                        "the table entry index bound, which is n and not ring_num")
    elif mode == "tblarray":
        text = sub_once(text, TBL_ARRAY, TBL_ARRAY_RING,
                        "the array a table entry is read out of")
    elif mode == "nestok":
        text = sub_once(text, TBL_NESTED, "",
                        "the refusal of a nested indirect descriptor")
    elif mode == "nooffer":
        text = sub_once(text, OFFER_INDIRECT, OFFER_NO_INDIRECT,
                        "device feature bit 28 in offer_features")
    elif mode == "gateonoffer":
        text = sub_once(text, GATE_ON_NEGOTIATED, GATE_ON_OFFER,
                        "the negotiated word the table walk is gated on")
    elif mode == "noreset":
        text = sub_once(text, RESET_INDIRECT, "",
                        "the indirect_desc clear in vq_reset")
    elif mode == "segmaxfull":
        text = sub_once(text, SEG_MAX_ADVERTISED, SEG_MAX_FULL,
                        "the two framing descriptors subtracted from seg_max")
    elif mode == "snapall":
        text = sub_once(text, STALE_BASE_GUARD, STALE_BASE_GUARD_ANY,
                        "the ring size the stale-cursor guard compares against")
    else:
        # A mode MUTANTS lists but no branch implements would otherwise fall through
        # and write the text back unchanged, printing MUTATED for a mutation that
        # never happened -- the one silent failure this script exists to avoid.
        # `check` reports the same thing as CHECK_NOOP; the real path must not be
        # quieter than the self-check. AssertionError rather than die() so `check`
        # does not file a script bug under anchor rot.
        raise AssertionError("no mutation body for %r, though MUTANTS lists it" % mode)
    return text


def check():
    """Re-grep every anchor against the live tree and write nothing.

    This is the discipline a round otherwise lacks: an anchor that has drifted is
    found here, before a build and a test run are spent on a binary that was never
    mutated. It needs no stamp, because it reads the sources and applies each mutant
    to an in-memory copy.

    Run it against the tree the experiment will use, and after any edit to a file an
    anchor names. Anchors here are literal against literal, so the descriptor-spelling
    change in `4423d05` (`de->` to `de.`) rotted five of them and took the seven
    mutants that reference them out of service; a reworded comment inside an anchor
    breaks it the same way, and did to mutate-blk.py's. `sub_once` reports the damage
    only at mutation time, after `backup` has already stamped the tree."""
    for mode, key in MUTANTS.items():
        if key not in SRCS:
            die("mutant %s targets %r, which SRCS does not know" % (mode, key))
    if STAMP.exists():
        # Not a refusal -- the read is safe -- but the verdict needs the caveat: the
        # mutant currently applied has already consumed its own anchor, so that one
        # reports CHECK_ANCHOR for a reason that is not rot.
        print("CHECK_WARN mid-experiment: %s exists, so a mutant is applied to these "
              "sources right now and its own anchor will report CHECK_ANCHOR for a "
              "reason that is not rot; `restore` first for a clean verdict" % STAMP)
    bad = 0
    for mode in sorted(MUTANTS):
        src = SRCS[MUTANTS[mode]][0]
        if not src.exists():
            print("CHECK_MISSING %s %s" % (mode, src))
            bad += 1
            continue
        text = src.read_text()
        try:
            mutated = mutate(mode, text)
        except (SystemExit, ValueError):
            # SystemExit is sub_once's and span_of's die(); ValueError is text.index()
            # inside them when the mark is there but the closing brace after it is not.
            # Both mean the anchor no longer names exactly one editable place.
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
    print("CHECK_ALL_OK %d mutants, nothing written" % len(MUTANTS))


if __name__ == "__main__":
    main()
