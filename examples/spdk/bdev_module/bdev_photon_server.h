#pragma once

#include <cstdint>
#include <string>

namespace photon {
namespace spdk {

class BlockDevice {
public:
    virtual ~BlockDevice() = default;
    virtual int Init(const char* trid_str, uint32_t nsid, uint64_t num_blocks, uint32_t* sector_size, uint64_t* num_sectors) = 0;
    virtual int Fini() = 0;
    virtual int Writev(struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count) = 0;
    virtual int Readv(struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count) = 0;
protected:
    uint64_t num_blocks_ = 0;   // setting

    uint32_t sector_size_ = 0;
    uint64_t num_sectors_ = 0;  // real
};

class SPDKBDevPhotonServer {
public:
    SPDKBDevPhotonServer(BlockDevice* device) : device_(device) {}
    virtual ~SPDKBDevPhotonServer() = default;
    virtual int run() = 0;
protected:
    BlockDevice* device_ = nullptr;
};

BlockDevice* new_blkdev_local_nvme_ssd();

BlockDevice* new_blkdev_localfs();

SPDKBDevPhotonServer* new_server(BlockDevice* device, std::string ip="127.0.0.1", uint16_t port=43548);

}   // namespace spdk
}   // namespace photon