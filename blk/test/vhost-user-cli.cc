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

// vhost-user-cli: serve one image file over a vhost-user socket, so that a REAL
// vhost-user frontend can be pointed at this module's backend.
//
//   vhost-user-cli <sock_path> <image_path> [size_bytes]
//
// Listens on <sock_path> (SERVER role) and serves <image_path> as a virtio-blk
// device. size_bytes defaults to the image's fstat size. The controller's scope
// directory is dirname(<sock_path>), created if absent.
//
// WHY THIS EXISTS. test-vhost-user.cpp drives blk/vhost-user.cpp with a MOCK
// frontend that lives in the same file and keeps its own copy of the wire
// constants. That mock is the right tool for the rejection paths -- it can send
// what QEMU never would -- but it structurally cannot catch a wire format that
// BOTH sides got wrong the same way, which is the shared-defect failure mode this
// module has already been bitten by twice (the message header's packing, and the
// mock's own out-of-range descriptor heads). QEMU's tests/qtest/vhost-user-blk-test.c
// is the independent peer: it runs qemu-system's real vhost-user frontend and
// drives it with libqos's user-space virtio-blk driver under -accel qtest, so it
// needs no guest kernel, no TCG and no /dev/kvm. Its only assumption about the
// peer is "something serves this socket and exits 0 on SIGTERM" -- which is what
// this program is. QEMU's own peer in that test is qemu-storage-daemon, so
// running both gives an A/B baseline: same frontend, same libqos driver, same six
// cases.
//
// BUILD (from the repo root, against any photon build dir; Linux only, since
// blk/vhost-user.cpp itself is Linux-gated). Against the shared library:
//   g++ -O2 -Wall -I include -o vhost-user-cli blk/test/vhost-user-cli.cc -L build/output -lphoton -Wl,-rpath,$PWD/build/output
// or statically, adding whatever that build enabled:
//   g++ -O2 -Wall -I include -o vhost-user-cli blk/test/vhost-user-cli.cc build/output/libphoton.a -lpthread -ldl -laio -luring
//
// EXIT STATUS IS PART OF THE CONTRACT. QEMU's quit_storage_daemon() SIGTERMs the
// peer and asserts it exited 0, so teardown here is orderly and main returns 0
// once serving has started: shutdown() unlinks the socket, then the device, the
// controller, the image and photon are released by DEFER in reverse order.
// Setup failures return non-zero, which the harness reads as a hard error.
//
// Assistant program, deliberately NOT a build target -- hence the .cc extension
// AGENTS.md reserves for programs outside normal compilation (see vduse-cli.cc).

#include "../blk.h"

#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/fs/localfs.h>
#include <photon/io/signal.h>
#include <photon/thread/thread.h>

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

using namespace photon;

// photon::sync_signal, not signal(2). photon::init already created a signalfd
// whose mask is every signal, and sync_signal is what puts a signal into the
// process signal mask so it reaches that fd, dispatching the handler on photon's
// own signal thread. Mixing a plain signal(2) handler with that signalfd produced
// a spurious `fire_signal:SignalFD read failed errno=11` at ERROR level. Because
// the handler now runs on a photon thread it may touch photon primitives, so this
// is a semaphore rather than a flag, which also removes the poll the flag version
// needed in order to notice it.
static photon::semaphore g_term(0);
static void on_term(int) { g_term.signal(1); }

// Not dirname(3): the POSIX one modifies its argument and the XPG one may return
// static storage, and this runs on argv[1] which the log lines still quote.
static int scope_dir_of(const char* sock_path, char* out, size_t outsz) {
    const char* slash = strrchr(sock_path, '/');
    size_t n = slash ? (size_t)(slash - sock_path) : 0;
    if (n == 0) {   // a bare filename: its directory is the cwd
        if (outsz < 2)
            LOG_ERROR_RETURN(ENAMETOOLONG, -1, "buffer too small for the scope directory");
        out[0] = '.';
        out[1] = '\0';
        return 0;
    }
    if (n + 1 > outsz)
        LOG_ERROR_RETURN(ENAMETOOLONG, -1, "socket path ` does not fit a `-byte buffer",
                         sock_path, outsz);
    memcpy(out, sock_path, n);
    out[n] = '\0';
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        LOG_ERROR("usage: ` <sock_path> <image_path> [size_bytes]", argv[0]);
        return 2;
    }
    set_log_output_level(ALOG_INFO);

    char dir[PATH_MAX];
    if (scope_dir_of(argv[1], dir, sizeof(dir)) < 0)
        return 2;
    if (mkdir(dir, 0755) < 0 && errno != EEXIST)
        LOG_ERRNO_RETURN(0, 2, "cannot create the scope directory ", dir);

    // INIT_IO_NONE, matching test-vhost-user's main(): the backend IFile is a
    // plain localfs regular file, so its preadv/pwritev are synchronous and there
    // is no async engine to initialise.
    if (photon::init(INIT_EVENT_DEFAULT, INIT_IO_NONE) < 0)
        LOG_ERROR_RETURN(0, 1, "photon::init failed");
    DEFER(photon::fini());

    fs::IFileSystem* lfs = fs::new_localfs_adaptor();
    if (!lfs)
        LOG_ERROR_RETURN(ENOMEM, 1, "cannot create the localfs adaptor");
    DEFER(delete lfs);
    fs::IFile* file = lfs->open(argv[2], O_RDWR);
    if (!file)
        LOG_ERRNO_RETURN(0, 1, "cannot open the image ", argv[2]);
    DEFER(delete file);

    uint64_t size = 0;
    if (argc == 4) {
        size = strtoull(argv[3], nullptr, 0);
    } else {
        struct stat sb;
        if (file->fstat(&sb) < 0)
            LOG_ERRNO_RETURN(0, 1, "cannot stat the image ", argv[2]);
        size = (uint64_t)sb.st_size;
    }
    if (size == 0)
        LOG_ERROR_RETURN(EINVAL, 1, "the image ` has no size to serve", argv[2]);

    blk::BlkDevInfo info;
    info.identity = argv[1];   // vhost-user's identity IS the socket path
    info.size = size;
    info.sector_size_shift = 9;
    info.features = blk::FEATURE_FLUSH;

    blk::VhostUserController::Config cfg(info);
    cfg.sock_path = argv[1];
    cfg.sock_role = blk::VhostUserController::SockRole::SERVER;

    auto* ctl = blk::new_vhost_user_controller(dir);
    if (!ctl)
        LOG_ERRNO_RETURN(0, 1, "cannot create the vhost-user controller for ", dir);
    DEFER(delete ctl);
    auto* dev = ctl->new_device(cfg);
    if (!dev)
        LOG_ERRNO_RETURN(0, 1, "cannot create the vhost-user device on ", argv[1]);
    DEFER(delete dev);
    if (dev->start(file) < 0)
        LOG_ERRNO_RETURN(0, 1, "cannot start serving ", argv[1]);
    DEFER(dev->shutdown());

    // From here the peer may SIGTERM us at any moment, and exit 0 is what its
    // harness asserts -- so the handler only releases the semaphore and the DEFERs
    // above do the teardown on the way out of main().
    photon::sync_signal(SIGTERM, on_term);
    photon::sync_signal(SIGINT, on_term);
    LOG_INFO("serving ` (` bytes) on `, pid `; SIGTERM stops it",
             argv[2], size, argv[1], getpid());
    g_term.wait(1);
    LOG_INFO("SIGTERM/SIGINT: tearing down");
    return 0;
}
