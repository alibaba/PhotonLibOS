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
#ifdef _WIN32
// signalfd not available on Windows
#include <signal.h>
struct signalfd_siginfo { uint32_t ssi_signo; };
inline int signalfd(int, const sigset_t*, int) { return -1; }
#else
#include_next <sys/signalfd.h>
#endif
