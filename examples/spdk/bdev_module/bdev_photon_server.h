#pragma once

#include <photon/fs/filesystem.h>
#include <string>

namespace photon {
namespace spdk {

class SPDKBDevPhotonServer {
public:
    virtual ~SPDKBDevPhotonServer() = default;
    virtual int run() = 0;
};

enum DeviceType {
    kUnknown = 0,
    kNVMeSSD,
    kLocalFile
};

SPDKBDevPhotonServer* new_server(enum DeviceType type=kLocalFile, std::string ip="127.0.0.1", uint16_t port=43548);

}
}   // namespace photon