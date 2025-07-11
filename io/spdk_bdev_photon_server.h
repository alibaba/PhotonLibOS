#pragma once

namespace photon {
namespace spdk {

class SPDKBDevPhotonServer {
public:
    virtual ~SPDKBDevPhotonServer() {}
    virtual int run() = 0;
};

SPDKBDevPhotonServer* new_server();

}   // namespace spdk
}   // namespace photon