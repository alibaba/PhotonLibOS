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
#include <csignal>

#ifdef __APPLE__
using sighandler_t = void(*)(int);
#endif

#ifdef _WIN32
// Windows has no POSIX signal API.  Provide stub types / constants / inline
// no-op functions so code written against the POSIX signal interface (typical
// in examples and tests: `photon::sync_signal(SIGINT, handler)` etc.)
// compiles and runs without modification.  Handlers registered here are
// ignored — Windows programs that need graceful shutdown should use
// SetConsoleCtrlHandler directly.
#ifndef SIGTSTP
#define SIGTSTP 0
#endif
#ifndef SIGPIPE
#define SIGPIPE 0
#endif
#ifndef SIGTERM
#define SIGTERM 0
#endif
#ifndef SIGINT
#define SIGINT 0
#endif

struct sigaction;  // incomplete — Windows has no sigaction; declared only so
                   // the sync_sigaction prototype below parses.
using sighandler_t = void(*)(int);

namespace photon {

inline int sync_signal_init() { return 0; }
inline int block_all_signal() { return 0; }
inline sighandler_t sync_signal(int, sighandler_t) { return nullptr; }
inline int sync_sigaction(int, const struct sigaction*, struct sigaction*) {
    return 0;
}
inline int sync_signal_fini() { return 0; }

}  // namespace photon

#else  // POSIX

// sync_signal will be executed sequentially in a dedicated photon thread
namespace photon
{
    extern "C" int sync_signal_init();

    // block all default action to signals
    // except SIGSTOP & SIGKILL
    extern "C" int block_all_signal();

    // set signal handler for signal `signum`, default to SIG_IGN
    // the handler is always restartable, until removed by setting to SIG_IGN
    extern "C" sighandler_t sync_signal(int signum, sighandler_t handler);

    // set signal handler for signal `signum`, default to SIG_IGN
    // the handler is always restartable, until removed by setting to SIG_IGN
    // `sa_mask` and most flags of `sa_flags` are ingored
    // It is not fully supported and equivlent to sync_signal on MacOS
    extern "C" int sync_sigaction(int signum,
        const struct sigaction *act, struct sigaction *oldact);

    extern "C" int sync_signal_fini();
}

#endif  // _WIN32
