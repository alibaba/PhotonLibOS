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
