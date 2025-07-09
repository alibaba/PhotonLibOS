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
struct fuse_handle {
    struct fuse *fuse = NULL;
    struct fuse_session *se = NULL;
    uint64_t looptype = FUSE_SESSION_LOOP_EPOLL;
    char *mountpoint = NULL;
    int multithreaded = 1;
    int threads = 4;
    void *userdata = NULL;

    int setup(
            int argc, char *argv[],
            const struct ::fuse_operations *ops,
            const struct ::fuse_lowlevel_ops *llops,
            void *user_data) {
        struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
        DEFER(fuse_opt_free_args(&args));

        struct user_config cfg{ .threads = 4, .looptype = NULL};
        fuse_opt_parse(&args, &cfg, user_opts, NULL);
        threads = cfg.threads;
        if (cfg.looptype)
            looptype = find_looptype(cfg.looptype);
#if FUSE_USE_VERSION < FUSE_MAKE_VERSION(3, 13)
        looptype = FUSE_SESSION_LOOP_EPOLL;
#endif
        LOG_INFO("session cfg loop type: `", VALUE(cfg.looptype),
                 " loop type: `", VALUE(looptype));

#if FUSE_USE_VERSION < FUSE_MAKE_VERSION(3, 0)
        if (!ops) {
            LOG_ERROR("Lowlevel interface is currently only supported when libfuse version is above 3.0");
            return -1;
        }
        fuse = fuse_setup(args.argc, args.argv,
                           ops, sizeof(*ops),
                           &mountpoint, &multithreaded, user_data);
        if (!fuse)
            return -1;
        se = fuse_get_session(fuse);
        return 0;
#else
        struct fuse_cmdline_opts opts;
        if (fuse_parse_cmdline(&args, &opts) != 0)
            return -1;

        mountpoint = opts.mountpoint;
        multithreaded = !opts.singlethread;
        if (opts.show_version) {
            fuse_lowlevel_version();
            return -1;
        }

        if (opts.show_help) {
            if(args.argv[0][0] != '\0')
                printf("usage: %s [options] <mountpoint>\n\n",
                       args.argv[0]);
            printf("FUSE options:\n");
            fuse_cmdline_help();
            fuse_lib_help(&args);
            fuse_lowlevel_help();
            return -1;
        }

        if (!opts.show_help &&
            !opts.mountpoint) {
            fprintf(stderr, "error: no mountpoint specified\n");
            return -1;
        }

        if (ops) {
            fuse = fuse_new(&args, ops, sizeof(*ops), user_data);
            if (!fuse)
                return -1;
            se = fuse_get_session(fuse);
        } else {
            se = fuse_session_new(&args, llops, sizeof(*llops), user_data);
        }

        if (fuse_session_mount(se, mountpoint) != 0) {
            return -1;
        }

        if (fuse_daemonize(opts.foreground) != 0) {
            return -1;
        }

        if (fuse_set_signal_handlers(se) != 0) {
            return -1;
        }
        return 0;
#endif
    }

    ~fuse_handle() {
        if (!se)
            return;

        fuse_remove_signal_handlers(se);
#if FUSE_USE_VERSION < FUSE_MAKE_VERSION(3, 0)
        fuse_unmount(mountpoint, fuse_session_next_chan(se, NULL));
#else
        fuse_session_unmount(se);
#endif
        if (fuse)
            fuse_destroy(fuse);
        else
            fuse_session_destroy(se);

        if (mountpoint)
            free(mountpoint);

        se = NULL;
        fuse = NULL;
        mountpoint = NULL;

        return;
    }
};

static int _run_fuse(int argc, char *argv[],
                     const struct ::fuse_operations *op,
                     const struct ::fuse_lowlevel_ops *llops,
                     void *user_data) {
    struct fuse_handle fh;
    int ret = fh.setup(argc, argv, op, llops, user_data);
    if (ret < 0)
        return ret;

    struct fuse_session *se = fh.se;
#if FUSE_USE_VERSION >= FUSE_MAKE_VERSION(3, 13)
    if (fh.looptype == FUSE_SESSION_LOOP_SYNC) {
        set_sync_custom_io(se);
    } else if (fh.looptype == FUSE_SESSION_LOOP_IOURING_CASCADING) {
        set_iouring_custom_io(se);
    }
#endif

    if (fh.multithreaded) {
        if (fh.threads < 1) fh.threads = 1;
        if (fh.threads > 64) fh.threads = 64;

        std::vector<std::thread> ths;
        for (int i = 0; i < fh.threads; ++i) {
          ths.emplace_back(std::thread([&]() {
              init(INIT_EVENT_EPOLL, INIT_IO_LIBAIO);
              DEFER(fini());
              if (fuse_session_loop_mpt(se, fh.looptype) != 0) ret = -1;
          }));
        }
        for (auto& th : ths) th.join();
        fuse_session_reset(se);
    } else {
        ret = fuse_session_loop_mpt(se);
        fuse_session_reset(se);
    }

    if (ret == -1) return 1;

    return 0;
}

int run_fuse(int argc, char *argv[],
             const struct ::fuse_operations *ops,
             void *user_data) {
    return _run_fuse(argc, argv, ops, NULL, user_data);
}

int run_fuse_ll(int argc, char *argv[],
                const struct ::fuse_lowlevel_ops *llops,
                void *user_data) {
    return _run_fuse(argc, argv, NULL, llops, user_data);
}

}  // namespace fs
}  // namespace photon
