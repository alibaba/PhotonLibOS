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
#include <photon/net/socket.h>

namespace photon {
namespace net {

extern "C" int et_poller_init();
extern "C" int et_poller_fini();

extern "C" ISocketClient* new_et_tcp_socket_client();
extern "C" ISocketServer* new_et_tcp_socket_server();
extern "C" ISocketStream* new_et_tcp_socket_stream(int fd);

}
}
