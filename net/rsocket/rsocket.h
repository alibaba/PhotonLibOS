#pragma once

#include <photon/net/socket.h>

extern "C" photon::net::ISocketClient* new_rsocket_client();
extern "C" photon::net::ISocketServer* new_rsocket_server();
