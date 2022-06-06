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

struct ssl_st;
typedef struct ssl_st SSL;

namespace photon {
namespace net {

int ssl_init(const char* cert_path, const char* key_path,
             const char* passphrase);
int ssl_init(void* cert, void* key);
int ssl_init();
void* ssl_get_ctx();
int ssl_set_cert(const char* cert);
int ssl_set_pkey(const char* key, const char* passphrase);
int ssl_fini();
ssize_t ssl_set_cert_file(const char* path);
ssize_t ssl_set_pkey_file(const char* path);

ISocketClient* new_tls_socket_client();
ISocketServer* new_tls_socket_server();

}  // namespace net
}
