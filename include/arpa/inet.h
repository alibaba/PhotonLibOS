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
// arpa/inet.h provides inet_ntop/inet_pton etc.
// On Windows these are in ws2tcpip.h (included via winsock2)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include_next <arpa/inet.h>
#endif
