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

#pragma once
#include <utility>

#include <photon/common/alog.h>
#include <photon/common/utility.h>

#define AU_FILEOP(pathname, offset, size)   \
    make_named_value("pathname", pathname), \
        make_named_value("offset", offset), make_named_value("size", size)

#define AU_SOCKETOP(ep) make_named_value("endpoint", ep)

#ifndef DISABLE_AUDIT
#define SCOPE_AUDIT(...)                                                       \
    auto _CONCAT(__audit_start_time__, __LINE__) = photon::now;                \
    DEFER(LOG_AUDIT(__VA_ARGS__,                                               \
                    make_named_value(                                          \
                        "latency", photon::now - _CONCAT(__audit_start_time__, \
                                                         __LINE__))));

#define SCOPE_AUDIT_THRESHOLD(threshold, ...)                                 \
    auto _CONCAT(__audit_start_time__, __LINE__) = photon::now;               \
    DEFER({                                                                   \
        auto latency = photon::now - _CONCAT(__audit_start_time__, __LINE__); \
        if (latency >= (threshold)) {                                         \
            LOG_AUDIT(__VA_ARGS__, make_named_value("latency", latency));     \
        }                                                                     \
    });

#else
#define SCOPE_AUDIT(...)
#define SCOPE_AUDIT_THRESHOLD(...)
#endif
