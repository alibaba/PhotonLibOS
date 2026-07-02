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
#include <photon/net/datagram_socket.h>

namespace photon {
namespace net {

// Socket option level for KCP-specific parameters
const int SOL_KCP = 0x1000;

// KCP socket option names for setsockopt/getsockopt
const int KCP_MTU          = 5;  // int: maximum transmission unit
const int KCP_AUTO_FLUSH   = 8;  // int (0/1): flush immediately after send (default 0)
const int KCP_NODELAY_OPTS = 9;  // int[4]: nodelay, interval, resend, nc in one call
const int KCP_WND_OPTS     = 10; // int[2]: sndwnd, rcvwnd in one call
const int KCP_CONV         = 11; // uint32_t (get only): conversation id
const int KCP_UPDATE_INTERVAL = 12; // int (10-100): ikcp_update poll interval in ms (default 100)

// Note: KCP does not implement handshake or graceful close (FIN/RST).
// Closing a KCP stream releases local resources immediately; unacknowledged
// data in the send buffer is lost and the remote peer is not notified.
// Applications needing graceful shutdown should implement it at the
// application layer (e.g., send a "disconnect" message, wait for ack).

// Create a KCP socket client over an existing datagram socket.
// The datagram socket is NOT owned by the client.
// Multiple KCP connections (each with a distinct conv) can share
// the same datagram socket.
extern "C" ISocketClient* new_kcp_socket_client(UDPSocket* datagram_socket);


// Create a KCP socket server over an existing datagram socket.
// The datagram socket is NOT owned by the server.
// The server accepts multiple KCP connections multiplexed over
// the single datagram socket, demultiplexed by KCP conv.
extern "C" ISocketServer* new_kcp_socket_server(UDPSocket* datagram_socket);

}  // namespace net
}  // namespace photon
