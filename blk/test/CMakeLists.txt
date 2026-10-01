# every blk suite shares the test helpers: harness.h (declarations) + harness.cpp
# (implementations), compiled into each test binary
set(BLK_TEST_SRC harness.cpp)

# checks of the helpers themselves (the stress block format, its validator and
# failure classifier): pure logic, so they run on all platforms. Named for what it
# tests -- a bare "test-blk" read like a sibling of the per-transport suites.
photon_add_test(test-blk-harness test-harness.cpp ${BLK_TEST_SRC})

# the WorkPool primitives every transport hands its serving coroutines to; no
# transport and no kernel involvement, so it runs on all platforms like the
# harness checks do
photon_add_test(test-blk-pool test-blk-pool.cpp ${BLK_TEST_SRC})

# the shared virtio execution engine both virtio transports sit on: a vring in this
# process's memory, no transport and no kernel, so the request-parsing and
# notification-ordering decisions are reachable without a frontend mock deciding
# what a legal request looks like. Runs everywhere for the same reason.
photon_add_test(test-blk-vq test-blk-vq.cpp ${BLK_TEST_SRC})

photon_add_test(test-blk-nbd test-nbd.cpp ${BLK_TEST_SRC})
if (LINUX)
    # tcmu is LINUX-gated (blk_tcmu), so its symbols are absent from photon_shared
    # on other platforms; only build the test where the transport exists.
    photon_add_test(test-blk-tcmu test-tcmu.cpp ${BLK_TEST_SRC})
    photon_add_test(test-blk-vduse test-vduse.cpp ${BLK_TEST_SRC})
    photon_add_test(test-blk-vhost-user test-vhost-user.cpp ${BLK_TEST_SRC})
    if (PHOTON_ENABLE_URING)
        # ublk additionally rides the iouring engine (blk_ublk's ENABLE)
        photon_add_test(test-blk-ublk test-ublk.cpp ${BLK_TEST_SRC})
    endif ()
endif ()

# The two .cc files here are deliberately NOT build targets -- that extension is
# what AGENTS.md reserves for assistant programs outside normal compilation, and
# each is built by hand with the g++ line in its own header comment.
#
# vduse-cli.cc diagnoses and unwedges vduse devices, and is only runnable as root
# with the vduse module loaded. It links a photon build for alog (the LOG_*
# macros) but cannot include <linux/virtio_ring.h> -- that header's inline
# vring_init() assigns void* to typed pointers, which C++ rejects -- so it copies
# the split-ring structs the way blk/utils.h does.
#
# vhost-user-cli.cc serves one image over a vhost-user socket so that an
# INDEPENDENT frontend can be pointed at blk/vhost-user.cpp: QEMU's own
# tests/qtest/vhost-user-blk-test.c, whose peer is normally qemu-storage-daemon.
# test-vhost-user.cpp's mock frontend shares this repo's assumptions about the
# wire format, so it cannot catch a format both sides got wrong the same way;
# QEMU's frontend can.
