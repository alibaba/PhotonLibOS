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

#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 317
#endif

#if FUSE_USE_VERSION >= 30
#include <fuse3/fuse.h>
#else
#include <fuse.h>
#endif

namespace photon {
namespace fs {

#define SHIFT(n) (1 << n)
const uint64_t FUSE_SESSION_LOOP_NONE = 0;
const uint64_t FUSE_SESSION_LOOP_EPOLL = SHIFT(0);
const uint64_t FUSE_SESSION_LOOP_SYNC = SHIFT(1);
const uint64_t FUSE_SESSION_LOOP_IOURING_CASCADING = SHIFT(2);
const uint64_t FUSE_SESSION_LOOP_IOURING = SHIFT(3);

const uint64_t FUSE_SESSION_LOOP_DEFAULT = FUSE_SESSION_LOOP_EPOLL |
                                           FUSE_SESSION_LOOP_SYNC  |
                                           FUSE_SESSION_LOOP_IOURING_CASCADING |
                                           FUSE_SESSION_LOOP_IOURING;
#undef SHIFT


class FuseSessionLoop {
public:
    virtual ~FuseSessionLoop() = default;

    virtual void run() = 0;
};

int set_sync_custom_io(struct fuse_session *);

#define DECLARE_SESSION_LOOP(name)  \
FuseSessionLoop *new_##name##_session_loop(struct fuse_session *)

DECLARE_SESSION_LOOP(epoll);
#if FUSE_USE_VERSION >= FUSE_MAKE_VERSION(3, 13)
DECLARE_SESSION_LOOP(sync);
#endif

inline FuseSessionLoop *
new_session_loop(struct fuse_session *se, uint64_t loop_type) {
    switch (loop_type) {
// The API function `fuse_session_custom_io` was introduced in version 3.13.0
#if FUSE_USE_VERSION >= FUSE_MAKE_VERSION(3, 13)
       case FUSE_SESSION_LOOP_SYNC:
           return new_sync_session_loop(se);
#endif
       case FUSE_SESSION_LOOP_EPOLL:
       default:
           return new_epoll_session_loop(se);
    }
}

}  // namespace fs
}  // namespace photon
