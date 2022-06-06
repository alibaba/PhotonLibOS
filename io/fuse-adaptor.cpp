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
        if (res == -EINTR) return nullptr;
        if (res <= 0) {
            obj->loop->stop();
            return nullptr;
        }
        fuse_session_process_buf(obj->se, &fbuf, tmpch);
        return nullptr;
    }

    int on_accept(EventLoop *) {
        ++ref;
        threadpool.thread_create(&FuseSessionLoop::worker, (void *)this);
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
    fuse_session_reset(se);
    return 0;
}

static int fuse_loop_mpt(struct fuse *f) {
    // fuse may fork and daemonized the process,
    // but aioctx will not copy, so init aio wrapper again
    auto rfdev = fd_events_init();
    auto rlaio = libaio_wrapper_init();
    DEFER({
        if (rfdev == 0) fd_events_fini();
        if (rlaio == 0) libaio_wrapper_fini();
    });
    return fuse_session_loop_mpt(fuse_get_session(f));
}

int run_fuse(int argc, char *argv[], const struct ::fuse_operations *op,
             void *user_data) {
    struct fuse *fuse;
    char *mountpoint;
    int multithreaded;
    int res;
    size_t op_size = sizeof(*(op));

    fuse = fuse_setup(argc, argv, op, op_size, &mountpoint, &multithreaded,
                      user_data);
    if (fuse == NULL) return 1;

    res = fuse_loop_mpt(fuse);

    fuse_teardown(fuse, mountpoint);
    if (res == -1) return 1;

    return 0;
}

}  // namespace photon
