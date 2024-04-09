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

#include <photon/ecosystem/redis.h>
#include <inttypes.h>
#include <memory>
#include <photon/net/socket.h>
#include <photon/common/alog.h>
#include <cpp_redis/network/tcp_client_iface.hpp>

namespace photon {
using namespace net;
namespace integration {

static thread_local std::unique_ptr<ISocketClient>
    _client { new_tcp_socket_client() };

class tcp_client_redis : public cpp_redis::network::tcp_client_iface {
public:
    ISocketStream* _s = nullptr;
    ~tcp_client_redis() {
        delete _s;
    }
    void connect(const std::string& addr,
            uint32_t port, uint32_t timeout_msecs) override {
        if (_s)
            LOG_ERROR_RETURN(0, , "client is already connected");
        EndPoint ep(addr.c_str(), port);
        _s = _client->connect(ep);
        if (_s == nullptr)
            LOG_ERRNO_RETURN(0, , "failed to connect to ", ep);
    }
    bool is_connected(void) const override {
        return _s;
    }
    void disconnect(bool /*wait_for_removal*/ = false) override {
        delete _s;
        _s = nullptr;
        _dch();
    }
    disconnection_handler_t _dch;
    void set_on_disconnection_handler(const disconnection_handler_t& h) override {
        _dch = h;
    }
    void async_read(read_request& request) override {
        ssize_t cnt;
        read_result r;
        if (!_s) {
            LOG_ERROR("not connected");
            goto fail;
        }
        if (!request.size) { fail:
            r.success = false;
            goto out;
        }

        r.buffer.resize(request.size);
        cnt = _s->read(&r.buffer[0], r.buffer.size());
        if (cnt <= 0) {
            if (cnt == 0) errno = ENETRESET;
            LOG_ERROR("failed to read socket: ", ERRNO());
            r.buffer.clear();
            disconnect();
        } else { r.success = true; }
out:
        request.async_read_callback(r);
    }
    void async_write(write_request& request) override {
        ssize_t cnt;
        write_result r{false, 0};
        if (!_s) {
            LOG_ERROR("not connected");
            goto out;
        }
        if (!request.buffer.size()) {
            goto out;
        }

        cnt = _s->write(&request.buffer[0], request.buffer.size());
        if (cnt <= 0) {
            if (cnt == 0) errno = ENETRESET;
            LOG_ERROR("failed to write socket: ", ERRNO());
            disconnect();
        } else { r = {true, (size_t)cnt}; }
out:
        request.async_write_callback(r);
    }
};

cpp_redis::network::tcp_client_iface* new_tcp_client_redis() {
    return new tcp_client_redis;
}

}
}
