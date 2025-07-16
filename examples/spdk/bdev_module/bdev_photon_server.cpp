#include "bdev_photon_protocol.h"
#include "bdev_photon_server.h"

#include <spdk/nvme.h>

#include <photon/io/spdknvme-wrapper.h>
#include <photon/net/socket.h>
#include <photon/common/alog-stdstring.h>
#include <photon/fs/localfs.h>


namespace photon {
namespace spdk {

class LocalNvmeDevice : public BlockDevice {
public:
    ~LocalNvmeDevice() override {
        ns_ = nullptr;
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
    int Init(const char* trid_str, uint32_t nsid, uint64_t num_blocks, uint32_t* sector_size, uint64_t* num_sectors) override {
        LOG_DEBUG("Initialize LocalNvmeDevice, ", VALUE(trid_str), VALUE(nsid), VALUE(num_blocks));
        if (ctrlr_ != nullptr || ns_ != nullptr || qpair_ != nullptr) {
            LOG_ERROR_RETURN(0, -1, "already initialized");
        }
        if (sector_size == nullptr || num_sectors == nullptr) {
            LOG_ERROR_RETURN(0, -1, "invalid parameter");
        }
        if (!is_nvme_env_inited) {
            if (nvme_env_init() != 0) LOG_ERROR_RETURN(0, -1, "failed to init nvme env");
            is_nvme_env_inited = true;
        }

        ctrlr_ = nvme_probe_attach(trid_str);
        ns_ = nvme_get_namespace(ctrlr_, nsid);

        sector_size_ = spdk_nvme_ns_get_sector_size(ns_);
        num_sectors_ = spdk_nvme_ns_get_num_sectors(ns_);
        LOG_DEBUG("Init, ", VALUE(sector_size_), VALUE(num_sectors_));

        if (num_blocks > num_sectors_) {
            LOG_ERROR_RETURN(0, -1, "invalid num_blocks, only `", num_sectors_);
        }
        num_blocks_ = num_blocks;

        qpair_ = nvme_ctrlr_alloc_io_qpair(ctrlr_, nullptr, 0);

        *sector_size = sector_size_;
        *num_sectors = num_sectors_;
        return 0;
    }

    int Fini() override {
        ns_ = nullptr;
        if (qpair_ != nullptr) {
            nvme_ctrlr_free_io_qpair(ctrlr_, qpair_);
            qpair_ = nullptr;
        }
        if (ctrlr_ != nullptr) {
            nvme_detach(ctrlr_);
            ctrlr_ = nullptr;
        }
        return 0;
    }

    int Writev(struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count) override {
        IOVector iovec;
        for (int i=0; i<iovcnt; i++) {
            uint64_t len = iov[i].iov_len;
            void* buf = spdk_zmalloc(len, 512, NULL, SPDK_ENV_SOCKET_ID_ANY, 1);
            memcpy(buf, iov[i].iov_base, len);
            iovec.push_back(buf, len);
        }
        int rc = nvme_ns_cmd_writev(ns_, qpair_, iovec.iovec(), iovec.iovcnt(), lba, lba_count, 0);
        for (int i=0; i<iovcnt; i++){
            spdk_free(iovec[i].iov_base);
        }
        return rc;
    }

    int Readv(struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count) override {
        LOG_DEBUG(VALUE(iovcnt), VALUE(lba), VALUE(lba_count));
        IOVector iovec;
        for (int i=0; i<iovcnt; i++) {
            uint64_t len = iov[i].iov_len;
            void* buf = spdk_zmalloc(len, 512, NULL, SPDK_ENV_SOCKET_ID_ANY, 1);
            iovec.push_back(buf, len);
        }
        int rc = nvme_ns_cmd_readv(ns_, qpair_, iovec.iovec(), iovec.iovcnt(), lba, lba_count, 0);
        for (int i=0; i<iovcnt; i++) {
            void* buf = iovec[i].iov_base;
            uint64_t len = iovec[i].iov_len;
            memcpy(iov[i].iov_base, buf, len);
            spdk_free(buf);
        }
        return rc;
    }

private:
    bool is_nvme_env_inited = false;
    struct spdk_nvme_ctrlr* ctrlr_ = nullptr;
    struct spdk_nvme_ns* ns_ = nullptr;
    struct spdk_nvme_qpair* qpair_ = nullptr;
};

class LocalFSDevice : public BlockDevice {
public:
    ~LocalFSDevice() override {
        if (mock_disk_ != nullptr) {
            delete mock_disk_;
            mock_disk_ = nullptr;
        }
        if (fs_ != nullptr) {
            delete fs_;
            fs_ = nullptr;
        }
    }

    int Init(const char* trid_str, uint32_t nsid, uint64_t num_blocks, uint32_t* sector_size, uint64_t* num_sectors) override {
        LOG_DEBUG("Initialize LocalFSDevice, ", VALUE(trid_str), VALUE(nsid), VALUE(num_blocks));
        if (fs_ != nullptr || mock_disk_ != nullptr) {
            LOG_ERROR_RETURN(0, -1, "already initialized");
        }
        if (sector_size == nullptr || num_sectors == nullptr) {
            LOG_ERROR_RETURN(0, -1, "invalid parameter");
        }

        fs_ = photon::fs::new_localfs_adaptor("/tmp/bdev_photon_test", photon::fs::ioengine_psync);
        if (fs_ == nullptr) {
            LOG_ERROR_RETURN(0, -1, "failed to create localfs adaptor");
        }

        std::string fname = trid_str + std::to_string(nsid);
        mock_disk_ = fs_->open(fname.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (mock_disk_ == nullptr) {
            LOG_ERROR_RETURN(0, -1, "failed to create file for mock disk");
        }

        sector_size_ = 512;
        num_sectors_ = 1024;
        if (num_blocks > num_sectors_) {
            LOG_ERROR_RETURN(0, -1, "invalid num_blocks, only `", num_sectors_);
        }
        num_blocks_ = num_blocks;


        if (mock_disk_->ftruncate(sector_size_ * num_sectors_) != 0) {
            LOG_ERROR_RETURN(0, -1, "failed to truncate file for mock disk");
        }

        *sector_size = sector_size_;
        *num_sectors = num_sectors_;
        return 0;
    }

    int Fini() override {
        if (mock_disk_ != nullptr) {
            delete mock_disk_;
            mock_disk_ = nullptr;
        }
        if (fs_ != nullptr) {
            delete fs_;
            fs_ = nullptr;
        }
        return 0;
    }

    int Writev(struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count) override {
        LOG_DEBUG(VALUE(iovcnt), VALUE(lba), VALUE(lba_count));
        uint64_t offset_in_bytes = lba * sector_size_;
        uint64_t count_in_bytes = lba_count * sector_size_;
        if (offset_in_bytes + count_in_bytes > num_blocks_ * sector_size_) {
            LOG_ERROR_RETURN(0, -1, "invalid lba range");
        }
        return mock_disk_->pwritev(iov, iovcnt, offset_in_bytes);
    }

    int Readv(struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count) override {
        LOG_DEBUG(VALUE(iovcnt), VALUE(lba), VALUE(lba_count));
        uint64_t offset_in_bytes = lba * sector_size_;
        uint64_t count_in_bytes = lba_count * sector_size_;
        if (offset_in_bytes + count_in_bytes > num_blocks_ * sector_size_) {
            LOG_ERROR_RETURN(0, -1, "invalid lba range");
        }
        return mock_disk_->preadv(iov, iovcnt, offset_in_bytes);
    }

private:
    photon::fs::IFileSystem* fs_ = nullptr;
    photon::fs::IFile* mock_disk_ = nullptr;
};

class RPCServer : public SPDKBDevPhotonServer {
public:
    RPCServer(BlockDevice* device, std::string ip, uint16_t port)
    :
    SPDKBDevPhotonServer(device),
    ep_(ip.c_str(), port),
    socket_server_(photon::net::new_tcp_socket_server()),
    skeleton_(photon::rpc::new_skeleton()) {
        skeleton_->register_service<InitDevice, FiniDevice, WritevBlocks, ReadvBlocks>(this);
    }

    int do_rpc_service(InitDevice::Request* req, InitDevice::Response* resp, IOVector*, IStream*) {
        LOG_DEBUG("recieve initdevice request: ", VALUE(req->trid.c_str()), VALUE(req->nsid));
        resp->rc = device_->Init(req->trid.c_str(), req->nsid, req->num_blocks, &resp->sector_size, &resp->num_sectors);
        return resp->rc;
    }

    int do_rpc_service(FiniDevice::Request* req, FiniDevice::Response* resp, IOVector*, IStream*) {
        LOG_DEBUG("recieve finidevice request");
        resp->rc = device_->Fini();
        return resp->rc;
    }

    int do_rpc_service(WritevBlocks::Request* req, WritevBlocks::Response* resp, IOVector*, IStream*) {
        LOG_DEBUG("recieve writevblocks request: ", VALUE(req->offset_blocks), VALUE(req->num_blocks));
        resp->rc = device_->Writev(req->buf.begin(), req->buf.size(), req->offset_blocks, req->num_blocks);
        return resp->rc;
    }

    int do_rpc_service(ReadvBlocks::Request* req, ReadvBlocks::Response* resp, IOVector* iov, IStream*) {
        LOG_DEBUG("recieve readvblocks request: ", VALUE(req->offset_blocks), VALUE(req->num_blocks));
        iov->push_back(req->num_blocks * 512);
        resp->rc = device_->Readv(iov->iovec(), iov->iovcnt(), req->offset_blocks, req->num_blocks);
        if (resp->rc < 0) iov->shrink_to(0);
        resp->buf.assign(iov->iovec(), iov->iovcnt());
        return resp->rc;
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
};


BlockDevice* new_blkdev_local_nvme_ssd() {
    return new LocalNvmeDevice();
}

BlockDevice* new_blkdev_localfs() {
    return new LocalFSDevice();
}

SPDKBDevPhotonServer* new_server(BlockDevice* device, std::string ip, uint16_t port) {
    return new RPCServer(device, ip, port);
}

}   // namespace spdk
}   // namespace photon