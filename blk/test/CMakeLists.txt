# every blk suite shares the test helpers: harness.h (declarations) + harness.cpp
# (implementations), compiled into each test binary
set(BLK_TEST_SRC harness.cpp)

# This directory is reached only when blk is in the build -- the top-level
# CMakeLists gates add_subdirectory(blk/test) on PHOTON_MODULE_blk_ENABLED, which is
# PHOTON_ENABLE_BLK -- so that switch is not restated here, and neither is any
# transport's target condition. Each per-transport suite below reads its own
# transport's PHOTON_MODULE_<name>_ENABLED, the property photon_module() records and
# photon_module_targets() gates the library on, so a suite is built exactly where the
# code it tests is and neither side keeps a second copy of the condition to drift.

# checks of the helpers themselves (the stress block format, its validator and
# failure classifier): pure logic, so they run wherever blk is built. Named for what
# it tests -- a bare "test-blk" read like a sibling of the per-transport suites.
photon_add_test(test-blk-harness test-harness.cpp ${BLK_TEST_SRC})

# the WorkPool primitives every transport hands its serving coroutines to; no
# transport and no kernel involvement, so it runs wherever blk is built, like the
# harness checks do
photon_add_test(test-blk-pool test-blk-pool.cpp ${BLK_TEST_SRC})

photon_add_test(test-blk-nbd test-nbd.cpp ${BLK_TEST_SRC})

# The shared virtio execution engine both virtio transports sit on: a vring in
# this process's memory, no transport and no kernel, so the request-parsing and
# notification-ordering decisions are reachable without a frontend mock deciding
# what a legal request looks like. The engine needs neither, but its definitions
# sit under #ifdef __linux__ in blk_utils alongside the virtio core they serve,
# while blk/utils.h declares the class unconditionally. So on other platforms
# this test compiles and then fails to link -- gate it here rather than let the
# declaration promise a definition that is not there. This is the one suite with no
# module property to read: blk_utils is enabled wherever blk is, on every platform,
# because nbd.cpp needs its portable half, and the LINUX-only virtio core sitting
# beside that half is not separately gated.
if (LINUX)
    photon_add_test(test-blk-vq test-blk-vq.cpp ${BLK_TEST_SRC})
endif ()

get_property(_suite_tcmu GLOBAL PROPERTY PHOTON_MODULE_blk_tcmu_ENABLED)
if (_suite_tcmu)
    photon_add_test(test-blk-tcmu test-tcmu.cpp ${BLK_TEST_SRC})
endif ()

# test-vduse.cpp includes blk/vduse.cpp TEXTUALLY, so this binary defines the vduse
# transport itself instead of importing it from photon_shared. The note at the top of
# that file carries the full argument, what it costs, and the two `nm` commands that
# show it; the short version is that several ungated cases at its bottom need symbols
# that are file-local to vduse.cpp and in no header, and the note at the top of that
# file names them. vduse.cpp's three observation seams are read-only, so they can
# report the mapping byte counts but can neither install a mapping nor drive a tick.
get_property(_suite_vduse GLOBAL PROPERTY PHOTON_MODULE_blk_vduse_ENABLED)
if (_suite_vduse)
    photon_add_test(test-blk-vduse test-vduse.cpp ${BLK_TEST_SRC})
endif ()

get_property(_suite_vhost_user GLOBAL PROPERTY PHOTON_MODULE_blk_vhost_user_ENABLED)
if (_suite_vhost_user)
    photon_add_test(test-blk-vhost-user test-vhost-user.cpp ${BLK_TEST_SRC})
endif ()

# ublk additionally rides the iouring engine, and that requirement is blk_ublk's
# ENABLE, so it is already inside the property this reads.
get_property(_suite_ublk GLOBAL PROPERTY PHOTON_MODULE_blk_ublk_ENABLED)
if (_suite_ublk)
    photon_add_test(test-blk-ublk test-ublk.cpp ${BLK_TEST_SRC})
endif ()

# The two CLI tools -- vduse-cli and vhost-user-cli -- are NOT here and NOT tests:
# they live in blk/ and the top-level CMakeLists builds them as operator tools. Only
# vhost-user-cli is driven from this directory.
#
# Four Python scripts live here, and none is registered with add_test: they are manual
# instruments that exit non-zero on a missing prerequisite instead of skipping quietly.
# The two qemu-vhost-user-*.py are drivers, and both need a hand-built QEMU tree that CI
# does not have -- the peer one patches QEMU's own vhost-user-blk test to accept our
# backend and takes its path in PHOTON_VHU_BACKEND, and the qtest one runs the six cases
# and reports what each actually proved. The two mutate-*.py are mutation harnesses
# forming ONE ledger of seventeen mutants: mutate-vhost-user.py owns the protocol and
# EVENT_IDX guards in blk/vhost-user.cpp (test-vhost-user.cpp cites its `noprogress`
# record by name), and mutate-blk.py carries the eight multiqueue mutants targeting
# utils.cpp, ublk.cpp, tcmu.cpp or nbd.cpp while refusing the other nine BY NAME rather
# than dropping them silently. This note is the only pointer to mutate-blk.py from any
# tracked file. Nothing here execs vduse-cli: test-vduse.cpp ports its rescue recipe
# in-tree rather than spawning it.
#
# One shell script lives here as well, load-kernel-modules.sh. ci.linux.x86_64.yml runs
# it before the build: it loads the modules the privileged transports need and records
# which of them came up, because those suites skip in a log line that ctest counts as a
# pass. It never installs anything and is never fatal.
