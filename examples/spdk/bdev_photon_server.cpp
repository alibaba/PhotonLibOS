#include <photon/photon.h>
#include <photon/io/spdk_bdev_photon_server.h>

int main() {
    photon::init();
    DEFER(photon::fini());
    auto server = photon::spdk::new_server();
    DEFER(delete server);
    server->run();
}