#include "bdev_photon_server.h"

#include <photon/photon.h>
#include <photon/common/alog-stdstring.h>


int main(int argc, char** argv) {
    if (argc != 2) {
        LOG_ERROR_RETURN(0, -1, "Usage: ` <device type>\ndevice type: ssd, fs", argv[0]);
    }
    std::string device_type(argv[1]);

    photon::init();
    DEFER(photon::fini());

    photon::spdk::BlockDevice* blkdev = nullptr;
    if (device_type == "ssd") {
        blkdev = photon::spdk::new_blkdev_local_nvme_ssd();
    }
    else if (device_type == "fs") {
        blkdev = photon::spdk::new_blkdev_localfs();
    }
    else {
        LOG_ERROR_RETURN(0, -1, "unknown device type: `", device_type.c_str());
    }
    if (blkdev == nullptr) {
        LOG_ERRNO_RETURN(0, -1, "device create failed");
    }
    DEFER(delete blkdev);

    auto server = photon::spdk::new_server(blkdev);
    if (server == nullptr) {
        LOG_ERRNO_RETURN(0, -1, "server create failed");
    }
    DEFER(delete server);

    server->run();
}