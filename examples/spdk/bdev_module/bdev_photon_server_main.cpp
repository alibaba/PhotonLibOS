#include "bdev_photon_server.h"

#include <gflags/gflags.h>

#include <photon/photon.h>
#include <photon/common/alog-stdstring.h>

DEFINE_string(device_type, "fs", "device type is ssd or fs");
DEFINE_string(ip, "127.0.0.1", "ip address");
DEFINE_uint64(port, 43548, "port");

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    std::string device_type(FLAGS_device_type.c_str());
    std::string ip(FLAGS_ip.c_str());
    uint16_t port = (uint16_t)FLAGS_port;

    photon::init();
    DEFER(photon::fini());

    auto type = photon::spdk::DeviceType::kUnknown;
    if (device_type == "ssd") {
        type = photon::spdk::DeviceType::kNVMeSSD;
    }
    else if (device_type == "fs") {
        type = photon::spdk::DeviceType::kLocalFile;
    }
    else {
        LOG_ERROR_RETURN(0, -1, "unknown device type: `", device_type.c_str());
    }

    auto server = photon::spdk::new_server(type, ip, port);
    if (server == nullptr) {
        LOG_ERRNO_RETURN(0, -1, "server create failed");
    }
    DEFER(delete server);

    server->run();
}