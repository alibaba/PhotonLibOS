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
#include "fuse_session_loop.h"

#if FUSE_USE_VERSION >= 30
#include <fuse3/fuse_lowlevel.h>
#else
#include <fuse/fuse_lowlevel.h>
#endif

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <photon/common/alog.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread-pool.h>
#include <photon/common/event-loop.h>

namespace photon {
namespace fs {

class FuseSessionLoopEPoll : public FuseSessionLoop {
private:
    struct fuse_session *se;
#if FUSE_USE_VERSION < FUSE_MAKE_VERSION(3, 0)
    struct fuse_chan *ch;
    size_t bufsize = 0;
    IdentityPool<void, 32> bufpool;
#else
    IdentityPool<struct fuse_buf, 32> bufpool;
#endif
    ThreadPool<32> threadpool;
    EventLoop *loop;
    int ref = 1;
    condition_variable cond;

    int wait_for_readable(EventLoop *) {
        if (fuse_session_exited(se)) return -1;
    #if FUSE_USE_VERSION < FUSE_MAKE_VERSION(3, 0)
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

    static void *fuse_do_work(void *args) {
        auto obj = (FuseSessionLoopEPoll *)args;
#if FUSE_USE_VERSION < FUSE_MAKE_VERSION(3, 0)
        struct fuse_buf fbuf;
        memset(&fbuf, 0, sizeof(fbuf));
        struct fuse_chan *tmpch = obj->ch;
        fbuf.mem = obj->bufpool.get();
        fbuf.size = obj->bufsize;
        DEFER({
            obj->bufpool.put(fbuf.mem);
        });
        auto res = fuse_session_receive_buf(obj->se, &fbuf, &tmpch);
#else
        struct fuse_buf *pfbuf = obj->bufpool.get();
        DEFER({
            obj->bufpool.put(pfbuf);
        });
        auto res = fuse_session_receive_buf(obj->se, pfbuf);
#endif
        DEFER({
            if (--obj->ref == 0) obj->cond.notify_all();
        });
        if (res == -EINTR || res == -EAGAIN) return nullptr;
        if (res <= 0) {
            obj->loop->stop();
            return nullptr;
        }

#if FUSE_USE_VERSION < FUSE_MAKE_VERSION(3, 0)
        fuse_session_process_buf(obj->se, &fbuf, tmpch);
#else
        fuse_session_process_buf(obj->se, pfbuf);
#endif
        return nullptr;
    }

    int on_accept(EventLoop *) {
        ++ref;
        auto th = threadpool.thread_create(&FuseSessionLoopEPoll::fuse_do_work, (void *)this);
        thread_yield_to(th);
        return 0;
   }

#if FUSE_USE_VERSION < FUSE_MAKE_VERSION(3, 0)
    int bufctor(void **buf) {
        *buf = malloc(bufsize);
        if (*buf == nullptr) return -1;
        return 0;
    }

    int bufdtor(void *buf) {
        free(buf);
        return 0;
    }
#else
    int bufctor(struct fuse_buf **pbuf) {
        *pbuf = reinterpret_cast<struct fuse_buf *>(malloc(sizeof(struct fuse_buf)));
        if (*pbuf == nullptr) return -1;

        memset((*pbuf), 0, sizeof(struct fuse_buf));
        return 0;
    }

    int bufdtor(struct fuse_buf *fbuf) {
        if (fbuf->mem)
            free(fbuf->mem);
        fbuf->mem = NULL;

        free(fbuf);
        return 0;
    }
#endif

public:
   explicit FuseSessionLoopEPoll(struct fuse_session *session)
        : se(session),
#if FUSE_USE_VERSION < FUSE_MAKE_VERSION(3, 0)
          ch(fuse_session_next_chan(se, NULL)),
          bufsize(fuse_chan_bufsize(ch)),
#endif
          bufpool({this, &FuseSessionLoopEPoll::bufctor},
                  {this, &FuseSessionLoopEPoll::bufdtor}),
          threadpool(),
          loop(new_event_loop({this, &FuseSessionLoopEPoll::wait_for_readable},
                              {this, &FuseSessionLoopEPoll::on_accept})) {}

    int init() {
#if FUSE_USE_VERSION < FUSE_MAKE_VERSION(3, 0)
        auto ch = fuse_session_next_chan(se, NULL);
        auto fd = fuse_chan_fd(ch);
#else
        auto fd = fuse_session_fd(se);
#endif

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        return 0;
    }

    ~FuseSessionLoopEPoll() {
        loop->stop();
        --ref;
        while (ref != 0) {
            cond.wait_no_lock();
        }

        delete loop;
    }

    void run() { loop->run(); }
};

FuseSessionLoop* new_epoll_session_loop(struct fuse_session *se) {
    return NewObj<FuseSessionLoopEPoll>(se)->init();
}

}  // namespace fs
}  // namespace photon
