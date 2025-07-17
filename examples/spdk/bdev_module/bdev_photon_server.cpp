#include "photon/protocol.h"
#include "bdev_photon_server.h"

#include <spdk/nvme.h>

#include <photon/io/spdknvme-wrapper.h>
#include <photon/net/socket.h>
#include <photon/common/alog-stdstring.h>
#include <photon/fs/localfs.h>

#include <stdexcept>

namespace photon {
namespace spdk {

class LocalNVMeDevice : public photon::fs::IFile {
public:
    LocalNVMeDevice(const char* trid_str, uint32_t nsid, uint64_t num_blocks, uint32_t* sector_size, uint64_t* num_sectors) {
        LOG_DEBUG("Initialize LocalNVMeDevice, ", VALUE(trid_str), VALUE(nsid), VALUE(num_blocks));
        if (Init(trid_str, nsid, num_blocks, sector_size, num_sectors) != 0) {
            throw std::runtime_error("Initialize LocalNvmeDevice failed");
        }
    }

    ~LocalNVMeDevice() {
        LOG_DEBUG("Destroy LocalNVMeDevice");
        ns_ = nullptr;
        if (qpair_ != nullptr) {
            nvme_ctrlr_free_io_qpair(ctrlr_, qpair_);
            qpair_ = nullptr;
        }
        if (ctrlr_ != nullptr) {
            nvme_detach(ctrlr_);
            ctrlr_ = nullptr;
        }
    }

    ssize_t pwritev(const struct iovec* iov, int iovcnt, off_t offset) override {
        if (offset % sector_size_ != 0) {
            LOG_ERROR_RETURN(0, -1, "offset is not aligned to sector size");
        }

        uint64_t total_len = 0;
        IOVector iovec;
        for (int i=0; i<iovcnt; i++) {
            uint64_t len = iov[i].iov_len;
            void* buf = spdk_zmalloc(len, 512, NULL, SPDK_ENV_SOCKET_ID_ANY, 1);
            memcpy(buf, iov[i].iov_base, len);
            iovec.push_back(buf, len);
            total_len += len;
        }
        if (total_len % sector_size_ != 0) {
            LOG_ERROR_RETURN(0, -1, "write len is not aligned to sector size");
        }

        uint64_t lba = offset / sector_size_;
        uint32_t lba_count = total_len / sector_size_;
        int rc = nvme_ns_cmd_writev(ns_, qpair_, iovec.iovec(), iovec.iovcnt(), lba, lba_count, 0);

        for (int i=0; i<iovcnt; i++){
            spdk_free(iovec[i].iov_base);
        }

        return rc;
    }

    ssize_t preadv(const struct iovec* iov, int iovcnt, off_t offset) override {
        if (offset % sector_size_ != 0) {
            LOG_ERROR_RETURN(0, -1, "offset is not aligned to sector size");
        }

        uint64_t total_len = 0;
        IOVector iovec;
        for (int i=0; i<iovcnt; i++) {
            uint64_t len = iov[i].iov_len;
            void* buf = spdk_zmalloc(len, 512, NULL, SPDK_ENV_SOCKET_ID_ANY, 1);
            memcpy(buf, iov[i].iov_base, len);
            iovec.push_back(buf, len);
            total_len += len;
        }
        if (total_len % sector_size_ != 0) {
            LOG_ERROR_RETURN(0, -1, "write len is not aligned to sector size");
        }

        uint64_t lba = offset / sector_size_;
        uint32_t lba_count = total_len / sector_size_;
        int rc = nvme_ns_cmd_readv(ns_, qpair_, iovec.iovec(), iovec.iovcnt(), lba, lba_count, 0);

        for (int i=0; i<iovcnt; i++){
            spdk_free(iovec[i].iov_base);
        }

        return rc;
    }

    photon::fs::IFileSystem* filesystem() override {
        return nullptr;
    }

    UNIMPLEMENTED(ssize_t pread(void *buf, size_t count, off_t offset) override);
    UNIMPLEMENTED(ssize_t pwrite(const void *buf, size_t count, off_t offset) override);
    UNIMPLEMENTED(off_t lseek(off_t offset, int whence) override);
    UNIMPLEMENTED(int fsync() override);
    UNIMPLEMENTED(int fdatasync() override);
    UNIMPLEMENTED(int fchmod(mode_t mode) override);
    UNIMPLEMENTED(int fchown(uid_t owner, gid_t group) override);
    UNIMPLEMENTED(int fstat(struct stat *buf) override);
    UNIMPLEMENTED(int ftruncate(off_t length) override);

    UNIMPLEMENTED(int close() override);
    UNIMPLEMENTED(ssize_t read(void *buf, size_t count) override);
    UNIMPLEMENTED(ssize_t readv(const struct iovec *iov, int iovcnt) override);
    UNIMPLEMENTED(ssize_t write(const void *buf, size_t count) override);
    UNIMPLEMENTED(ssize_t writev(const struct iovec *iov, int iovcnt) override);

private:
    struct spdk_nvme_ctrlr* ctrlr_ = nullptr;
    struct spdk_nvme_ns* ns_ = nullptr;
    struct spdk_nvme_qpair* qpair_ = nullptr;

    uint64_t num_blocks_ = 0;   // request

    uint32_t sector_size_ = 0;  // real
    uint64_t num_sectors_ = 0;  // real

    int Init(const char* trid_str, uint32_t nsid, uint64_t num_blocks, uint32_t* sector_size, uint64_t* num_sectors) {
        LOG_DEBUG("Initialize LocalNvmeDevice, ", VALUE(trid_str), VALUE(nsid), VALUE(num_blocks));
        if (ctrlr_ != nullptr || ns_ != nullptr || qpair_ != nullptr) {
            LOG_ERROR_RETURN(0, -1, "already initialized");
        }
        if (sector_size == nullptr || num_sectors == nullptr) {
            LOG_ERROR_RETURN(0, -1, "invalid parameter");
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
};

class LocalFileDevice : public photon::fs::IFile {
public:
    LocalFileDevice(const char* trid_str, uint32_t nsid, uint64_t num_blocks, uint32_t* sector_size, uint64_t* num_sectors) {
        LOG_DEBUG("Initialize LocalFSDevice, ", VALUE(trid_str), VALUE(nsid), VALUE(num_blocks));
        if (Init(trid_str, nsid, num_blocks, sector_size, num_sectors) != 0) {
            throw std::runtime_error("failed to construct LocalFSDevice");
        }
    }

    ~LocalFileDevice() override {
        LOG_DEBUG("Destroy LocalFSDevice");
        if (mock_disk_ != nullptr) {
            delete mock_disk_;
            mock_disk_ = nullptr;
        }
        if (fs_ != nullptr) {
            delete fs_;
            fs_ = nullptr;
        }
    }

    ssize_t pwritev(const struct iovec* iov, int iovcnt, off_t offset) override {
        return mock_disk_->pwritev(iov, iovcnt, offset);
    }

    ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override {
        return mock_disk_->preadv(iov, iovcnt, offset);
    }

    photon::fs::IFileSystem* filesystem() override {
        return nullptr;
    }

    UNIMPLEMENTED(ssize_t pread(void *buf, size_t count, off_t offset) override);
    UNIMPLEMENTED(ssize_t pwrite(const void *buf, size_t count, off_t offset) override);
    UNIMPLEMENTED(off_t lseek(off_t offset, int whence) override);
    UNIMPLEMENTED(int fsync() override);
    UNIMPLEMENTED(int fdatasync() override);
    UNIMPLEMENTED(int close() override);
    UNIMPLEMENTED(int fchmod(mode_t mode) override);
    UNIMPLEMENTED(int fchown(uid_t owner, gid_t group) override);
    UNIMPLEMENTED(int fstat(struct stat *buf) override);
    UNIMPLEMENTED(int ftruncate(off_t length) override);

    UNIMPLEMENTED(ssize_t read(void *buf, size_t count) override);
    UNIMPLEMENTED(ssize_t readv(const struct iovec *iov, int iovcnt) override);
    UNIMPLEMENTED(ssize_t write(const void *buf, size_t count) override);
    UNIMPLEMENTED(ssize_t writev(const struct iovec *iov, int iovcnt) override);

private:
    photon::fs::IFileSystem* fs_ = nullptr;     // localfs
    photon::fs::IFile* mock_disk_ = nullptr;    // local file mock disk

    uint64_t num_blocks_ = 0;   // request

    uint32_t sector_size_ = 0;  // real
    uint64_t num_sectors_ = 0;  // real

    int Init(const char* trid_str, uint32_t nsid, uint64_t num_blocks, uint32_t* sector_size, uint64_t* num_sectors) {
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
};

class RPCServer : public SPDKBDevPhotonServer {
public:
    RPCServer(enum DeviceType type, std::string ip, uint16_t port)
    :
    device_type_(type),
    ep_(ip.c_str(), port),
    socket_server_(photon::net::new_tcp_socket_server()),
    skeleton_(photon::rpc::new_skeleton()) {
        skeleton_->register_service<InitDevice, FiniDevice, WritevBlocks, ReadvBlocks>(this);
    }

    int do_rpc_service(InitDevice::Request* req, InitDevice::Response* resp, IOVector*, IStream*) {
        LOG_DEBUG("recieve initdevice request: ", VALUE(req->trid.c_str()), VALUE(req->nsid));
        if (device_ != nullptr) {
            LOG_ERRNO_RETURN(0, -1, "device already exists");
        }
        try {
            if (device_type_ == DeviceType::kNVMeSSD) {
                device_ = new LocalNVMeDevice(req->trid.c_str(), req->nsid, req->num_blocks, &resp->sector_size, &resp->num_sectors);
                resp->rc = 0;
            }
            else if (device_type_ == DeviceType::kLocalFile) {
                device_ = new LocalFileDevice(req->trid.c_str(), req->nsid, req->num_blocks, &resp->sector_size, &resp->num_sectors);
                resp->rc = 0;
            }
            else LOG_ERROR_RETURN(0, -1, "unknown device type");
        }
        catch (std::exception& e) {
            LOG_ERROR_RETURN(0, -1, "exception: `", e.what());
        }
        return resp->rc;
    }

    int do_rpc_service(FiniDevice::Request* req, FiniDevice::Response* resp, IOVector*, IStream*) {
        LOG_DEBUG("recieve finidevice request");
        if (device_) {
            delete device_;
            device_ = nullptr;
        }
        return 0;
    }

    int do_rpc_service(WritevBlocks::Request* req, WritevBlocks::Response* resp, IOVector*, IStream*) {
        LOG_DEBUG("recieve writevblocks request: ", VALUE(req->offset));
        return device_->pwritev(req->buf.begin(), req->buf.size(), req->offset);
    }

    int do_rpc_service(ReadvBlocks::Request* req, ReadvBlocks::Response* resp, IOVector* iov, IStream*) {
        LOG_DEBUG("recieve readvblocks request: ", VALUE(req->offset), VALUE(req->length));
        iov->push_back(req->length);
        resp->rc = device_->preadv(iov->iovec(), iov->iovcnt(), req->offset);
        if (resp->rc < 0) iov->shrink_to(0);
        resp->buf.assign(iov->iovec(), iov->iovcnt());
        return resp->rc;
    }

    int run() override {
        LOG_DEBUG("server run");

        if (device_type_ == DeviceType::kNVMeSSD) {
            if (nvme_env_init() != 0) LOG_ERROR_RETURN(0, -1, "failed to init nvme env");
        }
        DEFER({if (device_type_ == DeviceType::kNVMeSSD) nvme_env_fini();});

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
    enum DeviceType device_type_;
    photon::net::EndPoint ep_;
    std::unique_ptr<photon::net::ISocketServer> socket_server_;
    std::unique_ptr<photon::rpc::Skeleton> skeleton_;
    photon::fs::IFile* device_;
};

SPDKBDevPhotonServer* new_server(enum DeviceType type, std::string ip, uint16_t port) {
    return new RPCServer(type, ip, port);
}

}   // namespace spdk
}   // namespace photon