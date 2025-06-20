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
#include <vector>
#include <tuple>
#include <unordered_map>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/ioctl.h>
#include <unistd.h>

#include <photon/thread/thread.h>

namespace photon {
namespace fs{

template <typename ...Args>
class WorkerArgs {
public:
    std::tuple<Args...> args;
};

class FuseSessionLoopSync : public FuseSessionLoop {
private:
    int error_;
    struct fuse_session *se_;
    int blk_fd_;
    int nonblk_fd_;
    int num_worker_;
    int numavail_;
    int max_workers_;
    bool waitting_;
    std::vector<photon::thread *> workers_;
    std::unordered_map<int, int>idlers_;
    photon::semaphore sem_;

   int set_fd();
   static void *fuse_do_work(void *data);

public:
    explicit FuseSessionLoopSync(fuse_session *session);
    ~FuseSessionLoopSync();
    void run();
    static int set_custom_io(struct fuse_session *se);
};

}  // namespace fs
}  // namespace photon
