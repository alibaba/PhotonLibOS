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
#pragma once
#include "fuse_session_loop.h"

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

class FuseSessionLoopEPoll : public FuseSessionLoop {
private:
    struct fuse_session *se;
#if FUSE_USE_VERSION < 30
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

    int wait_for_readable(EventLoop *);

    static void *fuse_do_work(void *args);

    int on_accept(EventLoop *);

#if FUSE_USE_VERSION < 30
    int bufctor(void **buf);
    int bufdtor(void *buf);
#else
    int bufctor(struct fuse_buf **pbuf);
    int bufdtor(struct fuse_buf *fbuf);
#endif

public:
    explicit FuseSessionLoopEPoll(struct fuse_session *session);
    ~FuseSessionLoopEPoll();
    void run() { loop->run(); }
};

}  // namespace fs
}  // namespace alibaba
