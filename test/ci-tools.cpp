#include "ci-tools.h"
#include <cstdlib>
#include <cstring>
#include "../photon.h"

#if __linux__
uint64_t ci_ev_engine = photon::INIT_EVENT_EPOLL;
#else
uint64_t ci_ev_engine = photon::INIT_EVENT_KQUEUE;
#endif

uint64_t ci_ev_engine_with_signal = ci_ev_engine | photon::INIT_EVENT_SIGNAL;

void ci_parse_env() {
    const char* ev_engine = getenv("PHOTON_CI_EV_ENGINE");
    if (!ev_engine)
        return;
    if (strcmp(ev_engine, "epoll") == 0) {
        ci_ev_engine = photon::INIT_EVENT_EPOLL;
    } else if (strcmp(ev_engine, "io_uring") == 0) {
        ci_ev_engine = photon::INIT_EVENT_IOURING;
    } else if (strcmp(ev_engine, "kqueue") == 0) {
        ci_ev_engine = photon::INIT_EVENT_KQUEUE;
    }
    ci_ev_engine_with_signal = ci_ev_engine | photon::INIT_EVENT_SIGNAL;
}