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
#define FUSE_USE_VERSION 29
#endif

#include <fuse/fuse.h>
#include <fuse/fuse_common.h>
#include <fuse/fuse_common_compat.h>
#include <fuse/fuse_lowlevel.h>
#include <fuse/fuse_lowlevel_compat.h>
#include <fuse/fuse_opt.h>
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
    struct fuse_chan *ch;
    IdentityPool<void, 32> bufpool;
    ThreadPool<32> threadpool;
    EventLoop *loop;
    size_t bufsize = 0;
    int ref = 1;
    condition_variable cond;

    int wait_for_readable(EventLoop *) {
        if (fuse_session_exited(se)) return -1;
        auto ret = wait_for_fd_readable(fuse_chan_fd(ch));
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
        struct fuse_chan *tmpch = obj->ch;
        struct fuse_buf fbuf;
        memset(&fbuf, 0, sizeof(fbuf));
        fbuf.mem = obj->bufpool.get();
        fbuf.size = obj->bufsize;
        DEFER({
            obj->bufpool.put(fbuf.mem);
            if (--obj->ref == 0) obj->cond.notify_all();;
        });
        auto res = fuse_session_receive_buf(obj->se, &fbuf, &tmpch);
        if (res == -EINTR || res == -EAGAIN) return nullptr;
        if (res <= 0) {
            obj->loop->stop();
            return nullptr;
        }
        fuse_session_process_buf(obj->se, &fbuf, tmpch);
        return nullptr;
    }

    int on_accept(EventLoop *) {
        ++ref;
        auto th = threadpool.thread_create(&FuseSessionLoop::worker, (void *)this);
        thread_yield_to(th);
        return 0;
    }

    int bufctor(void **buf) {
        *buf = malloc(bufsize);
        if (*buf == nullptr) return -1;
        return 0;
    }

    int bufdtor(void *buf) {
        free(buf);
        return 0;
    }

public:
    explicit FuseSessionLoop(struct fuse_session *session)
        : se(session),
          ch(fuse_session_next_chan(se, NULL)),
          bufpool({this, &FuseSessionLoop::bufctor},
                  {this, &FuseSessionLoop::bufdtor}),
          threadpool(),
          loop(new_event_loop({this, &FuseSessionLoop::wait_for_readable},
                              {this, &FuseSessionLoop::on_accept})),
          bufsize(fuse_chan_bufsize(ch)) {}

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
    char *mountpoint;
    int multithreaded;
    int res;
    size_t op_size = sizeof(*(op));
    fuse = fuse_setup(args.argc, args.argv, op, op_size, &mountpoint, &multithreaded,
                      user_data);
    if (fuse == NULL) return 1;
    if (multithreaded) {
        if (cfg.threads < 1) cfg.threads = 1;
        if (cfg.threads > 64) cfg.threads = 64;
        auto se = fuse_get_session(fuse);
        auto ch = fuse_session_next_chan(se, NULL);
        auto fd = fuse_chan_fd(ch);
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
        auto se = fuse_get_session(fuse);
        res = fuse_session_loop_mpt(se);
        fuse_session_reset(se);
    }
    fuse_teardown(fuse, mountpoint);
    if (res == -1) return 1;

    return 0;
}

}  // namespace photon
