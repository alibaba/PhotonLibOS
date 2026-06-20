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

#include "socket.h"

#include <string>

#include <photon/common/alog.h>
#include <photon/net/basic_socket.h>

#include "base_socket.h"
#include "stream_pool.h"

namespace photon {
namespace net {

class TCPSocketPool : public ForwardSocketClient,
                      public StreamPoolBase<EndPoint> {
public:
    TCPSocketPool(ISocketClient* client, uint64_t TTL_us = -1UL,
                  bool client_ownership = false)
        : ForwardSocketClient(client, client_ownership),
          StreamPoolBase(TTL_us) {}

    ISocketStream* connect(const char* path, size_t count) override {
        LOG_ERROR_RETURN(ENOSYS, nullptr,
                         "Socket pool supports TCP-like socket only");
    }

    ISocketStream* connect(const EndPoint& remote,
                           const EndPoint* local) override {
        return acquire(remote, [this, &remote, local]() {
            return m_underlay->connect(remote, local);
        });
    }
};

extern "C" ISocketClient* new_tcp_socket_pool(ISocketClient* client, uint64_t TTL_us, bool client_ownership) {
    return new TCPSocketPool(client, TTL_us, client_ownership);
}

}
}
