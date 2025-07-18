#ifndef SPDK_BDEV_PHOTON_PROTOCOL_H
#define SPDK_BDEV_PHOTON_PROTOCOL_H

#include <photon/rpc/rpc.h>

struct Operation {
    const static uint32_t IID = 0x111;
};

struct InitDevice: public Operation {
    const static uint32_t FID = 0x777;

    struct Request : public photon::rpc::Message {
        photon::rpc::string trid;
        uint32_t nsid;
        uint64_t num_blocks;
        PROCESS_FIELDS(trid, nsid, num_blocks);
    };

    struct Response : public photon::rpc::Message {
        int rc;
        uint32_t sector_size;
        uint64_t num_sectors;
        PROCESS_FIELDS(rc, sector_size, num_sectors);
    };
};

struct FiniDevice: public Operation {
    const static uint32_t FID = 0x999;

    struct Request : public photon::rpc::Message {
        int foo;
        PROCESS_FIELDS(foo);
    };

    struct Response : public photon::rpc::Message {
        int rc;
        PROCESS_FIELDS(rc);
    };
};

struct WritevBlocks: public Operation {
    const static uint32_t FID = 0x333;

    struct Request : public photon::rpc::Message {
        uint64_t offset;
        photon::rpc::aligned_iovec_array buf;
        PROCESS_FIELDS(offset, buf);
    };

    struct Response : public photon::rpc::Message {
        int rc;
        PROCESS_FIELDS(rc);
    };
};

struct ReadvBlocks: public Operation {
    const static uint32_t FID = 0x555;

    struct Request : public photon::rpc::Message {
        uint64_t offset;
        uint64_t length;
        PROCESS_FIELDS(offset);
    };

    struct Response : public photon::rpc::Message {
        int rc;
        photon::rpc::aligned_iovec_array buf;
        PROCESS_FIELDS(rc, buf);
    };
};

#endif  /* SPDK_BDEV_PHOTON_PROTOCOL_H */