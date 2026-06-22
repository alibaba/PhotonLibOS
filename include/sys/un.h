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
// Windows 10 build 17063+ supports Unix domain sockets natively.
// <afunix.h> defines struct sockaddr_un (sun_family + sun_path[108]) and the
// AF_UNIX constant. Pull in winsock2 first so SOCKET / socklen_t are visible,
// then afunix.h.
#include <winsock2.h>
#include <afunix.h>
#else
#include_next <sys/un.h>
#endif
