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
#include "fuse_adaptor_epoll.h"

#if FUSE_USE_VERSION >= 30
#include <fuse3/fuse_lowlevel.h>
#else
#include <fuse/fuse_lowlevel.h>
#endif
#include <thread>
#include <vector>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <photon/common/alog.h>
#include <photon/common/event-loop.h>
#include <photon/io/fd-events.h>
#include <photon/fs/exportfs.h>
#include <photon/fs/filesystem.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread-pool.h>

namespace photon {
namespace fs{


int FuseSessionLoopEPoll::wait_for_readable(EventLoop *) {
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

void *FuseSessionLoopEPoll::fuse_do_work(void *args) {
    auto obj = (FuseSessionLoopEPoll *)args;
#if FUSE_USE_VERSION < 30
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
#if FUSE_USE_VERSION < 30
    fuse_session_process_buf(obj->se, &fbuf, tmpch);
#else
    fuse_session_process_buf(obj->se, pfbuf);
#endif
    return nullptr;
}

int FuseSessionLoopEPoll::on_accept(EventLoop *) {
    ++ref;
    auto th = threadpool.thread_create(&FuseSessionLoopEPoll::fuse_do_work, (void *)this);
    thread_yield_to(th);
    return 0;
}

#if FUSE_USE_VERSION < 30
int FuseSessionLoopEPoll::bufctor(void **buf) {
    *buf = malloc(bufsize);
    if (*buf == nullptr) return -1;
    return 0;
}

int FuseSessionLoopEPoll::bufdtor(void *buf) {
    free(buf);
    return 0;
}
#else
int FuseSessionLoopEPoll::bufctor(struct fuse_buf **pbuf) {
    *pbuf = reinterpret_cast<struct fuse_buf *>(malloc(sizeof(struct fuse_buf)));
    if (*pbuf == nullptr) return -1;

    memset((*pbuf), 0, sizeof(struct fuse_buf));
    return 0;
}

int FuseSessionLoopEPoll::bufdtor(struct fuse_buf *fbuf) {
    if (fbuf->mem)
        free(fbuf->mem);
    fbuf->mem = NULL;

    free(fbuf);
    return 0;
}
#endif

FuseSessionLoopEPoll::FuseSessionLoopEPoll(struct fuse_session *session)
    : se(session),
#if FUSE_USE_VERSION < 30
      ch(fuse_session_next_chan(se, NULL)),
      bufsize(fuse_chan_bufsize(ch)),
#endif
      bufpool({this, &FuseSessionLoopEPoll::bufctor},
              {this, &FuseSessionLoopEPoll::bufdtor}),
      threadpool(),
      loop(new_event_loop({this, &FuseSessionLoopEPoll::wait_for_readable},
                          {this, &FuseSessionLoopEPoll::on_accept})) {}

FuseSessionLoopEPoll::~FuseSessionLoopEPoll() {
    loop->stop();
    --ref;
    while (ref != 0) {
        cond.wait_no_lock();
    }

    delete loop;
}

}  // namespace fs
}  // namespace alibaba
