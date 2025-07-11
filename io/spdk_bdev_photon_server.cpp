#include "spdk_bdev_photon_protocol.h"
#include "spdk_bdev_photon_server.h"
#include "spdknvme-wrapper.h"

#include <photon/net/socket.h>
#include <photon/common/alog-stdstring.h>

namespace photon {
namespace spdk {

class LocalNvmeDevice {
public:
    ~LocalNvmeDevice() {
        if (qpair_ != nullptr) {
            nvme_ctrlr_free_io_qpair(ctrlr_, qpair_);
            qpair_ = nullptr;
        }
        if (ctrlr_ != nullptr) {
            nvme_detach(ctrlr_);
            ctrlr_ = nullptr;
        }
        nvme_env_fini();
    }
    int Init(const char* trid_str, uint32_t nsid, uint32_t* sector_size, uint64_t* num_sectors) {
        if (ctrlr_ != nullptr || ns_ != nullptr || qpair_ != nullptr) {
            LOG_ERROR_RETURN(0, -1, "already initialized");
        }
        if (nvme_env_init() != 0) {
            LOG_ERROR_RETURN(0, -1, "failed to init nvme env");
        }
        if (sector_size == nullptr || num_sectors == nullptr) {
            LOG_ERROR_RETURN(0, -1, "invalid parameter");
        }
        ctrlr_ = nvme_probe_attach(trid_str);
        ns_ = nvme_get_namespace(ctrlr_, nsid);
        if (nvme_ns_get_info(ns_, &sector_size_, &num_sectors_) != 0) {
            LOG_ERROR_RETURN(0, -1, "failed to get namespace info");
        }
        qpair_ = nvme_ctrlr_alloc_io_qpair(ctrlr_, nullptr, 0);

        *sector_size = sector_size_;
        *num_sectors = num_sectors_;
        return 0;
    }

    int Writev(struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count)
    {
        return nvme_ns_cmd_writev(ns_, qpair_, iov, iovcnt, lba, lba_count, 0);
    }

    int Readv(struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count) {
        return nvme_ns_cmd_readv(ns_, qpair_, iov, iovcnt, lba, lba_count, 0);
    }

private:
    struct spdk_nvme_ctrlr* ctrlr_ = nullptr;
    struct spdk_nvme_ns* ns_ = nullptr;
    struct spdk_nvme_qpair* qpair_ = nullptr;

    uint32_t sector_size_ = 0;
    uint64_t num_sectors_ = 0;
};

class RPCServer : public SPDKBDevPhotonServer {
public:
    RPCServer(std::string ip="127.0.0.1", uint16_t port=43548)
    :
    ep_(ip.c_str(), port),
    socket_server_(photon::net::new_tcp_socket_server()),
    skeleton_(photon::rpc::new_skeleton()) {
        skeleton_->register_service<InitDevice, WritevBlocks, ReadvBlocks>(this);
    }

    int do_rpc_service(InitDevice::Request* req, InitDevice::Response* resp, IOVector*, IStream*) {
        LOG_DEBUG("recieve initdevice request: ", VALUE(req->trid.c_str()), VALUE(req->nsid));
        resp->rc = device_.Init(req->trid.c_str(), req->nsid, &resp->sector_size, &resp->num_sectors);
        return 0;
    }

    int do_rpc_service(WritevBlocks::Request* req, WritevBlocks::Response* resp, IOVector*, IStream*) {
        LOG_DEBUG("recieve writevblocks request: ", VALUE(req->offset_blocks), VALUE(req->num_blocks));
        resp->rc = device_.Writev(req->buf.begin(), req->buf.size(), req->offset_blocks, req->num_blocks);
        return 0;
    }

    int do_rpc_service(ReadvBlocks::Request* req, ReadvBlocks::Response* resp, IOVector*, IStream*) {
        LOG_DEBUG("recieve readvblocks request: ", VALUE(req->offset_blocks), VALUE(req->num_blocks));
        resp->rc = device_.Readv(resp->buf.begin(), resp->buf.size(), req->offset_blocks, req->num_blocks);
        return 0;
    }

    int run() override {
        LOG_DEBUG("server run");
        auto handler = [&](photon::net::ISocketStream* socket_stream) {
            LOG_DEBUG("get into handler");
            return skeleton_->serve(socket_stream);
        };

        socket_server_->set_handler(handler);
        socket_server_->bind(ep_);
        socket_server_->listen();

        int rc = socket_server_->start_loop(true);
        return rc;
    }

private:
    photon::net::EndPoint ep_;
    std::unique_ptr<photon::net::ISocketServer> socket_server_;
    std::unique_ptr<photon::rpc::Skeleton> skeleton_;
    LocalNvmeDevice device_;
};

SPDKBDevPhotonServer* new_server() {
    return new RPCServer();
}

}   // namespace spdk
}   // namespace photon