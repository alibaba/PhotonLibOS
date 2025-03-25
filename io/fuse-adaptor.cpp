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

#include "fuse-adaptor.h"

#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 35
#endif

#if FUSE_USE_VERSION >= 30
#include <fuse3/fuse.h>
#include <fuse3/fuse_common.h>
#include <fuse3/fuse_lowlevel.h>
#include <fuse3/fuse_opt.h>
#else
#include <fuse/fuse.h>
#include <fuse/fuse_common.h>
#include <fuse/fuse_lowlevel.h>
#include <fuse/fuse_opt.h>
#endif
#include <linux/limits.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../common/event-loop.h"
#include "../common/identity-pool.h"
#include "../common/utility.h"
#include "aio-wrapper.h"
#include "fd-events.h"
#include "../thread/thread-pool.h"
#include "../photon.h"
#include <vector>
#include <thread>

namespace photon {

class FuseSessionLoop {
protected:
    struct fuse_session *se;
#if FUSE_USE_VERSION < 30
    struct fuse_chan *ch;
    IdentityPool<void, 32> bufpool;
    size_t bufsize = 0;
#endif
    ThreadPool<32> threadpool;
    EventLoop *loop;
    int ref = 1;
    condition_variable cond;

    int wait_for_readable(EventLoop *) {
        if (fuse_session_exited(se)) return -1;
#if FUSE_USE_VERSION < 30
        auto ret = wait_for_fd_readable(fuse_chan_fd(ch));
#else
        auto ret = wait_for_fd_readable(fuse_session_fd(se));
#endif
        if (ret < 0) {
            if (errno == ETIMEDOUT) {
                return 0;
            }
            return -1;
        }
        return 1;
    }

    static void *worker(void *args) {
        auto obj = (FuseSessionLoop *)args;
        struct fuse_buf fbuf;
        memset(&fbuf, 0, sizeof(fbuf));
#if FUSE_USE_VERSION < 30
        struct fuse_chan *tmpch = obj->ch;
        fbuf.mem = obj->bufpool.get();
        fbuf.size = obj->bufsize;
        DEFER({
            obj->bufpool.put(fbuf.mem);
        });
        auto res = fuse_session_receive_buf(obj->se, &fbuf, &tmpch);
#else
        auto res = fuse_session_receive_buf(obj->se, &fbuf);
#endif
        DEFER({
            if (--obj->ref == 0) obj->cond.notify_all();
        });
        if (res == -EINTR || res == -EAGAIN) return nullptr;
        if (res <= 0) {
            obj->loop->stop();
            return nullptr;
        }
#if FUSE_USE_VERSION < 30
        fuse_session_process_buf(obj->se, &fbuf, tmpch);
#else
        fuse_session_process_buf(obj->se, &fbuf);
#endif
        return nullptr;
    }

    int on_accept(EventLoop *) {
        ++ref;
        auto th = threadpool.thread_create(&FuseSessionLoop::worker, (void *)this);
        thread_yield_to(th);
        return 0;
    }

#if FUSE_USE_VERSION < 30
    int bufctor(void **buf) {
        *buf = malloc(bufsize);
        if (*buf == nullptr) return -1;
        return 0;
    }

    int bufdtor(void *buf) {
        free(buf);
        return 0;
    }
#endif

public:
    explicit FuseSessionLoop(struct fuse_session *session)
        : se(session),
#if FUSE_USE_VERSION < 30
          ch(fuse_session_next_chan(se, NULL)),
          bufpool({this, &FuseSessionLoop::bufctor},
                  {this, &FuseSessionLoop::bufdtor}),
          bufsize(fuse_chan_bufsize(ch)),
#endif
          threadpool(),
          loop(new_event_loop({this, &FuseSessionLoop::wait_for_readable},
                              {this, &FuseSessionLoop::on_accept})) {}

    ~FuseSessionLoop() {
        loop->stop();
        --ref;
        while (ref != 0) {
            cond.wait_no_lock();
        }

        delete loop;
    }

    void run() { loop->run(); }
};

static int fuse_session_loop_mpt(struct fuse_session *se) {
    FuseSessionLoop loop(se);
    loop.run();
    return 0;
}

#if FUSE_USE_VERSION >= 30
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
};

#define USER_OPT(t, p, v) { t, offsetof(struct user_config, p), v }
struct fuse_opt user_opts[] = { USER_OPT("threads=%d", threads, 0), FUSE_OPT_END };

int run_fuse(int argc, char *argv[], const struct ::fuse_operations *op,
             void *user_data) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct user_config cfg{ .threads = 4, };
    fuse_opt_parse(&args, &cfg, user_opts, NULL);
    struct fuse *fuse;
    struct fuse_session* se;
    char *mountpoint;
    int multithreaded;
    int res;
    size_t op_size = sizeof(*(op));
#if FUSE_USE_VERSION < 30
    fuse = fuse_setup(args.argc, args.argv, op, op_size, &mountpoint, &multithreaded, user_data);
#else
    fuse = fuse3_setup(args.argc, args.argv, op, op_size, &mountpoint, &multithreaded, user_data);
#endif
    if (fuse == NULL) return 1;
    if (multithreaded) {
        if (cfg.threads < 1) cfg.threads = 1;
        if (cfg.threads > 64) cfg.threads = 64;
        se = fuse_get_session(fuse);
#if FUSE_USE_VERSION < 30
        auto ch = fuse_session_next_chan(se, NULL);
        auto fd = fuse_chan_fd(ch);
#else
        auto fd = fuse_session_fd(se);
#endif
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        std::vector<std::thread> ths;
        for (int i = 0; i < cfg.threads; ++i) {
          ths.emplace_back(std::thread([&]() {
              init(INIT_EVENT_EPOLL, INIT_IO_LIBAIO);
              DEFER(fini());
              if (fuse_session_loop_mpt(se) != 0) res = -1;
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
#if FUSE_USE_VERSION < 30
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

}  // namespace photon
