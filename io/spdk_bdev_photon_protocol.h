#pragma once

#include <photon/rpc/rpc.h>

namespace photon {
namespace spdk {

struct Operation {
    const static uint32_t IID = 0x111;
};

struct InitDevice: public Operation {
    const static uint32_t FID = 0x777;

    struct Request : public photon::rpc::Message {
        photon::rpc::string trid;
        uint32_t nsid;
        PROCESS_FIELDS(trid, nsid);
    };

    struct Response : public photon::rpc::Message {
        int rc;
        uint32_t sector_size;
        uint64_t num_sectors;
        PROCESS_FIELDS(rc, sector_size, num_sectors);
    };
};

struct WritevBlocks: public Operation {
    const static uint32_t FID = 0x333;

    struct Request : public photon::rpc::Message {
        uint64_t offset_blocks;
        uint64_t num_blocks;
        photon::rpc::aligned_iovec_array buf;
        PROCESS_FIELDS(offset_blocks, num_blocks, buf);
    };

    struct Response : public photon::rpc::Message {
        int rc;
        PROCESS_FIELDS(rc);
    };
};

struct ReadvBlocks: public Operation {
    const static uint32_t FID = 0x555;

    struct Request : public photon::rpc::Message {
        uint64_t offset_blocks;
        uint64_t num_blocks;
        PROCESS_FIELDS(offset_blocks, num_blocks);
    };

    struct Response : public photon::rpc::Message {
        int rc;
        photon::rpc::aligned_iovec_array buf;
        PROCESS_FIELDS(rc, buf);
    };
};

}   // namespace spdk
}   // namespace photon