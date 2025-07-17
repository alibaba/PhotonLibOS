#include "bdev_photon.h"
#include "protocol.h"

#include "spdk/log.h"
#include "spdk/string.h"

#include <photon/photon.h>
#include <photon/net/socket.h>
#include <photon/rpc/rpc.h>
#include <photon/common/executor/executor.h>

#include <sys/epoll.h>
#include <unistd.h>

SPDK_LOG_REGISTER_COMPONENT(bdev_photon)

class RPCClient {
public:
    int do_rpc_init_device(photon::rpc::Stub* stub, const char* trid, uint32_t nsid, uint64_t num_blocks, uint32_t* block_size) {
        SPDK_DEBUGLOG(bdev_photon, "do_rpc_init_device, num_blocks=%lu\n", num_blocks);
        InitDevice::Request req;
        req.trid.assign(trid);
        req.nsid = nsid;
        req.num_blocks = num_blocks;

        InitDevice::Response resp;
        resp.rc = -1;
        resp.num_sectors = 0;
        resp.sector_size = 0;

        int rc = stub->call<InitDevice>(req, resp);
        if (rc < 0) return rc;

        *block_size = resp.sector_size;
        return resp.rc;
    }

    int do_rpc_fini_device(photon::rpc::Stub* stub) {
        FiniDevice::Request req;
        req.foo = 0;
        FiniDevice::Response resp;
        resp.rc = -1;
        int rc = stub->call<FiniDevice>(req, resp);
        if (rc < 0) return rc;
        return resp.rc;
    }

    int do_rpc_readv_writev_blocks(photon::rpc::Stub* stub, struct iovec* iov, int iovcnt, uint64_t offset, uint64_t length, bool is_write) {
        SPDK_DEBUGLOG(bdev_photon, "do_rpc_readv_writev_blocks, iovcnt=%d, offset=%lu, length=%lu, is_write=%d\n", iovcnt, offset, length, is_write);
        if (is_write) {
            WritevBlocks::Request req;
            req.offset = offset;
            req.buf.assign(iov, iovcnt);

            WritevBlocks::Response resp;
            resp.rc = -1;

            int rc = stub->call<WritevBlocks>(req, resp);
            if (rc < 0) return rc;
            return resp.rc;
        }
        else {
            ReadvBlocks::Request req;
            req.offset = offset;
            req.length = length;

            ReadvBlocks::Response resp;
            resp.rc = -1;
            resp.buf.assign(iov, iovcnt);

            int rc = stub->call<ReadvBlocks>(req, resp);
            if (rc < 0) return rc;
            return resp.rc;
        }
    }
};

struct photon_task_context {
    enum spdk_bdev_io_status status;
    spdk_thread* thread;
};

static void msg_fn(void* arg) {
    SPDK_DEBUGLOG(bdev_photon, "msg_fn begin\n");
    struct photon_task_context* task_ctx = (struct photon_task_context*)arg;
    struct spdk_bdev_io* bdev_io = spdk_bdev_io_from_ctx(task_ctx);
    spdk_bdev_io_complete(bdev_io, task_ctx->status);
    SPDK_DEBUGLOG(bdev_photon, "msg_fn done\n");
}

class BlockDevice {
public:
    BlockDevice() : executor_(new photon::Executor()) {}

    void init(const char* ip, uint16_t port, uint64_t expiration, uint64_t timeout) {
        ep_ = photon::net::EndPoint::parse(ip, port);
        expiration_ = expiration;
        timeout_ = timeout;
    }

    void init_device(const char* trid, uint32_t nsid, uint64_t num_blocks, uint32_t* block_size) {
        SPDK_DEBUGLOG(bdev_photon, "init_device begin\n");
        executor_->perform([=]{
            auto pool = get_stub_pool();
            auto stub = pool->get_stub(ep_, false);
            assert(stub != nullptr);
            rpc_client_.do_rpc_init_device(stub, trid, nsid, num_blocks, block_size);
            pool->put_stub(ep_, false);
        });
        SPDK_DEBUGLOG(bdev_photon, "init_device done, block_size=%u\n", *block_size);
    }

    void fini_device() {
        SPDK_DEBUGLOG(bdev_photon, "fini_device begin\n");
        executor_->perform([=]{
            auto pool = get_stub_pool();
            auto stub = pool->get_stub(ep_, false);
            assert(stub != nullptr);
            rpc_client_.do_rpc_fini_device(stub);
            pool->put_stub(ep_, false);
        });
        SPDK_DEBUGLOG(bdev_photon, "fini_device end\n");
    }

    void readv_writev_blocks(struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length, bool is_write, struct photon_task_context* task_ctx) {
        SPDK_DEBUGLOG(bdev_photon, "readv_writev_blocks, iovcnt=%d, offset=%lu, length=%lu, is_write=%d, task_ctx=%p\n", iovcnt, offset, length, is_write, task_ctx);
        executor_->async_perform(new auto([=]{
            auto pool = get_stub_pool();
            auto stub = pool->get_stub(ep_, false);
            assert(stub != nullptr);
            int rc = rpc_client_.do_rpc_readv_writev_blocks(stub, iov, iovcnt, offset, length, is_write);
            pool->put_stub(ep_, false);
            if (rc >= 0) task_ctx->status = SPDK_BDEV_IO_STATUS_SUCCESS;
            else task_ctx->status = SPDK_BDEV_IO_STATUS_FAILED;
            spdk_thread_send_msg(task_ctx->thread, msg_fn, task_ctx);
        }));
        SPDK_DEBUGLOG(bdev_photon, "readv_writev_blocks done, status=%d\n", task_ctx->status);
    }

private:
    static RPCClient rpc_client_;

    photon::net::EndPoint ep_;
    uint64_t expiration_;
    uint64_t timeout_;

    std::unique_ptr<photon::Executor> executor_;

    photon::rpc::StubPool* get_stub_pool() {
        thread_local std::unique_ptr<photon::rpc::StubPool> stub_pool;
        if (stub_pool == nullptr) {
            stub_pool.reset(photon::rpc::new_stub_pool(expiration_, timeout_));
            auto delete_stub_pool = [&]{ stub_pool.reset(); };
            photon::fini_hook(delete_stub_pool);
        }
        return stub_pool.get();
    }
};
RPCClient BlockDevice::rpc_client_;

static int bdev_photon_count = 0;

struct photon_bdev {
    struct spdk_bdev bdev;
    BlockDevice* device;
};

struct module_io_channel_context {};

struct disk_io_channel_context {
    struct module_io_channel_context *module_ioch_ctx;
};


static int bdev_photon_module_init(void);
static void bdev_photon_module_fini(void);

static int bdev_photon_get_ctx_size(void) {
    return sizeof(struct photon_task_context);
}

static struct spdk_bdev_module photon_if = {
    .module_init = bdev_photon_module_init,
    .module_fini = bdev_photon_module_fini,
    .name = "photon",
    .get_ctx_size = bdev_photon_get_ctx_size
};
SPDK_BDEV_MODULE_REGISTER(photon, &photon_if)



static int module_io_channel_create_cb(void *io_device, void *ctx_buf) {
    SPDK_DEBUGLOG(bdev_photon, "module_io_channel_create_cb\n");
    struct module_io_channel_context* module_ioch_ctx = (struct module_io_channel_context*)ctx_buf;
    return 0;
}

static void module_io_channel_destroy_cb(void *io_device, void *ctx_buf) {
    SPDK_DEBUGLOG(bdev_photon, "module_io_channel_destroy_cb\n");
    struct module_io_channel_context* module_ioch_ctx = (struct module_io_channel_context*)ctx_buf;
}


static int bdev_photon_module_init(void) {
    SPDK_DEBUGLOG(bdev_photon, "bdev_photon_module_init\n");
    spdk_io_device_register(&photon_if, module_io_channel_create_cb, module_io_channel_destroy_cb, sizeof(struct module_io_channel_context), "PhotonModule");
    return 0;
}

static void bdev_photon_module_fini(void) {
    SPDK_DEBUGLOG(bdev_photon, "bdev_photon_module_fini\n");
    spdk_io_device_unregister(&photon_if, NULL);
}




static int bdev_photon_destruct(void *ctx) {
    SPDK_DEBUGLOG(bdev_photon, "bdev_photon_destruct\n");

    struct photon_bdev *pt_bdev = (struct photon_bdev*)ctx;
    assert(pt_bdev != NULL);

    assert(pt_bdev->device != NULL);
    pt_bdev->device->fini_device();
    delete pt_bdev->device;

    spdk_io_device_unregister(pt_bdev, NULL);
    free(pt_bdev);

    SPDK_DEBUGLOG(bdev_photon, "bdev_photon_destruct done\n");
	return 0;
}

static void bdev_photon_rwv(struct photon_bdev* pt_bdev, struct photon_task_context* task_ctx, struct spdk_io_channel* ch, struct iovec* iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks, bool is_write) {
    SPDK_DEBUGLOG(bdev_photon, "bdev_photon_rwv, iovcnt=%d, offset_blocks=%lu, num_blocks=%lu, is_write=%d\n", iovcnt, offset_blocks, num_blocks, is_write);
    struct disk_io_channel_context* disk_ioch_ctx = (struct disk_io_channel_context*)spdk_io_channel_get_ctx(ch);
    assert(disk_ioch_ctx != NULL);
    assert(disk_ioch_ctx->module_ioch_ctx != NULL);
    assert(pt_bdev != NULL);
    assert(pt_bdev->device != NULL);
    pt_bdev->device->readv_writev_blocks(iov, iovcnt, offset_blocks * pt_bdev->bdev.blocklen, num_blocks * pt_bdev->bdev.blocklen, is_write, task_ctx);
}

static void bdev_photon_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io) {
    SPDK_DEBUGLOG(bdev_photon, "bdev_photon_submit_request, type is %d\n", bdev_io->type);

    struct photon_bdev* pt_bdev = (struct photon_bdev*)bdev_io->bdev->ctxt;
    struct photon_task_context* task_ctx = (struct photon_task_context*)bdev_io->driver_ctx;
    task_ctx->thread = spdk_get_thread();

    switch (bdev_io->type) {
    case SPDK_BDEV_IO_TYPE_READ:
        bdev_photon_rwv(
            pt_bdev, task_ctx, ch,
            bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
            bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
            false
        );
        break;
    case SPDK_BDEV_IO_TYPE_WRITE:
        bdev_photon_rwv(
            pt_bdev, task_ctx, ch,
            bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
            bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
            true
        );
        break;
    default:
        SPDK_ERRLOG("Unsupported io type %d\n", bdev_io->type);
        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
    }
}

static bool bdev_photon_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type) {
    SPDK_DEBUGLOG(bdev_photon, "bdev_photon_io_type_supported\n");
    switch (io_type)
    {
    case SPDK_BDEV_IO_TYPE_READ:
    case SPDK_BDEV_IO_TYPE_WRITE:
        return true;
    default:
        return false;
    }
}

static struct spdk_io_channel* bdev_photon_get_io_channel(void *io_device) {
    SPDK_DEBUGLOG(bdev_photon, "bdev_photon_get_io_channel\n");
    struct photon_bdev *pt_bdev = (struct photon_bdev*)io_device;
    return spdk_get_io_channel(pt_bdev);
}


static const struct spdk_bdev_fn_table bdev_photon_fn_table = {
	.destruct		= bdev_photon_destruct,
	.submit_request		= bdev_photon_submit_request,
	.io_type_supported	= bdev_photon_io_type_supported,
	.get_io_channel		= bdev_photon_get_io_channel
};


static int disk_io_channel_create_cb(void* io_device, void* ctx_buf) {
    SPDK_DEBUGLOG(bdev_photon, "disk_io_channel_create_cb\n");

    struct photon_bdev* pt_bdev = (struct photon_bdev*)io_device; (void)pt_bdev;

    struct disk_io_channel_context* disk_ioch_ctx = (struct disk_io_channel_context*)ctx_buf;
    disk_ioch_ctx->module_ioch_ctx = (struct module_io_channel_context*)spdk_io_channel_get_ctx(spdk_get_io_channel(&photon_if));

    return 0;
}

static void disk_io_channel_destroy_cb(void* io_device, void* ctx_buf) {
    SPDK_DEBUGLOG(bdev_photon, "disk_io_channel_destroy_cb\n");
    struct disk_io_channel_context* disk_ioch_ctx = (struct disk_io_channel_context*)ctx_buf;
    assert(disk_ioch_ctx->module_ioch_ctx != NULL);
    spdk_put_io_channel(spdk_io_channel_from_ctx(disk_ioch_ctx->module_ioch_ctx));
    return;
}

static void* create_rpc_client(void* arg) {
    SPDK_DEBUGLOG(bdev_photon, "create_rpc_client begin\n");
    struct photon_bdev* pt_bdev = (struct photon_bdev*)arg;
    assert(pt_bdev->device == NULL);
    pt_bdev->device = new BlockDevice();
    SPDK_DEBUGLOG(bdev_photon, "create_rpc_client done\n");
    return nullptr;
}

extern "C" int bdev_photon_create(struct spdk_bdev **bdev, const char* trid, uint32_t nsid, uint64_t num_blocks, const char* ip, uint16_t port, uint64_t expiration, uint64_t timeout) {
    SPDK_DEBUGLOG(bdev_photon, "bdev_photon_create\n");

    struct photon_bdev* pt_bdev = (struct photon_bdev* )calloc(1, sizeof(struct photon_bdev));
    if (!bdev) {
        SPDK_ERRLOG("bdev_photon_create: calloc failed\n");
        return -ENOMEM;
    }

    spdk_call_unaffinitized(create_rpc_client, pt_bdev);
    assert(pt_bdev->device != NULL);

    uint32_t block_size = 0;
    pt_bdev->device->init(ip, port, expiration, timeout);
    pt_bdev->device->init_device(trid, nsid, num_blocks, &block_size);
    if (block_size == 0) {
        SPDK_ERRLOG("bdev_photon_create: init_device failed\n");
        return -1;
    }

    pt_bdev->bdev.name = spdk_sprintf_alloc("Photon%d", bdev_photon_count);
    pt_bdev->bdev.product_name = strdup("Photon implemented Disk");
    pt_bdev->bdev.write_cache = 0;
    pt_bdev->bdev.blocklen = block_size;
    pt_bdev->bdev.blockcnt = num_blocks;
    pt_bdev->bdev.fn_table = &bdev_photon_fn_table;
    pt_bdev->bdev.module = &photon_if;
    pt_bdev->bdev.ctxt = pt_bdev;
    spdk_uuid_generate(&pt_bdev->bdev.uuid);

    bdev_photon_count++;

    spdk_io_device_register(pt_bdev, disk_io_channel_create_cb, disk_io_channel_destroy_cb, sizeof(struct disk_io_channel_context), "PhotonDisk");

    int rc = spdk_bdev_register(&pt_bdev->bdev);
    if (rc) {
        free(pt_bdev);
        return rc;
    }

    *bdev = &pt_bdev->bdev;

    return 0;
}

extern "C" void bdev_photon_delete(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg) {
    SPDK_DEBUGLOG(bdev_photon, "bdev_photon_delete\n");

    if (!bdev || bdev->module != &photon_if) {
        cb_fn(cb_arg, -ENODEV);
        return;
    }

    spdk_bdev_unregister(bdev, cb_fn, cb_arg);
}