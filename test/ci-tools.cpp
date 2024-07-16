// #include "ci-tools.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <assert.h>
#include <photon/common/estring.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/photon.h>
#include <pthread.h>


namespace photon {

static uint64_t _engine = 0;
static const char* _engine_name = nullptr;

__attribute__((constructor))
static void parse_env_eng() {
    _engine_name = getenv("PHOTON_CI_EV_ENGINE");
    if (!_engine_name)
        return;
    if (strcmp(_engine_name, "epoll") == 0) {
        _engine = photon::INIT_EVENT_EPOLL;
    } else if (strcmp(_engine_name, "io_uring") == 0) {
        _engine = photon::INIT_EVENT_IOURING;
    } else if (strcmp(_engine_name, "kqueue") == 0) {
        _engine = photon::INIT_EVENT_KQUEUE;
    } else if (strcmp(_engine_name, "epoll_ng") == 0) {
        _engine = photon::INIT_EVENT_EPOLL_NG;
    } else {
        printf("invalid event engine: %s\n", _engine_name);
        _engine_name = nullptr;
        exit(-1);
    }
}

static estring_view get_engine_name(uint64_t eng) {
    switch(eng) {
        case INIT_EVENT_EPOLL:      return "epoll";
        case INIT_EVENT_IOURING:    return "io_uring";
        case INIT_EVENT_KQUEUE:     return "kqueue";
        case INIT_EVENT_SELECT:     return "select";
        case INIT_EVENT_IOCP:       return "iocp";
        case INIT_EVENT_EPOLL_NG:   return "epoll_ng";
    }
    return {};
}

static estring get_engine_names(uint64_t engs) {
    estring names;
    if (!engs) {
        names = "none";
    } else {
        for (uint64_t i = 0; i < 64; ++i) {
            auto name = get_engine_name(engs & (1UL << i));
            if (name.size()) names.appends(name, ", ");
        }
        if (names.size() > 2)
            names.resize(names.size() - 2);
    }
    return names;
}

int __photon_init(uint64_t, uint64_t, const PhotonOptions&);

// this function is supposed to be linked statically to test
// cases, so as to override the real one in libphoton.so/dylib
int init(uint64_t event_engine, uint64_t io_engine, const PhotonOptions& options) {
    LOG_INFO("enter CI overriden photon::init()");
    const uint64_t all_engines = INIT_EVENT_EPOLL  |
            INIT_EVENT_IOURING | INIT_EVENT_KQUEUE |
            INIT_EVENT_SELECT  | INIT_EVENT_IOCP   |
            INIT_EVENT_EPOLL_NG;
    auto arg_specified = event_engine & all_engines;
    LOG_INFO("argument specified: ` (`)", get_engine_names(arg_specified), arg_specified);
    LOG_INFO("environment specified: '`'", _engine_name);
    if (_engine && arg_specified) {
        event_engine &= ~all_engines;
        event_engine |= _engine;
        LOG_INFO("event engine overridden to: ", get_engine_names(event_engine & all_engines));
    }
    return __photon_init(event_engine, io_engine, options);
}

bool is_using_default_engine() {
#ifdef __linux__
    const uint64_t default_eng = INIT_EVENT_EPOLL;
#else
    const uint64_t default_eng = INIT_EVENT_KQUEUE;
#endif
    return !_engine || _engine == default_eng;
}

void set_cpu_affinity(int i) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(i, &cpuset);
    pthread_setaffinity_np(pthread_self(),
                sizeof(cpu_set_t), &cpuset);
#endif
}

}
