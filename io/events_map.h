#pragma once

#include <photon/io/fd-events.h>

namespace photon {

using EVENT_TYPE = int;

template <EVENT_TYPE EV_READ_, EVENT_TYPE EV_WRITE_, EVENT_TYPE EV_ERROR_>
struct EVGroupBase {
    static constexpr EVENT_TYPE EV_READ = EV_READ_;
    static constexpr EVENT_TYPE EV_WRITE = EV_WRITE_;
    static constexpr EVENT_TYPE EV_ERROR = EV_ERROR_;

    static_assert(EV_READ != EV_WRITE, "...");
    static_assert(EV_READ != EV_ERROR, "...");
    static_assert(EV_ERROR != EV_WRITE, "...");
    static_assert(EV_READ, "...");
    static_assert(EV_WRITE, "...");
    static_assert(EV_ERROR, "...");
};

struct EVUBase {};
struct EVKBase {};

template <EVENT_TYPE EV_READ_, EVENT_TYPE EV_WRITE_, EVENT_TYPE EV_ERROR_>
struct EVUnderlay : EVGroupBase<EV_READ_, EV_WRITE_, EV_ERROR_>, EVUBase {};

template <EVENT_TYPE EV_READ_, EVENT_TYPE EV_WRITE_, EVENT_TYPE EV_ERROR_>
struct EVKey : EVGroupBase<EV_READ_, EV_WRITE_, EV_ERROR_>, EVKBase {};

template <typename EV_UNDERLAY,
          typename EV_KEY = EVKey<EVENT_READ, EVENT_WRITE, EVENT_ERROR>>
struct EventsMap {
    static_assert(std::is_base_of<EVUBase, EV_UNDERLAY>::value,
                  "EV_UNDERLAY should be type of EVUnderlay");
    static_assert(std::is_base_of<EVKBase, EV_KEY>::value,
                  "EV_KEY should be type of EVKey");

    static constexpr EVENT_TYPE UNDERLAY_EVENT_READ = EV_UNDERLAY::EV_READ;
    static constexpr EVENT_TYPE UNDERLAY_EVENT_WRITE = EV_UNDERLAY::EV_WRITE;
    static constexpr EVENT_TYPE UNDERLAY_EVENT_ERROR = EV_UNDERLAY::EV_ERROR;

    EVENT_TYPE translate_bitwisely(EVENT_TYPE events) const {
        EVENT_TYPE ret = 0;
        if (events & EV_KEY::EV_READ) ret |= EV_UNDERLAY::EV_READ;
        if (events & EV_KEY::EV_WRITE) ret |= EV_UNDERLAY::EV_WRITE;
        if (events & EV_KEY::EV_ERROR) ret |= EV_UNDERLAY::EV_ERROR;
        return ret;
    }

    EVENT_TYPE translate_byval(EVENT_TYPE event) const {
        if (event == EV_KEY::EV_READ) return EV_UNDERLAY::EV_READ;
        if (event == EV_KEY::EV_WRITE) return EV_UNDERLAY::EV_WRITE;
        if (event == EV_KEY::EV_ERROR) return EV_UNDERLAY::EV_ERROR;
    }
};

}  // namespace photon
