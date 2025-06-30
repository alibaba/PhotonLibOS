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
#include "fuse_adaptor.h"
#include "session_loop.h"

#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 317
#endif

#include <thread>
#include <vector>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <photon/io/fd-events.h>
#include <photon/common/alog.h>
#include <photon/fs/exportfs.h>
#include <photon/fs/filesystem.h>
#include <photon/thread/thread.h>

namespace photon {
namespace fs{

/*
static void fuse_logger(enum fuse_log_level level, const char *fmt, va_list ap)
{
    const int LEN = 4096;
    char buf[LEN + 1];
    int ret = vsnprintf(buf, LEN, fmt, ap);
    if (ret < 0) return;
    if (ret > LEN) ret = LEN;

    buf[LEN] = '\n';
    log_output(buf, buf + LEN + 1);
}
*/
int fuser_go(IFileSystem* fs_, int argc, char* argv[])
{
    if (!fs_)
        return 0;

    // fuse_set_log_func(&fuse_logger);

    umask(0);
    set_fuse_fs(fs_);
    return run_fuse(argc, argv, get_fuse_xmp_oper(), NULL);
}

int fuser_go_exportfs(IFileSystem *fs_, int argc, char *argv[]) {
    if (!fs_) return 0;
    if (photon::init(INIT_EVENT_DEFAULT, INIT_IO_DEFAULT | INIT_IO_EXPORTFS))
        return -1;
    DEFER(photon::fini());

    auto efs = export_as_sync_fs(fs_);

    // fuse_set_log_func(&fuse_logger);

    umask(0);
    set_fuse_fs(efs);
    photon::semaphore exit_sem(0);
    std::thread([&] {
        fuse_main(argc, argv, get_fuse_xmp_oper(), NULL);
        exit_sem.signal(1);
    }).detach();
    exit_sem.wait(1);
    return 0;
}

static int fuse_session_loop_mpt(struct fuse_session *se,
                                 uint64_t looptype = FUSE_SESSION_LOOP_EPOLL) {
    auto loop = new_session_loop(se, looptype);
    loop->run();
    delete loop;
    return 0;
}

#if FUSE_USE_VERSION >= FUSE_MAKE_VERSION(3, 0)
fuse* fuse3_setup(int argc, char *argv[], const struct ::fuse_operations *op,
           size_t op_size, char **mountpoint, int *multithreaded, void *user_data)
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct fuse *fuse;
    struct fuse_cmdline_opts opts;
    struct fuse_session * se;

    if (fuse_parse_cmdline(&args, &opts) != 0)
        return NULL;

    *mountpoint = opts.mountpoint;
    *multithreaded = !opts.singlethread;
    if (opts.show_version) {
        fuse_lowlevel_version();
        goto out1;
    }

    if (opts.show_help) {
        if(args.argv[0][0] != '\0')
            printf("usage: %s [options] <mountpoint>\n\n",
                   args.argv[0]);
        printf("FUSE options:\n");
        fuse_cmdline_help();
        fuse_lib_help(&args);
        goto out1;
    }

    if (!opts.show_help &&
        !opts.mountpoint) {
        fprintf(stderr, "error: no mountpoint specified\n");
        goto out1;
    }

    fuse = fuse_new(&args, op, op_size, user_data);
    fuse_opt_free_args(&args);
    if (fuse == NULL) {
        goto out1;
    }

    if (fuse_mount(fuse,opts.mountpoint) != 0) {
        goto out2;
    }

    if (fuse_daemonize(opts.foreground) != 0) {
        goto out3;
    }

    se = fuse_get_session(fuse);
    if (fuse_set_signal_handlers(se) != 0) {
        goto out3;
    }
    return fuse;

out3:
    fuse_unmount(fuse);
out2:
    fuse_destroy(fuse);
out1:
    free(opts.mountpoint);
    return NULL;
}
#endif

struct user_config {
    int threads;
    char *looptype;
};

uint64_t find_looptype(const char *name) {
    if (!strncmp(name, "epoll", 5))
        return FUSE_SESSION_LOOP_EPOLL;

    if (!strncmp(name, "sync", 4))
        return FUSE_SESSION_LOOP_SYNC;

    if (!strncmp(name, "io_uring_cas", 12))
        return FUSE_SESSION_LOOP_IOURING_CASCADING;

    if (!strncmp(name, "io_uring", 8))
        return FUSE_SESSION_LOOP_IOURING;

    return FUSE_SESSION_LOOP_EPOLL;
}

#define USER_OPT(t, p, v) { t, offsetof(struct user_config, p), v }
struct fuse_opt user_opts[] = { USER_OPT("threads=%d",  threads, 0),
                                USER_OPT("looptype=%s", looptype, 0),
                                FUSE_OPT_END };

int run_fuse(int argc, char *argv[], const struct ::fuse_operations *op,
             void *user_data) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct user_config cfg{ .threads = 4, .looptype = NULL };
    uint64_t looptype = FUSE_SESSION_LOOP_EPOLL;
    fuse_opt_parse(&args, &cfg, user_opts, NULL);

    if (cfg.looptype)
        looptype = find_looptype(cfg.looptype);
#if FUSE_USE_VERSION < FUSE_MAKE_VERSION(3, 13)
    looptype = FUSE_SESSION_LOOP_EPOLL;
#endif
    LOG_INFO("session cfg loop type: `", VALUE(cfg.looptype), " loop type: `", VALUE(looptype));

    struct fuse *fuse;
    struct fuse_session* se;
    char *mountpoint;
    int multithreaded;
    int res;
    size_t op_size = sizeof(*(op));

#if FUSE_USE_VERSION < FUSE_MAKE_VERSION(3, 0)
    fuse = fuse_setup(args.argc, args.argv, op, op_size, &mountpoint, &multithreaded, user_data);
#else
    fuse = fuse3_setup(args.argc, args.argv, op, op_size, &mountpoint, &multithreaded, user_data);
#endif
    if (fuse == NULL) return 1;
    se = fuse_get_session(fuse);

#if FUSE_USE_VERSION >= FUSE_MAKE_VERSION(3, 13)
    if (looptype == FUSE_SESSION_LOOP_SYNC) {
        set_sync_custom_io(se);
    }
#endif

    if (multithreaded) {
        if (cfg.threads < 1) cfg.threads = 1;
        if (cfg.threads > 64) cfg.threads = 64;

	std::vector<std::thread> ths;
        for (int i = 0; i < cfg.threads; ++i) {
          ths.emplace_back(std::thread([&]() {
              init(INIT_EVENT_EPOLL, INIT_IO_LIBAIO);
              DEFER(fini());
              if (fuse_session_loop_mpt(se, looptype) != 0) res = -1;
          }));
        }
        for (auto& th : ths) th.join();
        fuse_session_reset(se);
    } else {
        se = fuse_get_session(fuse);
        res = fuse_session_loop_mpt(se);
        fuse_session_reset(se);
    }
    fuse_remove_signal_handlers(fuse_get_session(fuse));
#if FUSE_USE_VERSION < FUSE_MAKE_VERSION(3, 0)
    fuse_unmount(mountpoint, fuse_session_next_chan(se, NULL));
#else
    fuse_unmount(fuse);
#endif
    fuse_destroy(fuse);
    free(mountpoint);
    fuse_opt_free_args(&args);
    if (res == -1) return 1;

    return 0;
}

}  // namespace fs
}  // namespace photon
