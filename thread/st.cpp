#include "st.h"
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread-key.h>
#include <photon/io/fd-events.h>
#include <photon/net/basic_socket.h>
#include <photon/common/iovector.h>
#include <photon/common/alog.h>
#include <photon/common/timeout.h>

static int _eventsys = 0;
int st_get_eventsys(void) {
    return _eventsys;
}

int st_set_eventsys(int eventsys) {
    auto es = (uint32_t)eventsys;
    if (es > ST_EVENTSYS_IOURING)
        LOG_ERROR_RETURN(EINVAL, -1, "unknown eventsys ", eventsys);
    _eventsys = es;
    return 0;
}

int st_init(void) {
    auto engine = photon::INIT_EVENT_DEFAULT;
#if defined(__linux__)
    if (_eventsys == ST_EVENTSYS_IOURING) {
        engine ^= photon::INIT_EVENT_EPOLL;
    }
#endif
    return photon::init(engine, 0, {
        .libaio_queue_depth = 0,
        .use_pooled_stack_allocator = true,
        .bypass_threadpool = true,
    });
}

int st_getfdlimit(void) {
    struct rlimit rlim;
    if (getrlimit(RLIMIT_NOFILE, &rlim) < 0)
        LOG_ERRNO_RETURN(0, -1, "failed to getrlimit()");
    return rlim.rlim_max;
}

const char *st_get_eventsys_name(void) {
    return "event_sys_name";
}

// st_switch_cb_t st_set_switch_in_cb(st_switch_cb_t cb);
// st_switch_cb_t st_set_switch_out_cb(st_switch_cb_t cb);

st_thread_t st_thread_create(void *(*start)(void *arg), void *arg,
                             int joinable, int stack_size) {
    if (stack_size == 0)
        stack_size = photon::DEFAULT_STACK_SIZE;
    auto th = photon::thread_create(start, arg, stack_size);
    if (joinable) photon::thread_enable_join(th);
    return th;
}

void st_thread_exit(void *retval) {
    photon::thread_exit(retval);
}

int st_thread_join(st_thread_t thread, void **retvalp) {
    auto retval = photon::thread_join((photon::join_handle*)thread);
    if (retvalp) *retvalp = retval;
    return 0;
}

st_thread_t st_thread_self(void) {
    return photon::CURRENT;
}

void st_thread_interrupt(st_thread_t th) {
    thread_interrupt((photon::thread*)th);
}

int st_sleep(int secs) {
    return photon::thread_sleep(secs);
}

int st_usleep(st_utime_t usecs) {
    return photon::thread_usleep(usecs);
}

int st_randomize_stacks(int on) {
    return 0;
}

int st_key_create(int *keyp, void (*destructor)(void *)) {
    photon::thread_key_t key;
    auto ret = photon::thread_key_create(&key, destructor);
    if (ret < 0)
        LOG_ERRNO_RETURN(0, -1, "failed to thread_key_create()");
    if (key > INT_MAX) {
        photon::thread_key_delete(key);
        LOG_ERROR_RETURN(EOVERFLOW, -1, "thread key space overflow");
    }
    *keyp = key;
    return 0;
}

int st_key_getlimit(void) {
    return photon::THREAD_KEYS_MAX;
}

int st_thread_setspecific(int key, void *value) {
    photon::thread_key_t k = key;
    return photon::thread_setspecific(k, value);
}

void *st_thread_getspecific(int key) {
    photon::thread_key_t k = key;
    return photon::thread_getspecific(k);
}

// Synchronization
st_cond_t st_cond_new(void) {
    return new photon::condition_variable;
}

int st_cond_destroy(st_cond_t cvar) {
    delete (photon::condition_variable*)cvar;
    return 0;
}

int st_cond_wait(st_cond_t cvar) {
    return st_cond_timedwait(cvar, -1UL);
}

int st_cond_timedwait(st_cond_t cvar, st_utime_t timeout) {
    auto cv = (photon::condition_variable*)cvar;
    return cv->wait_no_lock(timeout);
}

int st_cond_signal(st_cond_t cvar) {
    auto cv = (photon::condition_variable*)cvar;
    return cv->signal(), 0;
}

int st_cond_broadcast(st_cond_t cvar) {
    auto cv = (photon::condition_variable*)cvar;
    return cv->broadcast(), 0;
}

st_mutex_t st_mutex_new(void) {
    return new photon::mutex;
}

int st_mutex_destroy(st_mutex_t lock) {
    delete (photon::mutex*) lock;
    return 0;
}

int st_mutex_lock(st_mutex_t lock) {
    auto m = (photon::mutex*) lock;
    return m->lock();
}

int st_mutex_trylock(st_mutex_t lock) {
    auto m = (photon::mutex*) lock;
    return m->try_lock();
}

int st_mutex_unlock(st_mutex_t lock) {
    auto m = (photon::mutex*) lock;
    return m->unlock(), 0;
}

time_t st_time(void) {
    return photon::now / 1000 /1000;
}

st_utime_t st_utime(void) {
    return photon::now;
}

int st_set_utime_function(st_utime_t (*func)(void)) {
    return 0;
}

int st_timecache_set(int on) {
    return 0;
}

struct netfd {
    int fd;
    void* specific = nullptr;
    void (*destructor)(void *);
    netfd(int fd) : fd(fd) { }
    netfd(int fd, bool) : fd(fd) {
        photon::net::set_fd_nonblocking(fd);
    }
    ~netfd() {
        if (specific && destructor)
            destructor(specific);
        if (fd >= 0)
            ::close(fd);
    }
};

inline int getfd(st_netfd_t fd) {
    return static_cast<netfd*>(fd)->fd;
}

// I/O Functions
st_netfd_t st_netfd_open(int osfd) {
    return new netfd(osfd);
}

st_netfd_t st_netfd_open_socket(int osfd) {
    return new netfd(osfd, true);
}

void st_netfd_free(st_netfd_t fd) {
    delete (netfd*)fd;
}

int st_netfd_close(st_netfd_t fd) {
    return ::close(getfd(fd));
}

int st_netfd_fileno(st_netfd_t fd) {
    return getfd(fd);
}

void st_netfd_setspecific(st_netfd_t fd, void *value, void (*destructor)(void *)) {
    auto _fd = (netfd*)fd;
    _fd->specific = value;
    _fd->destructor = destructor;
}

void *st_netfd_getspecific(st_netfd_t fd) {
    auto _fd = (netfd*)fd;
    return _fd->specific;
}

// On some platforms (e.g., Solaris 2.5 and possibly other SVR4 implementations)
// accept(3) calls from different processes on the same listening socket (see
// bind(3), listen(3)) must be serialized. This function causes all subsequent
// accept(3) calls made by st_accept() on the specified file descriptor object
// to be serialized.
int st_netfd_serialize_accept(st_netfd_t fd) {
    return 0;   // we do not support thoses platforms
}

inline uint32_t to_photon_events(int poll_event) {
    uint32_t events = 0;
    if (poll_event & POLLIN)  events |= photon::EVENT_READ;
    if (poll_event & POLLOUT) events |= photon::EVENT_WRITE;
    if (poll_event & POLLPRI) events |= photon::EVENT_ERROR;
    return events;
}

int st_netfd_poll(st_netfd_t fd, int how, st_utime_t timeout) {
    return photon::get_vcpu()->master_event_engine->
        wait_for_fd(getfd(fd), to_photon_events(how), timeout);
}

st_netfd_t st_accept(st_netfd_t fd, struct sockaddr *addr, int *addrlen, st_utime_t timeout) {
    static_assert(sizeof(socklen_t) == sizeof(int), "...");
    auto connection = photon::net::accept(
        getfd(fd), addr, (socklen_t*)addrlen, timeout);
    if (connection < 0)
        LOG_ERRNO_RETURN(0, nullptr, "failed to accept new connection");
    return st_netfd_open_socket(connection);
}

int st_connect(st_netfd_t fd, const struct sockaddr *addr, int addrlen, st_utime_t timeout) {
    return photon::net::connect(getfd(fd), addr, addrlen, timeout);
}

ssize_t st_read(st_netfd_t fd, void *buf, size_t nbyte, st_utime_t timeout) {
    return photon::net::read(getfd(fd), buf, nbyte, timeout);
}

ssize_t st_read_fully(st_netfd_t fd, void *buf, size_t nbyte, st_utime_t timeout) {
    return photon::net::read_n(getfd(fd), buf, nbyte, timeout);
}

int st_read_resid(st_netfd_t fd, void *buf, size_t *resid, st_utime_t timeout) {
    auto ret = photon::net::read_n(getfd(fd), buf, *resid, timeout);
    if (ret > 0) *resid -= ret;
    return ret;
}

ssize_t st_readv(st_netfd_t fd, const struct iovec *iov, int iov_size, st_utime_t timeout) {
    return photon::net::readv(getfd(fd), iov, iov_size, timeout);
}

int st_readv_resid(st_netfd_t fd, struct iovec **iov, int *iov_size, st_utime_t timeout) {
    if (unlikely(!iov || !*iov || !iov_size || *iov_size <= 0))
        LOG_ERROR_RETURN(EINVAL, -1, "invalid arguments");
    photon::Timeout tmo(timeout);
    iovector_view v(*iov, *iov_size);
    auto ret = DOIO_LOOP(photon::net::readv(getfd(fd), v.iov, v.iovcnt, tmo),
                         photon::net::BufStepV(v));
    *iov = v.iov;
    *iov_size = v.iovcnt;
    return ret;
}

ssize_t st_write(st_netfd_t fd, const void *buf, size_t nbyte, st_utime_t timeout) {
    return photon::net::write(getfd(fd), buf, nbyte, timeout);
}

int st_write_resid(st_netfd_t fd, const void *buf, size_t *resid, st_utime_t timeout) {
    auto ret = photon::net::write_n(getfd(fd), buf, *resid, timeout);
    if (ret > 0) *resid -= ret;
    return ret;
}

ssize_t st_writev(st_netfd_t fd, const struct iovec *iov, int iov_size, st_utime_t timeout) {
    return photon::net::writev(getfd(fd), iov, iov_size, timeout);
}

int st_writev_resid(st_netfd_t fd, struct iovec **iov, int *iov_size, st_utime_t timeout) {
    if (unlikely(!iov || !*iov || !iov_size || *iov_size <= 0))
        LOG_ERROR_RETURN(EINVAL, -1, "invalid arguments");
    photon::Timeout tmo(timeout);
    iovector_view v(*iov, *iov_size);
    // TODO:: this implementation of DOIO_LOOP incurs an extra wait_for_fd()
    // in every iteration, should fix it.
    auto ret = DOIO_LOOP(photon::net::writev(getfd(fd), v.iov, v.iovcnt, tmo),
                         photon::net::BufStepV(v));
    *iov = v.iov;
    *iov_size = v.iovcnt;
    return ret;
}

using photon::net::doio_once;
using photon::net::doio_loop;

int st_recvfrom(st_netfd_t fd, void *buf, int len, struct sockaddr *addr, int *addrlen, st_utime_t timeout) {
    iovec iov{buf, (size_t)len};
    struct msghdr hdr {
        .msg_name = (void*)addr,
        .msg_namelen = addrlen ? (socklen_t)*addrlen : 0,
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = nullptr,
        .msg_controllen = 0,
        .msg_flags = 0,
    };
    auto ret = st_recvmsg(fd, &hdr, 0, timeout);
    if (addrlen) *addrlen = hdr.msg_namelen;
    return ret;
}

int st_sendto(st_netfd_t fd, const void *buf, int len, struct sockaddr *addr, int addrlen, st_utime_t timeout) {
    iovec iov{(void*)buf, (size_t)len};
    struct msghdr hdr {
        .msg_name = (void*)addr,
        .msg_namelen = (socklen_t)addrlen,
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = nullptr,
        .msg_controllen = 0,
        .msg_flags = 0,
    };
    return st_sendmsg(fd, &hdr, 0, timeout);
}

int st_recvmsg(st_netfd_t fd, struct msghdr *msg, int flags, st_utime_t timeout) {
    return DOIO_ONCE(::recvmsg(getfd(fd), msg, flags | MSG_DONTWAIT),
               photon::wait_for_fd_readable(getfd(fd), timeout));
}

int st_sendmsg(st_netfd_t fd, const struct msghdr *msg, int flags, st_utime_t timeout) {
    return DOIO_ONCE(::sendmsg(getfd(fd), msg, flags | MSG_DONTWAIT | MSG_NOSIGNAL),
               photon::wait_for_fd_writable(getfd(fd), timeout));
}

st_netfd_t st_open(const char *path, int oflags, mode_t mode) {
    int fd = ::open(path, oflags, mode);
    if (fd < 0)
        LOG_ERRNO_RETURN(0, nullptr, "failed to open(`, `, `) file", path, oflags, mode);
    return new netfd(fd);
}

int st_poll(struct pollfd *pds, int npds, st_utime_t timeout) {
    if (!pds || !npds) return 0;
    auto eng = photon::new_default_cascading_engine();
    DEFER(delete eng);
    for (int i = 0; i < npds; ++i) {
        auto& p = pds[i];
        if (p.fd < 0)
            LOG_ERROR_RETURN(EINVAL, -1, "invalid fd ", p.fd);
        auto events = to_photon_events(p.events);
        eng->add_interest({p.fd, events, (void*)(int64_t)i});
    }
    constexpr int MAX = 32;
    void* data[MAX];
    int n = 0;
again:
    auto ret = eng->wait_for_events(data, MAX, timeout);
    if (ret < 0)
        LOG_ERRNO_RETURN(0, -1, "failed to wait_for_events() via default cascading engine");
    n += ret;
    for (ssize_t i = 0; i < ret; ++i) {
        auto j = (uint64_t)data[i];
        if (j >= (uint64_t)npds)
            LOG_ERROR_RETURN(EOVERFLOW, -1, "reap event data overflow");
        pds[j].revents = pds[j].events;
    }
    if (ret == MAX)
        goto again;
    return n;
}
