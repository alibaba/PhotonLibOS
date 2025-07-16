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
#include <errno.h>
#include <string.h>

#include <photon/common/alog.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#include <photon/thread/thread-local.h>
#include <photon/thread/thread-pool.h>
#include <photon/common/event-loop.h>
#include <photon/io/iouring-wrapper.h>

namespace photon {
namespace fs {

#if FUSE_USE_VERSION >= 30
struct fuse_pipe {
    size_t size;
    int pipefd[2] = {-1, -1};
    ssize_t flush() {
        int nuldev_fd = open("/dev/null", O_WRONLY);
        if (nuldev_fd < 0) {
            LOG_ERROR("fuse-adaptor failed to open null device: `", strerror(errno));
            return -1;
        }

        return splice(pipefd[0], NULL, nuldev_fd, NULL, size, SPLICE_F_MOVE);
    }

    int setup() {
        int rv;
        if ((pipefd[0] != -1) && (pipefd[1] != -1))
            return 0;

#if !defined(HAVE_PIPE2) || !defined(O_CLOEXEC)
        rv = pipe(pipefd);
        if (rv == -1)
            return rv;

        if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK) == -1 ||
            fcntl(pipefd[1], F_SETFL, O_NONBLOCK) == -1 ||
            fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) == -1 ||
            fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) == -1) {
            close(pipefd[0]);
            close(pipefd[1]);
            return -1;
        }
#else
        rv = pipe2(pipefd, O_CLOEXEC | O_NONBLOCK);
        if (rv == -1)
            return rv;
#endif

        size_t page_size = getpagesize();
        fcntl(pipefd[0], F_SETPIPE_SZ, page_size *(256 + 1));
        size = page_size * (256 + 1);
        return 0;
    }

    int setdown() {
        if (pipefd[0] != -1)
            close(pipefd[0]);
        if (pipefd[1] != -1)
            close(pipefd[1]);

        pipefd[0] = -1;
        pipefd[1] = -1;
        size = 0;

        return 0;
    }
};

int fuse_session_receive_splice(
        struct fuse_session *se,
        struct fuse_buf *buf,
        struct fuse_pipe *iopipe) {
    return fuse_session_receive_splice(se, buf, iopipe, fuse_session_fd(se));
}

int fuse_session_receive_splice(
        struct fuse_session *se,
        struct fuse_buf *buf,
        struct fuse_pipe *iopipe,
        int fd) {

    // proto_minor and conn.want_ext are defined internally in libfuse and are
    // not visible here, so we cannot use the following two conditions to determine
    // the availability of splice_read. For now, we assume that splice_read is
    // always enabled.
    // if (se->conn.proto_minor < 14 ||
    //    !(se->conn.want_ext & FUSE_CAP_SPLICE_READ))

    errno = 0;
    ssize_t res = splice(fd, NULL, iopipe->pipefd[1], NULL, iopipe->size, 0);
    int err = errno;

    if (fuse_session_exited(se))
        return 0;

    if (res == -1) {
        if (err == ENODEV) {
            fuse_session_exit(se);
            return 0;
        }

        if (err == EINVAL) {
            iopipe->flush();
        }

        if (err != EINTR && err != EAGAIN)
            LOG_INFO("splice from device error `", err);
        return -err;
    }

    // The struct fuse_in_header is defined internally within libfuse and is not
    // visible externally, so we cannot truly obtain its size. Although we all
    // know that its size is likely 40.
    // if (res < sizeof(struct fuse_in_header)) {
    //    LOG_ERROR("short splice from fuse device `", res);
    //    iopipe->flush();
    //    return -EIO;
    // }

    struct fuse_buf tmpbuf = (struct fuse_buf) {
        .size = (size_t)res,
        .flags = FUSE_BUF_IS_FD,
        .fd = iopipe->pipefd[0],
    };

    buf->fd = tmpbuf.fd;
    buf->flags = tmpbuf.flags;
    buf->size = tmpbuf.size;

    return res;
}

int fuse_session_receive_fd(
        struct fuse_session *se,
        struct fuse_buf *buf,
        int fd) {
    int err = 0;
    ssize_t res = 0;
    size_t bufsize = getpagesize() * (256 + 1);

    if (!buf->mem) {
        buf->mem = malloc(bufsize);
    }
restart:
    errno = 0;
    res = read(fd, buf->mem, bufsize);
    err = errno;
    if (res == -1 && err == EWOULDBLOCK) {
        return 0;
    }

    if (fuse_session_exited(se)) {
        return 0;
    }

    if (res == -1) {
        // ENOENT means the operation was interrupted, it's safe to restart
        if (err == ENOENT)
            goto restart;

        if (err == ENODEV) {
			// Filesystem was unmounted, or connection was aborted
            //  via /sys/fs/fuse/connections
            fuse_session_exit(se);
            return 0;
        }

        // Errors occurring during normal operation: EINTR (read
        // interrupted), EAGAIN (nonblocking I/O), ENODEV (filesystem
        // umounted) */
        if (err != EINTR && err != EAGAIN) {
            LOG_ERROR("reading device was interrupted");
        }

        return -err;
    }

    return res;
}

int fuse_session_receive_uring_splice(struct fuse_session *se, struct fuse_buf *buf,
        int fd, struct fuse_pipe *iopipe, CascadingEventEngine *cee) {

    // proto_minor and conn.want_ext are defined internally in libfuse and are
    // not visible here, so we cannot use the following two conditions to determine
    // the availability of splice_read. For now, we assume that splice_read is
    // always enabled.
    // if (se->conn.proto_minor < 14 ||
    //    !(se->conn.want_ext & FUSE_CAP_SPLICE_READ))

    errno = 0;
    ssize_t res = iouring_splice(fd, 0, iopipe->pipefd[1], -1, iopipe->size, 0, -1, cee);
    int err = errno;

    if (fuse_session_exited(se))
        return 0;

    if (res == -1) {
        if (err == ENODEV) {
            fuse_session_exit(se);
            return 0;
        }

        if (err == EINVAL) {
            iopipe->flush();
        }

        if (err != EINTR && err != EAGAIN)
            LOG_INFO("splice from device error `", err);
        return -err;
    }

    // The struct fuse_in_header is defined internally within libfuse and is not
    // visible externally, so we cannot truly obtain its size. Although we all
    // know that its size is likely 40.
    // if (res < sizeof(struct fuse_in_header)) {
    //    LOG_ERROR("short splice from fuse device `", res);
    //    iopipe->flush();
    //    return -EIO;
    // }

    struct fuse_buf tmpbuf = (struct fuse_buf) {
        .size = (size_t)res,
        .flags = FUSE_BUF_IS_FD,
        .fd = iopipe->pipefd[0],
    };

    buf->fd = tmpbuf.fd;
    buf->flags = tmpbuf.flags;
    buf->size = tmpbuf.size;

    return res;
}
#endif

class EPollSessionLoop : public FuseSessionLoop {
private:
    loop_args args_;
    struct fuse_session *se;
#if FUSE_USE_VERSION < FUSE_MAKE_VERSION(3, 0)
    struct fuse_chan *ch;
    size_t bufsize = 0;
    IdentityPool<void, 32> bufpool;
#else
    IdentityPool<struct fuse_buf, 32> bufpool;
    IdentityPool<struct fuse_pipe, 32> pipepool;
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
        int res;
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
        res = fuse_session_receive_buf(obj->se, &fbuf, &tmpch);
#else
        struct fuse_buf *pfbuf = obj->bufpool.get();
        DEFER({
            obj->bufpool.put(pfbuf);
        });
        if (!obj->args_.force_splice_read)
            res = fuse_session_receive_buf(obj->se, pfbuf);
        else {
            struct fuse_pipe *pfpipe = obj->pipepool.get();
            pfpipe->setup();
            DEFER({
                obj->pipepool.put(pfpipe);
            });
            res = fuse_session_receive_splice(obj->se, pfbuf, pfpipe);
        }
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

    int init(const loop_args &args) {
        args_ = args;
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

FuseSessionLoop* new_epoll_session_loop(struct fuse_session *se, loop_args args) {
    return NewObj<EPollSessionLoop>(se)->init(args);
}


#if FUSE_USE_VERSION >= FUSE_MAKE_VERSION(3, 0)

#define FUSE_DEV_IOC_MAGIC  229
#define FUSE_DEV_IOC_CLONE  _IOR(FUSE_DEV_IOC_MAGIC, 0, uint32_t)

static photon::thread_local_ptr<int> iofd;
static photon::thread_local_ptr<struct fuse_pipe> iopipe;

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

static ssize_t custom_splice_receive(
    int fdin, off_t *offin,
    int fdout, off_t *offout, size_t len,
    unsigned int flags, void *userdata)
{
    // flags |= SPLICE_F_NONBLOCK;
    return splice(*iofd, offin, iopipe->pipefd[1], offout, len, 0);
}

static ssize_t custom_splice_send(
    int fdin, off_t *offin,
    int fdout, off_t *offout, size_t len,
    unsigned int flags, void *userdata)
{
    return splice(iopipe->pipefd[0], offin, *iofd, offout, len, flags);
}

class SyncSessionLoop : public FuseSessionLoop {
public:
    int init(const loop_args &args) {
        args_ = args;
        set_fd();
        max_workers_ = args_.max_threads;
        if (max_workers_ <= 0)
            max_workers_ = 32;

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
        : se_(se),
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
	        .splice_receive = photon::fs::custom_splice_receive,
	        .splice_send = photon::fs::custom_splice_send,
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
    loop_args args_;
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

        LOG_INFO("masterfd:`", masterfd,
                 " block fd `", blk_fd_,
                 "  nonblock fd: `", nonblk_fd_);
        return 0;
    }

    void *fuse_do_work(int id) {
        int res;
        auto wrk_id = id;
        struct fuse_buf fbuf = {
            .mem = NULL,
        };

        if (args_.force_splice_read)
            (*iopipe).setup();
        sem_.wait(1);
        idlers_.erase(wrk_id);
        while(!fuse_session_exited(se_)) {
            if (idlers_.size() == (workers_.size() - 1)) {
                *iofd = blk_fd_;
                if (args_.force_splice_read)
                    res = fuse_session_receive_splice(se_, &fbuf, &*iopipe, *iofd);
                else
                    res = fuse_session_receive_fd(se_, &fbuf, *iofd);
                if (res <= 0) {
                    break;
                }

                if (!idlers_.empty()) {
                    photon::thread_interrupt(workers_[*idlers_.begin()]);
                }

                fuse_session_process_buf(se_, &fbuf);
            } else {
                *iofd = nonblk_fd_;
                if (args_.force_splice_read)
                    res = fuse_session_receive_splice(se_, &fbuf, &*iopipe, *iofd);
                else
                    res = fuse_session_receive_fd(se_, &fbuf, *iofd);
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

        if (args_.force_splice_read)
            (*iopipe).setdown();
        for (const auto& wrk : idlers_) {
            photon::thread_interrupt(workers_[wrk]);
        }
        return nullptr;
    }
};

FuseSessionLoop *new_sync_session_loop(struct fuse_session *se, loop_args args) {
    return NewObj<SyncSessionLoop>(se)->init(args);
}

int set_sync_custom_io(struct fuse_session *se) {
    return SyncSessionLoop::set_custom_io(se);
}

static thread_local CascadingEventEngine *IOengine = nullptr;
static ssize_t custom_iou_writev(int fd, struct iovec *iov, int count, void *userdata);
static ssize_t custom_iou_read(int fd, void *buf, size_t len, void *userdata);

class IouringSessionLoop : public FuseSessionLoop {
public:
    explicit IouringSessionLoop(struct fuse_session *se)
        : se_(se),
          max_workers_(32),
          poller_(nullptr) {

        iouring_args args;
        args.eager_submit = true;
        io_engine_ = new_iouring_cascading_engine(args);
        IOengine = io_engine_;
        io_fd = -1;
    }

    ~IouringSessionLoop() {
        if (se_) fuse_session_exit(se_);
        wait_all_fini();

        if (io_fd != -1)
            close(io_fd);

        delete io_engine_;
    }

    int init(const loop_args &args) {
        args_ = args;
        set_fd();
        max_workers_ = args.max_threads;
        if (max_workers_ <= 0)
            max_workers_ = 32;
        for (int i = 0; i < max_workers_; ++i) {
            auto th = photon::thread_create11(
                &IouringSessionLoop::fuse_do_work, this);
            photon::thread_enable_join(th);
            workers_.emplace_back(th);
            photon::thread_yield_to(th);
        }

        return 0;
    }

    void run() {
        sem_.signal(max_workers_);
        poller_ = photon::thread_create11(&IouringSessionLoop::fuse_do_poll, this);
        photon::thread_enable_join(poller_);
        wait_all_fini();
    }

    static int set_custom_io(struct fuse_session *se) {
        const struct fuse_custom_io custom_io = {
            .writev = photon::fs::custom_iou_writev,
            .read = photon::fs::custom_iou_read,
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

    static thread_local int io_fd;

private:
    loop_args args_;
    struct fuse_session *se_;
    int max_workers_;
    std::vector<photon::thread *> workers_;
    photon::thread * poller_;
    photon::semaphore sem_;
    CascadingEventEngine *io_engine_;

    void wait_all_fini() {
        if (poller_) {
            photon::thread_interrupt(poller_);
            photon::thread_join((photon::join_handle *)poller_);
            poller_ = nullptr;
        }

        // Since the poller exits after detecting that the session has exited,
        // and some workers may still be in a permanent sleep state, we interrupt
        // all workers to break any potential long-term sleep.
        while (!workers_.empty()) {
            auto th = workers_.back();
            photon::thread_interrupt(th, EOK);
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
        io_fd = clonefd;
        return 0;
    }

    void *fuse_do_work() {
        struct fuse_buf fbuf = {
            .mem = NULL,
        };

        if (args_.force_splice_read)
            (*iopipe).setup();
        sem_.wait(1);
        while (!fuse_session_exited(se_)) {
            int res;
            if (!args_.force_splice_read)
                res = fuse_session_receive_buf(se_, &fbuf);
            else
                res = fuse_session_receive_uring_splice(
                          se_, &fbuf, io_fd, &*iopipe, io_engine_);
            if (res == -EINTR)
                continue;
            if (res <= 0)  {// Returned 0 indeicate that session has exited
                if (res < 0) {
                    fuse_session_exit(se_);
                }
                break;
            }
            fuse_session_process_buf(se_, &fbuf);
        }

        if (args_.force_splice_read)
            (*iopipe).setdown();
       return NULL;
    }

    void *fuse_do_poll() {
        while (!fuse_session_exited(se_)) {
            ssize_t ret = io_engine_->wait_for_events(nullptr, 0, -1);
            (void) ret;
        }

        return nullptr;
    }
};

thread_local int IouringSessionLoop::io_fd = -1;

ssize_t custom_iou_writev(int fd, struct iovec *iov, int count, void *userdata)
{
    (void)userdata;

    return writev(IouringSessionLoop::io_fd, iov, count);
}

ssize_t custom_iou_read(int fd, void *buf, size_t len, void *userdata)
{
    (void)userdata;

    off_t nonoff = -1;
    uint64_t flags = ((uint64_t)IOSQE_ASYNC) << 32;
    ssize_t ret =iouring_pread(IouringSessionLoop::io_fd, buf, len, nonoff, flags, -1, IOengine);
    return ret;
}

FuseSessionLoop *new_iouring_session_loop(struct fuse_session *se, loop_args args) {
    return NewObj<IouringSessionLoop>(se)->init(args);
}

int set_iouring_custom_io(struct fuse_session *se) {
    return IouringSessionLoop::set_custom_io(se);
}

#endif

}  // namespace fs
}  // namespace photon
