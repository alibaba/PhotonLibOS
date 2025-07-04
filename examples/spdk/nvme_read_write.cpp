#include <photon/io/spdknvme-wrapper.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/iovector.h>

int main() {
    char trid_str[] = "trtype:pcie traddr:0000:86:00.0";
    uint32_t nsid = 1;

    if (photon::spdk::nvme_env_init() != 0) {
        LOG_ERROR_RETURN(0, -1, "nvme_env_init failed");
    }
    DEFER(photon::spdk::nvme_env_fini());

    struct spdk_nvme_ctrlr* ctrlr = photon::spdk::nvme_probe_attach(trid_str);
    if (ctrlr == nullptr) {
        LOG_ERROR_RETURN(0, -1, "nvme_probe_attach failed");
    }
    DEFER(photon::spdk::nvme_detach(ctrlr));

    struct spdk_nvme_ns* ns = photon::spdk::nvme_get_namespace(ctrlr, nsid);
    if (ns == nullptr) {
        LOG_ERROR_RETURN(0, -1, "nvme_get_namespace failed");
    }

    if (photon::init() != 0) {
        LOG_ERROR_RETURN(0, -1, "photon::init failed");
    }
    DEFER(photon::fini());

    LOG_INFO("alloc io qpair");
    struct spdk_nvme_qpair* qpair = photon::spdk::nvme_ctrlr_alloc_io_qpair(ctrlr, nullptr, 0);
    if (qpair == nullptr) {
        LOG_ERROR_RETURN(0, -1, "nvme_ctrlr_alloc_io_qpair failed");
    }
    DEFER(photon::spdk::nvme_ctrlr_free_io_qpair(ctrlr, qpair));

    uint32_t sectorsz = spdk_nvme_ns_get_sector_size(ns);
    uint32_t nsec = 4;
    uint64_t bufsz = sectorsz * nsec;

    void* buf_write = spdk_zmalloc(bufsz, 0, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    void* buf_read = spdk_zmalloc(bufsz, 0, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    if (buf_write == nullptr || buf_read == nullptr) {
        LOG_ERROR_RETURN(0, -1, "spdk_zmalloc failed");
    }
    DEFER(spdk_free(buf_write));
    DEFER(spdk_free(buf_read));

    // prepare datas to write
    char test_data[] = "hello world";
    strncpy((char*)buf_write, test_data, 12);

    LOG_INFO("write");
    if (photon::spdk::nvme_ns_cmd_write(ns, qpair, buf_write, 0, nsec, 0) != 0) {
        LOG_ERROR_RETURN(0, -1, "nvme_ns_cmd_write failed");
    }
    LOG_INFO("read");
    if (photon::spdk::nvme_ns_cmd_read(ns, qpair, buf_read, 0, nsec, 0) != 0) {
        LOG_ERROR_RETURN(0, -1, "nvme_ns_cmd_read failed");
    }

    // print
    LOG_INFO("burwrite=`, bufread=`", (char*)buf_write, (char*)buf_read);
}