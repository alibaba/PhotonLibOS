#include "fd-events.h"
#include <photon/common/alog.h>

namespace photon {

int EventEngine::wait_for_fd(int fd, uint32_t interests, uint64_t timeout) {
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

}
