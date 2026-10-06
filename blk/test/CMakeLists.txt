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

photon_add_test(test-blk-nbd test-nbd.cpp ${BLK_TEST_SRC})
if (LINUX)
    # The shared virtio execution engine both virtio transports sit on: a vring in
    # this process's memory, no transport and no kernel, so the request-parsing and
    # notification-ordering decisions are reachable without a frontend mock deciding
    # what a legal request looks like. The engine needs neither, but its definitions
    # sit under #ifdef __linux__ in blk_utils alongside the virtio core they serve,
    # while blk/utils.h declares the class unconditionally. So on other platforms
    # this test compiles and then fails to link -- gate it here rather than let the
    # declaration promise a definition that is not there.
    photon_add_test(test-blk-vq test-blk-vq.cpp ${BLK_TEST_SRC})
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

# The two CLI tools -- vduse-cli and vhost-user-cli -- are NOT here and NOT tests:
# they live in blk/ and the top-level CMakeLists builds them as operator tools. Only
# vhost-user-cli is driven from this directory, by qemu-vhost-user-peer.py, which
# stays here because it is a test driver and takes the binary's path in
# PHOTON_VHU_BACKEND. Nothing here execs vduse-cli: test-vduse.cpp ports its rescue
# recipe in-tree rather than spawning it.
