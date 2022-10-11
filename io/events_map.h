#pragma once

namespace photon {

// a helper class to translate events into underlay representation
#ifdef __APPLE__
typedef int EVENT_TYPE;  // On macOS, event_type are negative integer.
#else
typedef uint32_t EVENT_TYPE;
#endif
template<EVENT_TYPE UNDERLAY_EVENT_READ_,
        EVENT_TYPE UNDERLAY_EVENT_WRITE_,
        EVENT_TYPE UNDERLAY_EVENT_ERROR_>
struct EventsMap {
    const static EVENT_TYPE UNDERLAY_EVENT_READ = UNDERLAY_EVENT_READ_;
    const static EVENT_TYPE UNDERLAY_EVENT_WRITE = UNDERLAY_EVENT_WRITE_;
    const static EVENT_TYPE UNDERLAY_EVENT_ERROR = UNDERLAY_EVENT_ERROR_;
    static_assert(UNDERLAY_EVENT_READ != UNDERLAY_EVENT_WRITE, "...");
    static_assert(UNDERLAY_EVENT_READ != UNDERLAY_EVENT_ERROR, "...");
    static_assert(UNDERLAY_EVENT_ERROR != UNDERLAY_EVENT_WRITE, "...");
    static_assert(UNDERLAY_EVENT_READ, "...");
    static_assert(UNDERLAY_EVENT_WRITE, "...");
    static_assert(UNDERLAY_EVENT_ERROR, "...");

    EVENT_TYPE ev_read, ev_write, ev_error;

    EventsMap(EVENT_TYPE event_read, EVENT_TYPE event_write, EVENT_TYPE event_error) {
        ev_read = event_read;
        ev_write = event_write;
        ev_error = event_error;
        assert(ev_read);
        assert(ev_write);
        assert(ev_error);
        assert(ev_read != ev_write);
        assert(ev_read != ev_error);
        assert(ev_error != ev_write);
    }

    EVENT_TYPE translate_bitwisely(EVENT_TYPE events) const {
        EVENT_TYPE ret = 0;
        if (events & ev_read)
            ret |= UNDERLAY_EVENT_READ;
        if (events & ev_write)
            ret |= UNDERLAY_EVENT_WRITE;
        if (events & ev_error)
            ret |= UNDERLAY_EVENT_ERROR;
        return ret;
    }

    EVENT_TYPE translate_byval(EVENT_TYPE event) const {
        if (event == ev_read)
            return UNDERLAY_EVENT_READ;
        if (event == ev_write)
            return UNDERLAY_EVENT_WRITE;
        if (event == ev_error)
            return UNDERLAY_EVENT_ERROR;
    }
};

}
