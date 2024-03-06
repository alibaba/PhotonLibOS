#include "fd-events.h"
#include <photon/common/alog.h>

namespace photon {

int EventEngine::wait_for_fd(int fd, uint32_t interests, Timeout timeout) {
    Event event{fd, interests | ONE_SHOT, CURRENT};
    if (interests) {
        if (add_interest(event) < 0)
            LOG_ERROR_RETURN(0, -1, "failed to add event interest");
    }

    int ret = thread_usleep(timeout);

    ERRNO err;
    if (event.interests != ONE_SHOT) {
        rm_interest(event);
    }
    if (likely(ret == -1 && err.no == EOK)) {
        return 0;            // Event arrived and cleared
    }
    errno = (ret == 0) ? ETIMEDOUT : err.no;
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

int CascadingWaiter::wait(int fd, Timeout timeout) {
    _waiting_th = CURRENT;
    int ret = get_vcpu()->master_event_engine->
              wait_for_fd_readable(fd, timeout);
    _waiting_th = nullptr;
    return ret;
}

void CascadingWaiter::cancel() {
    if (_waiting_th)
        thread_interrupt(_waiting_th);
}

}
