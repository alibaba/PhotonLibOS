#!/usr/bin/env python3
"""Mutation harness for blk/vhost-user.cpp and blk/utils.cpp.

The four protocol mutants guard transport-level behaviours. Do not book this group
as "found by QEMU": that is substantiated for nocallsignal only -- it is a defect
the real frontend catches at its qvirtio_wait_queue_isr and a polling mock
structurally cannot. overread and nobound go the other way, and have long been
caught in-repo by pipelined_messages and oversized_payload. The eight EVENT_IDX
mutants are a different case, and each is annotated where it is listed below:
several of them have no real-frontend detection at all, so do not read this file
as claiming one. What every mutant here does
establish is narrower and still worth having -- that the new mock-side assertion
actually fires, rather than passing for a reason
that has nothing to do with the guard it is supposed to cover. Every mutation is
an anchored replacement that fails loudly if the anchor is not found exactly once,
so a mutation can never silently degrade into a no-op and report a false "the test
still passes".

Two source files, because the guards live at two levels: the protocol guards are
in the transport (blk/vhost-user.cpp) and the VIRTIO_RING_F_EVENT_IDX guards are
in the shared virtqueue engine (blk/utils.cpp), which both transports sit on.
`backup` and `restore` cover both; each mutant declares which file it acts on.
`backup` before the first mutant and `restore` after every single one -- a
mutant left in place silently poisons every later run.

    ./mutate-vhost-user.py backup
    ./mutate-vhost-user.py <mutant>
    ./mutate-vhost-user.py restore

blk/vhost-user.cpp -- protocol layer:

    overread        # revert recv_msg to one recvmsg of sizeof(vhost_user_msg)
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

Run on the VM against ~/PhotonLibOS (a copy, not the repository). The sequence is
`backup`, then one mutation at a time with a build and a run between it and the next
`backup`, then `restore`; `restore` and every mutation refuse to run without the
stamp `backup` leaves, because `restore` copies .orig over the source and an .orig of
unknown age reverts the tree silently.
"""
import pathlib
import sys

# NOT inside the rsync tree: the VM copy is refreshed with `rsync --delete`, which
# removes anything the repository does not have, so a sibling backup would vanish
# mid-experiment and leave `restore` with nothing to restore from.
SRCS = {
    "vhost-user": (pathlib.Path.home() / "PhotonLibOS/blk/vhost-user.cpp",
                   pathlib.Path.home() / "vhost-user.cpp.orig"),
    "utils": (pathlib.Path.home() / "PhotonLibOS/blk/utils.cpp",
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

BOUND_BLOCK = """        if (m->size > sizeof(m->payload)) {
            // copied out first: m is not const here, and alog's forwarding
            // reference cannot bind a packed field (see the access rule above)
            int32_t req = m->request;
            uint32_t sz = m->size;
            LOG_ERROR_RETURN(EPROTO, -1, "vhost-user request ` declares a ` byte payload, "
                             "the largest this protocol has is `",
                             req, sz, (uint32_t)sizeof(m->payload));
        }
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

# deleting just the call leaves an empty else-if body, which still compiles
CALL_SIGNAL = """                vq_notify();
            }
            break;
"""

CALL_SIGNAL_MUTATED = """            }
            break;
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

NOTIFY_VALID_BLOCK = """    if (!notify_valid) {
        notify_valid = true;
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
        die("usage: mutate-vhost-user.py {backup|restore|%s}" % "|".join(MUTANTS))
    mode = sys.argv[1]

    if mode == "backup":
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
    text = src.read_text()
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
    src.write_text(text)
    print("MUTATED", mode, src)


if __name__ == "__main__":
    main()
