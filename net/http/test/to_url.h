#pragma once
#include <photon/common/estring.h>
#include <photon/net/socket.h>

inline estring to_url(photon::net::ISocketServer* svr, std::string_view path) {
    return estring().appends("http://localhost:", svr->getsockname().port, path);
}

inline estring to_surl(photon::net::ISocketServer* svr, std::string_view path) {
    return estring().appends("https://localhost:", svr->getsockname().port, path);
}

