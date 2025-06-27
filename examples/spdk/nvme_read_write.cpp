#include <photon/io/spdknvme-wrapper.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/iovector.h>

int main() {
    char trid_str[] = "trtype:pcie traddr:0000:86:00.0";
    uint32_t nsid = 1;

    if (photon::init() != 0) {
        LOG_ERROR_RETURN(0, -1, "photon::init failed");
    }
    DEFER(photon::fini());

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

    struct spdk_nvme_qpair* qpair = photon::spdk::nvme_ctrlr_alloc_io_qpair(ctrlr, nullptr, 0);
    if (qpair == nullptr) {
        LOG_ERROR_RETURN(0, -1, "nvme_ctrlr_alloc_io_qpair failed");
    }
    DEFER(photon::spdk::nvme_ctrlr_free_io_qpair(qpair));

    uint32_t sectorsz = 512;
    void* buf_write = spdk_zmalloc(sectorsz, 0, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    void* buf_read = spdk_zmalloc(sectorsz, 0, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    if (buf_write == nullptr || buf_read == nullptr) {
        LOG_ERROR_RETURN(0, -1, "spdk_zmalloc failed");
    }
    DEFER(spdk_free(buf_write));
    DEFER(spdk_free(buf_read));

    // prepare datas to write
    char test_data[] = "hello world";
    strncpy((char*)buf_write, test_data, 12);

    if (photon::spdk::nvme_ns_cmd_write(ns, qpair, buf_write, 0, 1, 0) != 0) {
        LOG_ERROR_RETURN(0, -1, "nvme_ns_cmd_write failed");
    }
    if (photon::spdk::nvme_ns_cmd_read(ns, qpair, buf_read, 0, 1, 0) != 0) {
        LOG_ERROR_RETURN(0, -1, "nvme_ns_cmd_read failed");
    }

    // print
    LOG_INFO("burwrite=`, bufread=`", (char*)buf_write, (char*)buf_read);

    // prepare datas to write
    memset(buf_write, 0, 512);
    memset(buf_read, 0, 512);

    memset(buf_write, 'a', 200);
    memset((char*)buf_write+200, 'b', 112);
    memset((char*)buf_write+312, 'c', 200);
    IOVector iovs_write;
    iovs_write.push_back(buf_write, 200);
    iovs_write.push_back((char*)buf_write+200, 112);
    iovs_write.push_back((char*)buf_write+312, 200);

    IOVector iovs_read;
    iovs_read.push_back(buf_read, 100);
    iovs_read.push_back((char*)buf_read+100, 200);
    iovs_read.push_back((char*)buf_read+300, 212);

    if (photon::spdk::nvme_ns_cmd_writev(ns, qpair, iovs_write.iovec(), iovs_write.iovcnt(), 1, 1, 0) != 0) {
        LOG_ERRNO_RETURN(0, -1, "nvme_ns_cmd_writev");
    }
    if (photon::spdk::nvme_ns_cmd_readv(ns, qpair, iovs_read.iovec(), iovs_read.iovcnt(), 1, 1, 0) != 0) {
        LOG_ERRNO_RETURN(0, -1, "nvme_ns_cmd_readv");
    }

    // checking
    if (memcmp(buf_write, buf_read, 512) != 0) {
        LOG_ERROR_RETURN(0, -1, "checking failed");
    }
    else {
        LOG_INFO("checking success");
    }
}