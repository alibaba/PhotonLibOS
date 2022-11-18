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

#include "iouring-wrapper.h"

#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <cstdint>
#include <limits>
#include <atomic>
#include <unordered_map>

#include <liburing.h>
#include <photon/common/alog.h>
#include <photon/thread/thread11.h>
#include <photon/io/fd-events.h>
#include "events_map.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define container_of(ptr, type, member) \
    ((type*)((char*)static_cast<const decltype(((type*)0)->member)*>(ptr) - offsetof(type,member)))

namespace photon {

constexpr static EventsMap<EVUnderlay<POLLIN | POLLRDHUP, POLLOUT, POLLERR>>
    evmap;

class iouringEngine : public MasterEventEngine, public CascadingEventEngine {
public:
    explicit iouringEngine(bool master) : m_master(master) {}

    ~iouringEngine() {
        LOG_INFO("Finish event engine: iouring ", VALUE(m_master));
        if (m_cancel_poller != nullptr) {
            m_cancel_poller_running = false;
            thread_interrupt(m_cancel_poller);
            thread_join((join_handle*) m_cancel_poller);
        }
        if (m_cancel_fd >= 0) {
            close(m_cancel_fd);
        }
        if (m_cascading_event_fd >= 0) {
            if (io_uring_unregister_eventfd(m_ring) != 0) {
                LOG_ERROR("iouring: failed to unregister cascading event fd");
            }
            close(m_cascading_event_fd);
        }
        if (m_ring != nullptr) {
            io_uring_queue_exit(m_ring);
        }
        delete m_ring;
        m_ring = nullptr;
    }

    int init() {
        rlimit resource_limit{.rlim_cur = RLIM_INFINITY, .rlim_max = RLIM_INFINITY};
        if (setrlimit(RLIMIT_MEMLOCK, &resource_limit) != 0) {
            LOG_WARN("iouring: current user has no permission to set unlimited RLIMIT_MEMLOCK, change to root?");
        }
        set_submit_wait_function();

        m_ring = new io_uring{};
        io_uring_params params{};
        int ret = io_uring_queue_init_params(QUEUE_DEPTH, m_ring, &params);
        if (ret != 0) {
            // reset m_ring so that the destructor won't do duplicate munmap cleanup (io_uring_queue_exit)
            delete m_ring;
            m_ring = nullptr;
            LOG_ERRNO_RETURN(0, -1, "iouring: failed to init queue");
        }

        // Check feature supported
        for (auto i : REQUIRED_FEATURES) {
            if (!(params.features & i)) {
                LOG_ERROR_RETURN(0, -1, "iouring: required feature not supported");
            }
        }

        // Check opcode supported
        auto probe = io_uring_get_probe_ring(m_ring);
        if (probe == nullptr) {
            LOG_ERROR_RETURN(0, -1, "iouring: failed to get probe");
        }
        DEFER(free(probe));
        if (!io_uring_opcode_supported(probe, IORING_OP_PROVIDE_BUFFERS) ||
            !io_uring_opcode_supported(probe, IORING_OP_ASYNC_CANCEL)) {
            LOG_ERROR_RETURN(0, -1, "iouring: some opcodes are not supported");
        }

        // An additional feature only available since 5.18. Doesn't have to succeed
        ret = io_uring_register_ring_fd(m_ring);
        if (ret < 0 && ret != -EINVAL) {
            LOG_ERROR_RETURN(EINVAL, -1, "iouring: unable to register ring fd", ret);
        }

        if (m_master) {
            // Setup a cancel poller to watch on master engine
            m_cancel_fd = eventfd(0, EFD_CLOEXEC);
            if (m_cancel_fd < 0) {
                LOG_ERRNO_RETURN(0, -1, "iouring: failed to create eventfd");
            }
            m_cancel_poller = thread_create11(64 * 1024, &iouringEngine::run_cancel_poller, this);
            thread_enable_join(m_cancel_poller);

        } else {
            // Register an event fd for cascading engine
            m_cascading_event_fd = eventfd(0, EFD_CLOEXEC);
            if (m_cascading_event_fd < 0) {
                LOG_ERRNO_RETURN(0, -1, "iouring: failed to create cascading event fd");
            }
            if (io_uring_register_eventfd(m_ring, m_cascading_event_fd) != 0) {
                LOG_ERRNO_RETURN(0, -1, "iouring: failed to register cascading event fd");
            }
        }
        return 0;
    }

    /**
     * @brief Get a SQE from ring, prepare IO, and wait for completion. Note all the SQEs are batch submitted
     *     later in the `wait_and_fire_events`.
     * @param timeout Timeout in usec. It could be 0 (immediate cancel), and -1 (most efficient way, no linked SQE).
     *     Note that the cancelling has no guarantee to succeed, it's just an attempt.
     * @retval Non negative integers for success, -1 for failure. If failed with timeout, errno will
     *     be set to ETIMEDOUT. If failed because of external interruption, errno will also be set accordingly.
     */
    template<typename Prep, typename... Args>
    int32_t async_io(Prep prep, uint64_t timeout, Args... args) {
        io_uring_sqe* sqe = io_uring_get_sqe(m_ring);
        if (sqe == nullptr) {
            LOG_ERROR_RETURN(EBUSY, -1, "iouring: submission queue is full");
        }

        ioCtx io_ctx{photon::CURRENT, -1, false, false};
        prep(sqe, args...);
        io_uring_sqe_set_data(sqe, &io_ctx);

        ioCtx timer_ctx{photon::CURRENT, -1, true, false};
        __kernel_timespec ts{};
        if (timeout < std::numeric_limits<int64_t>::max()) {
            sqe->flags |= IOSQE_IO_LINK;
            usec_to_timespec(timeout, &ts);
            sqe = io_uring_get_sqe(m_ring);
            if (sqe == nullptr) {
                LOG_ERROR_RETURN(EBUSY, -1, "iouring: submission queue is full");
            }
            io_uring_prep_link_timeout(sqe, &ts, 0);
            io_uring_sqe_set_data(sqe, &timer_ctx);
        }

        photon::thread_sleep(-1);

        if (errno == EOK) {
            // Interrupted by `wait_and_fire_events`
            if (io_ctx.res < 0) {
                errno = -io_ctx.res;
                return -1;
            } else {
                // errno = 0;
                return io_ctx.res;
            }
        } else {
            // Interrupted by external user thread. Try to cancel the previous I/O
            ERRNO err_backup;
            sqe = io_uring_get_sqe(m_ring);
            if (sqe == nullptr) {
                LOG_ERROR_RETURN(EBUSY, -1, "iouring: submission queue is full");
            }
            ioCtx cancel_ctx{CURRENT, -1, true, false};
            io_uring_prep_cancel(sqe, &io_ctx, 0);
            io_uring_sqe_set_data(sqe, &cancel_ctx);
            photon::thread_sleep(-1);
            errno = err_backup.no;
            return -1;
        }
    }

    int wait_for_fd(int fd, uint32_t interests, uint64_t timeout) override {
        unsigned poll_mask = evmap.translate_bitwisely(interests);
        // The io_uring_prep_poll_add's return value is the same as poll(2)'s revents.
        int ret = async_io(&io_uring_prep_poll_add, timeout, fd, poll_mask);
        if (ret < 0) {
            return -1;
        }
        if (ret & POLLNVAL) {
            LOG_ERRNO_RETURN(EINVAL, -1, "iouring: poll invalid argument");
        }
        return 0;
    }

    int add_interest(Event e) override {
        io_uring_sqe* sqe = io_uring_get_sqe(m_ring);
        if (sqe == nullptr) {
            LOG_ERROR_RETURN(EBUSY, -1, "iouring: submission queue is full");
        }

        bool one_shot = e.interests & ONE_SHOT;
        fdInterest fd_interest{e.fd, (uint32_t)evmap.translate_bitwisely(e.interests)};
        ioCtx io_ctx{CURRENT, -1, false, true};
        eventCtx event_ctx{e, one_shot, io_ctx};
        auto pair = m_event_contexts.insert({fd_interest, event_ctx});
        if (!pair.second) {
            LOG_ERROR_RETURN(0, -1, "iouring: event has already been added");
        }

        if (one_shot) {
            io_uring_prep_poll_add(sqe, fd_interest.fd, fd_interest.interest);
        } else {
            io_uring_prep_poll_multishot(sqe, fd_interest.fd, fd_interest.interest);
        }
        io_uring_sqe_set_data(sqe, &pair.first->second.io_ctx);
        return 0;
    }

    int rm_interest(Event e) override {
        io_uring_sqe* sqe = io_uring_get_sqe(m_ring);
        if (sqe == nullptr) {
            LOG_ERROR_RETURN(EBUSY, -1, "iouring: submission queue is full");
        }

        fdInterest fd_interest{e.fd, (uint32_t)evmap.translate_bitwisely(e.interests)};
        auto iter = m_event_contexts.find(fd_interest);
        if (iter == m_event_contexts.end()) {
            LOG_ERROR_RETURN(0, -1, "iouring: event is either non-existent or one-shot finished");
        }

        io_uring_prep_poll_remove(sqe, (__u64) &iter->second.io_ctx);
        io_uring_sqe_set_data(sqe, nullptr);
        return 0;
    }

    ssize_t wait_for_events(void** data, size_t count, uint64_t timeout = -1) override {
        // Use master engine to wait for self event fd
        int ret = get_vcpu()->master_event_engine->wait_for_fd_readable(m_cascading_event_fd, timeout);
        if (ret < 0) {
            return errno == ETIMEDOUT ? 0 : -1;
        }
        uint64_t value = 0;
        if (eventfd_read(m_cascading_event_fd, &value)) {
            LOG_ERROR("iouring: error reading cascading event fd, `", ERRNO());
        }

        // Reap events
        size_t num = 0;
        io_uring_cqe* cqe = nullptr;
        uint32_t head = 0;
        unsigned i = 0;
        io_uring_for_each_cqe(m_ring, head, cqe) {
            ++i;
            auto ctx = (ioCtx*) io_uring_cqe_get_data(cqe);
            if (!ctx) {
                // rm_interest didn't set user data
                continue;
            }
            ctx->res = cqe->res;
            if (cqe->flags & IORING_CQE_F_MORE && cqe->res & POLLERR) {
                LOG_ERROR_RETURN(0, -1, "iouring: multi-shot poll got POLLERR");
            }
            if (!ctx->is_event) {
                LOG_ERROR_RETURN(0, -1, "iouring: only cascading engine need to handle event. Must be a bug...")
            }
            eventCtx* event_ctx = container_of(ctx, eventCtx, io_ctx);
            fdInterest fd_interest{event_ctx->event.fd, (uint32_t)evmap.translate_bitwisely(event_ctx->event.interests)};
            if (ctx->res == -ECANCELED) {
                m_event_contexts.erase(fd_interest);
            } else if (event_ctx->one_shot) {
                data[num++] = event_ctx->event.data;
                m_event_contexts.erase(fd_interest);
            } else {
                data[num++] = event_ctx->event.data;
            }
            if (num >= count) {
                break;
            }
        }
        io_uring_cq_advance(m_ring, i);
        return num;
    }

    ssize_t wait_and_fire_events(uint64_t timeout = -1) {
        // Prepare own timeout
        __kernel_timespec ts{};
        if (timeout > std::numeric_limits<int64_t>::max()) {
            timeout = std::numeric_limits<int64_t>::max();
        }
        usec_to_timespec(timeout, &ts);

        io_uring_cqe* cqe = nullptr;
        if (m_submit_wait_func(m_ring, &ts, &cqe) != 0) {
            return -1;
        }

        uint32_t head = 0;
        unsigned i = 0;
        io_uring_for_each_cqe(m_ring, head, cqe) {
            i++;
            auto ctx = (ioCtx*) io_uring_cqe_get_data(cqe);
            if (!ctx) {
                // Own timeout doesn't have user data
                continue;
            }
            ctx->res = cqe->res;
            if (!ctx->is_canceller && ctx->res == -ECANCELED) {
                // An I/O was canceled because of:
                // 1. IORING_OP_LINK_TIMEOUT. Leave the interrupt job to the linked timer later.
                // 2. IORING_OP_POLL_REMOVE. The I/O is actually a polling.
                // 3. IORING_OP_ASYNC_CANCEL. This OP is the superset of case 2.
                ctx->res = -ETIMEDOUT;
                continue;
            }
            if (ctx->is_canceller && ctx->res == -ECANCELED) {
                // The linked timer itself is also a canceller. The reasons it got cancelled could be:
                // 1. I/O finished in time
                // 2. I/O was cancelled by IORING_OP_ASYNC_CANCEL
                continue;
            }
            photon::thread_interrupt(ctx->th_id, EOK);
        }

        io_uring_cq_advance(m_ring, i);
        return 0;
    }

    int cancel_wait() override {
        if (eventfd_write(m_cancel_fd, 1) != 0) {
            LOG_ERRNO_RETURN(0, -1, "iouring: write eventfd failed");
        }
        return 0;
    }

private:
    struct ioCtx {
        photon::thread* th_id;
        int32_t res;
        bool is_canceller;
        bool is_event;
    };

    struct eventCtx {
        Event event;
        bool one_shot;
        ioCtx io_ctx;
    };

    struct fdInterest {
        int fd;
        uint32_t interest;

        bool operator==(const fdInterest& fi) const {
            return fi.fd == fd && fi.interest == interest;
        }
    };

    struct fdInterestHasher {
        size_t operator()(const fdInterest& key) const {
            auto ptr = (uint64_t*) &key;
            return std::hash<uint64_t>()(*ptr);
        }
    };

    static int submit_wait_by_timer(io_uring* ring, __kernel_timespec* ts, io_uring_cqe** cqe) {
        io_uring_sqe* sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            LOG_ERROR_RETURN(EBUSY, -1, "iouring: submission queue is full");
        }
        io_uring_prep_timeout(sqe, ts, 1, 0);
        io_uring_sqe_set_data(sqe, nullptr);

        // Batch submit all SQEs
        int ret = io_uring_submit_and_wait(ring, 1);
        if (ret <= 0) {
            LOG_ERROR_RETURN(0, -1, "iouring: failed to submit io")
        }
        return 0;
    }

    static int submit_wait_by_api(io_uring* ring, __kernel_timespec* ts, io_uring_cqe** cqe) {
        // Batch submit all SQEs
        int ret = io_uring_submit_and_wait_timeout(ring, cqe, 1, ts, nullptr);
        if (ret < 0 && ret != -ETIME) {
            LOG_ERROR_RETURN(0, -1, "iouring: failed to submit io");
        }
        return 0;
    }

    using SubmitWaitFunc = int (*)(io_uring* ring, __kernel_timespec* ts, io_uring_cqe** cqe);
    static SubmitWaitFunc m_submit_wait_func;

    static void set_submit_wait_function() {
        // The submit_and_wait_timeout API is more efficient than setting up a timer and waiting for it.
        // But there is a kernel bug before 5.17, so choose appropriate function here.
        // See https://git.kernel.dk/cgit/linux-block/commit/?h=io_uring-5.17&id=228339662b398a59b3560cd571deb8b25b253c7e
        if (m_submit_wait_func)
            return;
        int result;
        if (kernel_version_compare("5.17", result) == 0 && result >= 0) {
            m_submit_wait_func = submit_wait_by_api;
        } else {
            m_submit_wait_func = submit_wait_by_timer;
        }
    }

    void run_cancel_poller() {
        while (m_cancel_poller_running) {
            uint32_t poll_mask = evmap.translate_bitwisely(EVENT_READ);
            int ret = async_io(&io_uring_prep_poll_add, -1, m_cancel_fd, poll_mask);
            if (ret < 0) {
                if (errno == EINTR) {
                    break;
                }
                LOG_ERROR("iouring: poll eventfd failed, `", ERRNO());
            }
            eventfd_t val;
            if (eventfd_read(m_cancel_fd, &val) != 0) {
                LOG_ERROR("iouring: read eventfd failed, `", ERRNO());
            }
        }
    }

    static void usec_to_timespec(int64_t usec, __kernel_timespec* ts) {
        int64_t usec_rounded_to_sec = usec / 1000000L * 1000000L;
        long long nsec = (usec - usec_rounded_to_sec) * 1000L;
        ts->tv_sec = usec_rounded_to_sec / 1000000L;
        ts->tv_nsec = nsec;
    }

    static constexpr const uint32_t REQUIRED_FEATURES[] = {
            IORING_FEAT_CUR_PERSONALITY, IORING_FEAT_NODROP,
            IORING_FEAT_FAST_POLL, IORING_FEAT_EXT_ARG,
            IORING_FEAT_RW_CUR_POS};
    static const int QUEUE_DEPTH = 16384;
    bool m_master;
    int m_cascading_event_fd = -1;
    io_uring* m_ring = nullptr;
    int m_cancel_fd = -1;
    thread* m_cancel_poller = nullptr;
    bool m_cancel_poller_running = true;
    std::unordered_map<fdInterest, eventCtx, fdInterestHasher> m_event_contexts;
};

constexpr const uint32_t iouringEngine::REQUIRED_FEATURES[];

iouringEngine::SubmitWaitFunc iouringEngine::m_submit_wait_func = nullptr;

ssize_t iouring_pread(int fd, void* buf, size_t count, off_t offset, uint64_t timeout) {
    auto ee = (iouringEngine*) get_vcpu()->master_event_engine;
    return ee->async_io(&io_uring_prep_read, timeout, fd, buf, count, offset);
}

ssize_t iouring_pwrite(int fd, const void* buf, size_t count, off_t offset, uint64_t timeout) {
    auto ee = (iouringEngine*) get_vcpu()->master_event_engine;
    return ee->async_io(&io_uring_prep_write, timeout, fd, buf, count, offset);
}

ssize_t iouring_preadv(int fd, const iovec* iov, int iovcnt, off_t offset, uint64_t timeout) {
    auto ee = (iouringEngine*) get_vcpu()->master_event_engine;
    return ee->async_io(&io_uring_prep_readv, timeout, fd, iov, iovcnt, offset);
}

ssize_t iouring_pwritev(int fd, const iovec* iov, int iovcnt, off_t offset, uint64_t timeout) {
    auto ee = (iouringEngine*) get_vcpu()->master_event_engine;
    return ee->async_io(&io_uring_prep_writev, timeout, fd, iov, iovcnt, offset);
}

ssize_t iouring_send(int fd, const void* buf, size_t len, int flags, uint64_t timeout) {
    auto ee = (iouringEngine*) get_vcpu()->master_event_engine;
    return ee->async_io(&io_uring_prep_send, timeout, fd, buf, len, flags);
}

ssize_t iouring_recv(int fd, void* buf, size_t len, int flags, uint64_t timeout) {
    auto ee = (iouringEngine*) get_vcpu()->master_event_engine;
    return ee->async_io(&io_uring_prep_recv, timeout, fd, buf, len, flags);
}

ssize_t iouring_sendmsg(int fd, const msghdr* msg, int flags, uint64_t timeout) {
    auto ee = (iouringEngine*) get_vcpu()->master_event_engine;
    return ee->async_io(&io_uring_prep_sendmsg, timeout, fd, msg, flags);
}

ssize_t iouring_recvmsg(int fd, msghdr* msg, int flags, uint64_t timeout) {
    auto ee = (iouringEngine*) get_vcpu()->master_event_engine;
    return ee->async_io(&io_uring_prep_recvmsg, timeout, fd, msg, flags);
}

int iouring_connect(int fd, const sockaddr* addr, socklen_t addrlen, uint64_t timeout) {
    auto ee = (iouringEngine*) get_vcpu()->master_event_engine;
    return ee->async_io(&io_uring_prep_connect, timeout, fd, addr, addrlen);
}

int iouring_accept(int fd, sockaddr* addr, socklen_t* addrlen, uint64_t timeout) {
    auto ee = (iouringEngine*) get_vcpu()->master_event_engine;
    return ee->async_io(&io_uring_prep_accept, timeout, fd, addr, addrlen, 0);
}

int iouring_fsync(int fd) {
    auto ee = (iouringEngine*) get_vcpu()->master_event_engine;
    return ee->async_io(&io_uring_prep_fsync, -1, fd, 0);
}

int iouring_fdatasync(int fd) {
    auto ee = (iouringEngine*) get_vcpu()->master_event_engine;
    return ee->async_io(&io_uring_prep_fsync, -1, fd, IORING_FSYNC_DATASYNC);
}

int iouring_open(const char* path, int flags, mode_t mode) {
    auto ee = (iouringEngine*) get_vcpu()->master_event_engine;
    return ee->async_io(&io_uring_prep_openat, -1, AT_FDCWD, path, flags, mode);
}

int iouring_mkdir(const char* path, mode_t mode) {
    auto ee = (iouringEngine*) get_vcpu()->master_event_engine;
    return ee->async_io(&io_uring_prep_mkdirat, -1, AT_FDCWD, path, mode);
}

int iouring_close(int fd) {
    auto ee = (iouringEngine*) get_vcpu()->master_event_engine;
    return ee->async_io(&io_uring_prep_close, -1, fd);
}

__attribute__((noinline))
static iouringEngine* new_iouring(bool is_master) {
    LOG_INFO("Init event engine: iouring ", VALUE(is_master));
    return NewObj<iouringEngine>(is_master) -> init();
}

MasterEventEngine* new_iouring_master_engine() {
    return new_iouring(true);
}

CascadingEventEngine* new_iouring_cascading_engine() {
    return new_iouring(false);
}

}
