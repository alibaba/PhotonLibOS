#pragma once

#include <photon/net/socket.h>

namespace photon {
namespace net {

// adaptor for rsocket in RDMA-Core
extern "C" ISocketClient* new_rsocket_client();
extern "C" ISocketServer* new_rsocket_server();

}  // namespace net
}  // namespace photon
