#include "fd-events.h"
#include <photon/common/alog.h>

namespace photon {

int EventEngine::wait_for_fd(int fd, uint32_t interests, Timeout timeout) {
    if (fd < 0 || !interests)
        LOG_ERROR_RETURN(EINVAL, -1, "invalid fd or event interest");

    Event event{fd, interests, CURRENT};
    if (add_interest(event) < 0)
        LOG_ERROR_RETURN(0, -1, "failed to add event interest");

    int ret = thread_usleep(timeout);

    ERRNO err;
    if (likely(ret == -1 && err.no == EOK)) {
        return 0;       // Event arrived and cleared
    }
    rm_interest(event); // errno may be changed in rm_interest()
    err.set((ret == 0) ? ETIMEDOUT : err.no);
    return -1;
}

int EventEngine::wait_for_events(void** data, size_t count, Timeout timeout) {
    errno = ENOSYS;
    return -1;
}

ssize_t EventEngine::wait_and_fire_events(uint64_t timeout) {
    errno = ENOSYS;
    return -1;
}

void CascadingWaiter::rm_interest(int fd) {
    if (_master) {
        _master->rm_interest({fd, 0, 0});
        _master = nullptr;
    }
}

int CascadingWaiter::wait(int fd, Timeout timeout) {
    auto m = get_vcpu()->master_event_engine;
    if (unlikely(_master != m)) {
        if (_master)
            _master->rm_interest({fd, 0, 0});
        _master = m;
        m->add_interest({fd, 0, 0});
    }

    _waiting_th = CURRENT;
    int ret = m->wait_for_fd(fd, EVENT_READ, timeout);  // not using ONE_SHOT
    _waiting_th = nullptr;
    return ret;
}

void CascadingWaiter::cancel() {
    if (_waiting_th)
        thread_interrupt(_waiting_th);
}

}
