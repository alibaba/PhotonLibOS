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
#include "fuse_adaptor_sync.h"

#if FUSE_USE_VERSION >= 30
#include <fuse3/fuse_lowlevel.h>
#else
#include <fuse/fuse_lowlevel.h>
#endif
#include <thread>
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

#include <photon/common/alog.h>
#include <photon/common/event-loop.h>
#include <photon/io/fd-events.h>
#include <photon/fs/exportfs.h>
#include <photon/fs/filesystem.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread-pool.h>
#include <photon/thread/thread-local.h>

namespace photon {
namespace fs{

#define FUSE_DEV_IOC_MAGIC  229
#define FUSE_DEV_IOC_CLONE  _IOR(FUSE_DEV_IOC_MAGIC, 0, uint32_t)

static photon::thread_local_ptr<int> iofd;

int FuseSessionLoopSync::set_fd() {
    uint32_t masterfd = fuse_session_fd(se_);
    const char *devname = "/dev/fuse";
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
    int clonefd = open(devname, O_RDWR | O_CLOEXEC);
    if (clonefd == -1) {
        return -1;
    }
#ifndef O_CLOEXEC
    fcntl(clonefd, F_SETFD, FD_CLOEXEC);
#endif

    int res = ioctl(clonefd, FUSE_DEV_IOC_CLONE, &masterfd);
    if (res == -1) {
        close(clonefd);
        return -1;
    }
    blk_fd_ = clonefd;

    int noblk_fd = open(devname, O_RDWR | O_CLOEXEC);
    if (noblk_fd == -1) {
        return -1;
    }
#ifndef O_CLOEXEC
    fcntl(noblk_fd, F_SETFD, FD_CLOEXEC);
#endif
    res = ioctl(noblk_fd, FUSE_DEV_IOC_CLONE, &masterfd);
    if (res == -1) {
        close(noblk_fd);
        return -1;
    }
    int flags = fcntl(noblk_fd, F_GETFL, 0);
    fcntl(noblk_fd, F_SETFL, (flags | O_NONBLOCK));
    nonblk_fd_ = noblk_fd;

    printf("masterfd<%d>, local fd<%d>, non fd<%d>\n", masterfd, blk_fd_, nonblk_fd_);
    return 0;
}

FuseSessionLoopSync::FuseSessionLoopSync(struct fuse_session *se)
  : error_(0),
    se_(se),
    num_worker_(0) {
    max_workers_ = 32;
    numavail_ = 0;
    waitting_ = false;

    set_fd();
    for (int i = 0; i < max_workers_; ++i) {
        WorkerArgs<void *, int> *wrkargs = new WorkerArgs<void *, int>();
        wrkargs->args = std::make_tuple(reinterpret_cast<void *>(this), i);
        auto th = photon::thread_create(
            &FuseSessionLoopSync::fuse_do_work,
            reinterpret_cast<void *>(wrkargs));

        photon::thread_enable_join(th);
        workers_.emplace_back(th);
        num_worker_++;
        idlers_.emplace(i, 1);
        numavail_++;
        photon::thread_yield_to(th);
    }
    assert(num_worker_ = max_workers_);
    // sanyulh start here
}

void FuseSessionLoopSync::run() {
    sem_.signal(num_worker_);
    while (!workers_.empty()) {
        auto th = workers_.back();
        photon::thread_join((photon::join_handle *)th);
        workers_.pop_back();
        num_worker_--;
    }
    assert(num_worker_ = 0); 
}

FuseSessionLoopSync::~FuseSessionLoopSync() {
    if (se_) fuse_session_exit(se_);

    for (int i = 0; i < (int)(workers_.size()); ++i) {
        auto th = workers_.back();
        photon::thread_join((photon::join_handle *)th);
        workers_.pop_back();
    }
}

void *FuseSessionLoopSync::fuse_do_work(void *data) {
    auto wrkargs = (WorkerArgs<void *, int> *)data;
    auto loop = (FuseSessionLoopSync *)(std::get<0>(wrkargs->args));
    auto idx = std::get<1>(wrkargs->args);
    struct fuse_session *se = loop->se_;
    struct fuse_buf fbuf = {
        .mem = NULL,
    };

    loop->sem_.wait(1);
    --loop->numavail_;
    loop->idlers_.erase(idx);
    while(!fuse_session_exited(se)) {
        if (loop->numavail_ == loop->num_worker_ -1) {
            *iofd = loop->blk_fd_;
            int res = fuse_session_receive_buf(se, &fbuf);
            if (res <= 0) {
               break;
            }

            if (loop->idlers_.empty()) {
                photon::thread_interrupt(loop->workers_[loop->idlers_.begin()->first]);
            }

            fuse_session_process_buf(se, &fbuf);
        } else {
            *iofd = loop->nonblk_fd_;
            int res = fuse_session_receive_buf(se, &fbuf);
            if (res < 0) {
                if (res != -EWOULDBLOCK || res != -EAGAIN) {
                    break;
                }
                res = 0;
            }

            if (res == 0) {
                ++loop->numavail_;
                loop->idlers_.emplace(idx, 1);
                if (loop->waitting_)
                    photon::thread_usleep(-1);
                else {
                    loop->waitting_ = true;
                    auto ret = photon::wait_for_fd_readable(loop->nonblk_fd_);
                    loop->waitting_ = false;
                    if (ret < 0) {
                        if (errno == EALREADY) {
                            photon::thread_usleep(-1);                        
                        }
                    }
                }
                --(loop->numavail_);
                loop->idlers_.erase(idx);
            } else {
                if (!loop->idlers_.empty()) {
                    photon::thread_interrupt(loop->workers_[loop->idlers_.begin()->first]);
                }
                fuse_session_process_buf(se, &fbuf);
            }
        }
    }

    --(loop->num_worker_);
    if (!loop->idlers_.empty()) {
        for (const auto& pair : loop->idlers_) {
            photon::thread_interrupt(loop->workers_[pair.first]);
        }
    }
    return nullptr;
}

static ssize_t custom_writev(int fd, struct iovec *iov, int count, void *userdata)
{
    (void)userdata;

    return writev(*iofd, iov, count);
}

static ssize_t custom_read(int fd, void *buf, size_t len, void *userdata)
{
    (void)userdata;
    return read(*iofd, buf, len);
}

int FuseSessionLoopSync::set_custom_io(struct fuse_session *se) {
    const struct fuse_custom_io custom_io = {
        .writev = photon::fs::custom_writev,
	.read = photon::fs::custom_read,
	.splice_receive = NULL,
	.splice_send = NULL,
	.clone_fd = NULL,
    };

    return fuse_session_custom_io(se, &custom_io, fuse_session_fd(se));
}

}  // namespace fs
}  // namespace photon
