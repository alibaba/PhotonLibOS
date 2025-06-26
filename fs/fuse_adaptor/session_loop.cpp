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
#include "session_loop.h"

#if FUSE_USE_VERSION >= 30
#include <fuse3/fuse_lowlevel.h>
#else
#include <fuse/fuse_lowlevel.h>
#endif

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <vector>
#include <tuple>
#include <unordered_set>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/ioctl.h>
#include <unistd.h>

#include <photon/common/alog.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#include <photon/thread/thread-local.h>
#include <photon/thread/thread-pool.h>
#include <photon/common/event-loop.h>

namespace photon {
namespace fs {

class EPollSessionLoop : public FuseSessionLoop {
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
        auto obj = (EPollSessionLoop *)args;
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
        auto th = threadpool.thread_create(&EPollSessionLoop::fuse_do_work, (void *)this);
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
   explicit EPollSessionLoop(struct fuse_session *session)
        : se(session),
#if FUSE_USE_VERSION < FUSE_MAKE_VERSION(3, 0)
          ch(fuse_session_next_chan(se, NULL)),
          bufsize(fuse_chan_bufsize(ch)),
#endif
          bufpool({this, &EPollSessionLoop::bufctor},
                  {this, &EPollSessionLoop::bufdtor}),
          threadpool(),
          loop(new_event_loop({this, &EPollSessionLoop::wait_for_readable},
                              {this, &EPollSessionLoop::on_accept})) {}

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

    ~EPollSessionLoop() {
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
    return NewObj<EPollSessionLoop>(se)->init();
}


#if FUSE_USE_VERSION >= FUSE_MAKE_VERSION(3, 0)

#define FUSE_DEV_IOC_MAGIC  229
#define FUSE_DEV_IOC_CLONE  _IOR(FUSE_DEV_IOC_MAGIC, 0, uint32_t)

static photon::thread_local_ptr<int> iofd;

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

template <typename ...Args>
class WorkerArgs {
public:
    std::tuple<Args...> args;
};

class SyncSessionLoop : public FuseSessionLoop {
public:
    int init() {
        set_fd();
        for (int i = 0; i < max_workers_; ++i) {
            auto th = photon::thread_create11(
                &SyncSessionLoop::fuse_do_work, this, i);

            photon::thread_enable_join(th);
            workers_.emplace_back(th);
            idlers_.emplace(i);
            photon::thread_yield_to(th);
        }

        return 0;
    }

    explicit SyncSessionLoop(struct fuse_session *se)
        : error_(0),
          se_(se),
          blk_fd_(-1),
          nonblk_fd_(-1),
          max_workers_(32),
          waitting_(false) {}

    void run() {
        sem_.signal(workers_.size());
        wait_all_fini();
    }

    ~SyncSessionLoop() {
        if (se_) fuse_session_exit(se_);
        wait_all_fini();
        if (blk_fd_ != -1)
          close(blk_fd_);
        if (nonblk_fd_ != -1)
          close(nonblk_fd_);
    }

    static int set_custom_io(struct fuse_session *se) {
        const struct fuse_custom_io custom_io = {
            .writev = photon::fs::custom_writev,
	        .read = photon::fs::custom_read,
	        .splice_receive = NULL,
	        .splice_send = NULL,
#if FUSE_USE_VERSION >= FUSE_MAKE_VERSION(3, 17)
        	.clone_fd = NULL,
#endif
        };
#if FUSE_USE_VERSION >= FUSE_MAKE_VERSION(3, 17)
        return fuse_session_custom_io(se, &custom_io,
                                      sizeof(struct fuse_custom_io),
                                      fuse_session_fd(se));
#else
        return fuse_session_custom_io(se, &custom_io, fuse_session_fd(se));
#endif
    }

private:
    int error_;
    struct fuse_session *se_;
    int blk_fd_;
    int nonblk_fd_;
    int max_workers_;
    bool waitting_;
    std::vector<photon::thread *> workers_;
    std::unordered_set<int>idlers_;
    photon::semaphore sem_;

    void wait_all_fini() {
        while (!workers_.empty()) {
            auto th = workers_.back();
            photon::thread_join((photon::join_handle *)th);
            workers_.pop_back();
        }
    }

    int set_fd() {
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

        LOG_INFO("masterfd:`", masterfd, " block fd `", blk_fd_, "  nonblock fd: `", nonblk_fd_);
        return 0;
    }

    void *fuse_do_work(int id) {
        auto wrk_id = id;
        struct fuse_buf fbuf = {
            .mem = NULL,
        };

        sem_.wait(1);
        idlers_.erase(wrk_id);
        while(!fuse_session_exited(se_)) {
            if (idlers_.size() == (workers_.size() - 1)) {
                *iofd = blk_fd_;
                int res = fuse_session_receive_buf(se_, &fbuf);
                if (res <= 0) {
                    break;
                }

                if (!idlers_.empty()) {
                    photon::thread_interrupt(workers_[*idlers_.begin()]);
                }

                fuse_session_process_buf(se_, &fbuf);
            } else {
                *iofd = nonblk_fd_;
                int res = fuse_session_receive_buf(se_, &fbuf);
                if (res < 0) {
                    if (res != -EWOULDBLOCK || res != -EAGAIN) {
                        break;
                    }
                    res = 0;
                }

                if (res == 0) {
                    idlers_.emplace(wrk_id);
                    if (waitting_)
                        photon::thread_usleep(-1);
                    else {
                        waitting_ = true;
                        auto ret = photon::wait_for_fd_readable(nonblk_fd_);
                        waitting_ = false;
                        if (ret < 0) {
                            if (errno == EALREADY) {
                                photon::thread_usleep(-1);                        
                            }
                        }
                    }
                    idlers_.erase(wrk_id);
                } else {
                    if (!idlers_.empty()) {
                        photon::thread_interrupt(workers_[*idlers_.begin()]);
                    }   
                    fuse_session_process_buf(se_, &fbuf);
                }
            }
        }

        for (const auto& wrk : idlers_) {
            photon::thread_interrupt(workers_[wrk]);
        }
        return nullptr;
    }
};

FuseSessionLoop *new_sync_session_loop(struct fuse_session *se) {
    return NewObj<SyncSessionLoop>(se)->init();
}

int set_sync_custom_io(struct fuse_session *se) {
    return SyncSessionLoop::set_custom_io(se);
}

#endif

}  // namespace fs
}  // namespace photon
