#include "bdev_photon.h"

#include "spdk/log.h"
#include "spdk/string.h"

#include <photon/photon.h>
#include <photon/net/socket.h>
#include <photon/rpc/rpc.h>
#include <photon/thread/thread11.h>
#include <photon/thread/workerpool.h>

#include <sys/epoll.h>
#include <unistd.h>

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

class RPCClient {
public:
    RPCClient(std::string ip="127.0.0.1", uint16_t port=43548, uint64_t expiration=10UL * 1000 * 1000, uint64_t timeout=1UL * 1000 * 1000)
    :
    ep_(ip.c_str(), port),
    expiration_(expiration), timeout_(timeout),
    stub_pool_(photon::rpc::new_stub_pool(expiration_, timeout_))
    {
        SPDK_NOTICELOG("RPCClient construct\n");
    }

    ~RPCClient() {
        SPDK_NOTICELOG("RPCClient destruct begin\n");
        stub_pool_.reset();
        SPDK_NOTICELOG("RPCClient destruct done\n");
    }

    int do_rpc_init_device(uint64_t num_blocks, uint32_t* block_size) {
        SPDK_NOTICELOG("do_rpc_init_device, num_blocks=%lu\n", num_blocks);
        InitDevice::Request req;
        req.trid = "trtype:pcie traddr:0000:86:00.0";
        req.nsid = 1;
        req.num_blocks = num_blocks;

        InitDevice::Response resp;
        resp.rc = -1;
        resp.num_sectors = 0;
        resp.sector_size = 0;

        auto stub = stub_pool_->get_stub(ep_, false);
        assert(stub != nullptr);
        DEFER(stub_pool_->put_stub(ep_, false));

        int rc = stub->call<InitDevice>(req, resp);
        if (rc < 0) return rc;

        *block_size = resp.sector_size;
        return resp.rc;
    }

    int do_rpc_writev_blocks(struct iovec* iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks) {
        SPDK_NOTICELOG("do_rpc_writev_blocks\n");
        WritevBlocks::Request req;
        req.offset_blocks = offset_blocks;
        req.num_blocks = num_blocks;
        req.buf.assign(iov, iovcnt);

        WritevBlocks::Response resp;
        resp.rc = -1;

        auto stub = stub_pool_->get_stub(ep_, false);
        assert(stub != nullptr);
        DEFER(stub_pool_->put_stub(ep_, false));

        int rc = stub->call<WritevBlocks>(req, resp);
        if (rc < 0) return rc;
        return resp.rc;
    }

    int do_rpc_readv_blocks(struct iovec* iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks) {
        SPDK_NOTICELOG("do_rpc_readv_blocks, iovcnt=%d, offset_blocks=%lu, num_blocks=%lu\n", iovcnt, offset_blocks, num_blocks);
        ReadvBlocks::Request req;
        req.offset_blocks = offset_blocks;
        req.num_blocks = num_blocks;

        ReadvBlocks::Response resp;
        resp.rc = -1;
        resp.buf.assign(iov, iovcnt);

        auto stub = stub_pool_->get_stub(ep_, false);
        assert(stub != nullptr);
        DEFER(stub_pool_->put_stub(ep_, false));

        int rc = stub->call<ReadvBlocks>(req, resp);
        if (rc < 0) return rc;
        return resp.rc;
    }

private:
    photon::net::EndPoint ep_;
    uint64_t expiration_;
    uint64_t timeout_;
    std::unique_ptr<photon::rpc::StubPool> stub_pool_;
};

struct photon_task_context {
    enum spdk_bdev_io_status status;
};

class Client {
public:
    Client() : wp_(new photon::WorkPool(0)) {
        SPDK_NOTICELOG("Client construct begin\n");
        sem_t sem;
        sem_init(&sem, 0, 0);
        for (int i=0; i<1; i++) {
            ths_.emplace_back([this](sem_t* sem){
                SPDK_NOTICELOG("wp thread init begin\n");
                photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_LIBAIO);
                DEFER({
                    SPDK_NOTICELOG("photon fini begin\n");
                    photon::fini();
                    SPDK_NOTICELOG("photon fini done\n");
                });
                assert(rpc_client_ == NULL);
                rpc_client_ = std::make_unique<RPCClient>();
                auto delete_rpc_client = [this]() {
                    SPDK_NOTICELOG("fini hook begin\n");
                    if (rpc_client_ != NULL) {
                        SPDK_NOTICELOG("rpc client not null\n");
                    }
                    else {
                        SPDK_NOTICELOG("error: rpc client null\n");
                    }
                    rpc_client_.reset();
                    SPDK_NOTICELOG("fini hook done\n");
                };
                photon::fini_hook(delete_rpc_client);
                sem_post(sem);
                SPDK_NOTICELOG("wp thread init done\n");
                wp_->join_current_vcpu_into_workpool();
            }, &sem);
        }
        sem_wait(&sem);
        SPDK_NOTICELOG("Client construct done\n");
    }

    ~Client() {
        SPDK_NOTICELOG("Client destruct begin\n");
        wp_.reset();
        SPDK_NOTICELOG("Client destruct after wp reset\n");
        for (auto& th: ths_) th.join();
        SPDK_NOTICELOG("Client destruct done\n");
    }

    int init_device(uint64_t num_blocks, uint32_t* block_size) {
        SPDK_NOTICELOG("init_device\n");
        wp_->call<photon::StdContext>([=]{
            SPDK_NOTICELOG("init_device, get into wp call, before assert check\n");
            assert(rpc_client_ != NULL);
            SPDK_NOTICELOG("init_device, get into wp call, after assert check\n");
            rpc_client_->do_rpc_init_device(num_blocks, block_size);
            SPDK_NOTICELOG("init_device, rpc done, block_size=%u\n", *block_size);
        });
        return 0;
    }

    int readv_writev_blocks(struct iovec *iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks, bool is_write, struct photon_task_context* task_ctx, int pipe_write_fd) {
        SPDK_NOTICELOG("readv_writev_blocks, iovcnt=%d, offset_blocks=%lu, num_blocks=%lu, is_write=%d, task_ctx=%p\n", iovcnt, offset_blocks, num_blocks, is_write, task_ctx);

        auto func = [](struct iovec *iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks, bool is_write, struct photon_task_context* task_ctx, int pipe_write_fd){
            SPDK_NOTICELOG("readv_writev_blocks, get into wp call, before assert check\n");
            assert(rpc_client_ != NULL);
            SPDK_NOTICELOG("readv_writev_blocks, get into wp call, after assert check\n");
            int rc = 0;
            if (is_write) {
                rc = rpc_client_->do_rpc_writev_blocks(iov, iovcnt, offset_blocks, num_blocks);
            }
            else {
                rc = rpc_client_->do_rpc_readv_blocks(iov, iovcnt, offset_blocks, num_blocks);
            }
            SPDK_NOTICELOG("readv_writev_blocks, rpc done\n");
            if (rc >= 0) {
                SPDK_NOTICELOG("readv_writev_blocks, rpc success, task_ctx=%p\n", task_ctx);
                task_ctx->status = SPDK_BDEV_IO_STATUS_SUCCESS;
            }
            else {
                SPDK_NOTICELOG("readv_writev_blocks, rpc failed, task_ctx=%p\n", task_ctx);
                task_ctx->status = SPDK_BDEV_IO_STATUS_FAILED;
            }
            if (write(pipe_write_fd, (void*)&task_ctx, sizeof(void*)) != sizeof(void*))
            SPDK_NOTICELOG("readv_writev_blocks, eventfd_write done\n");
        };

        wp_->async_call(new auto(std::bind(func, iov, iovcnt, offset_blocks, num_blocks, is_write, task_ctx, pipe_write_fd)));

        return 0;
    }

private:
    static thread_local std::unique_ptr<RPCClient> rpc_client_;
    std::unique_ptr<photon::WorkPool> wp_;
    std::vector<std::thread> ths_;
};
thread_local std::unique_ptr<RPCClient> Client::rpc_client_;

static int bdev_photon_count = 0;

struct photon_bdev {
    struct spdk_bdev bdev;
    Client* client;
};

struct module_io_channel_context {
    int epoll_fd;
    struct spdk_poller* poller;
};

struct disk_io_channel_context {
    int pipe_fd[2];
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



static const int MAX_EVENTS_PER_POLL = 128;

static int module_poller_body(void* arg) {
    struct module_io_channel_context* module_ioch_ctx = (struct module_io_channel_context*)arg;

    struct epoll_event events[MAX_EVENTS_PER_POLL];
    int num_events = epoll_wait(module_ioch_ctx->epoll_fd, events, MAX_EVENTS_PER_POLL, 0);
    if (num_events <= 0) {
		return SPDK_POLLER_IDLE;
	}

	for (int i = 0; i < num_events; i++) {
        SPDK_NOTICELOG("module_poller_body, i=%d, num_events=%d\n", i, num_events);
        struct disk_io_channel_context* disk_ioch_ctx = (struct disk_io_channel_context*)events[i].data.ptr;
        void* buf = nullptr;
        int rc = read(disk_ioch_ctx->pipe_fd[0], &buf, sizeof(void*));
        if (rc == sizeof(void*)) {
            SPDK_NOTICELOG("rc == sizeof(void*), rc=%d, task_ctx=%p\n", rc, buf);
            struct photon_task_context* task_ctx = (struct photon_task_context*)buf;
            assert(task_ctx != nullptr);
            struct spdk_bdev_io* bdev_io = spdk_bdev_io_from_ctx(task_ctx);
            assert(bdev_io != nullptr);
            spdk_bdev_io_complete(bdev_io, task_ctx->status);
        }
        else {
            SPDK_ERRLOG("rc != sizeof(void*), rc=%d\n", rc);
        }
	}

	return SPDK_POLLER_BUSY;
}

static int module_io_channel_create_cb(void *io_device, void *ctx_buf) {
    SPDK_INFOLOG(bdev_photon, "module_io_channel_create_cb\n");

    struct module_io_channel_context* module_ioch_ctx = (struct module_io_channel_context*)ctx_buf;

    module_ioch_ctx->epoll_fd = epoll_create1(0);
    if (module_ioch_ctx->epoll_fd < 0) {
        SPDK_ERRLOG("bdev_photon: epoll_create1 failed\n");
        return -1;
    }

    module_ioch_ctx->poller = SPDK_POLLER_REGISTER(module_poller_body, module_ioch_ctx, 0);

    return 0;
}

static void module_io_channel_destroy_cb(void *io_device, void *ctx_buf) {
    SPDK_INFOLOG(bdev_photon, "module_io_channel_destroy_cb\n");
    struct module_io_channel_context* module_ioch_ctx = (struct module_io_channel_context*)ctx_buf;
    if (module_ioch_ctx->epoll_fd >= 0) {
		close(module_ioch_ctx->epoll_fd);
	}
    spdk_poller_unregister(&module_ioch_ctx->poller);
}


static int bdev_photon_module_init(void) {
    SPDK_INFOLOG(bdev_photon, "bdev_photon_module_init\n");
    spdk_io_device_register(&photon_if, module_io_channel_create_cb, module_io_channel_destroy_cb, sizeof(struct module_io_channel_context), "PhotonModule");
    return 0;
}

static void bdev_photon_module_fini(void) {
    SPDK_INFOLOG(bdev_photon, "bdev_photon_module_fini\n");
    spdk_io_device_unregister(&photon_if, NULL);
}




static int bdev_photon_destruct(void *ctx) {
    SPDK_INFOLOG(bdev_photon, "bdev_photon_destruct\n");

    struct photon_bdev *pt_bdev = (struct photon_bdev*)ctx;

    assert(pt_bdev != NULL);
    assert(pt_bdev->client != NULL);
    delete pt_bdev->client;

    spdk_io_device_unregister(pt_bdev, NULL);
    free(pt_bdev);

    SPDK_INFOLOG(bdev_photon, "bdev_photon_destruct done\n");
	return 0;
}

static void bdev_photon_rwv(struct photon_bdev* pt_bdev, struct photon_task_context* task_ctx, struct spdk_io_channel* ch, struct iovec* iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks, bool is_write) {
    SPDK_INFOLOG(bdev_photon, "bdev_photon_rwv, iovcnt=%d, offset_blocks=%lu, num_blocks=%lu, is_write=%d\n", iovcnt, offset_blocks, num_blocks, is_write);
    struct disk_io_channel_context* disk_ioch_ctx = (struct disk_io_channel_context*)spdk_io_channel_get_ctx(ch);
    assert(disk_ioch_ctx != NULL);
    assert(disk_ioch_ctx->module_ioch_ctx != NULL);
    assert(pt_bdev != NULL);
    assert(pt_bdev->client != NULL);
    pt_bdev->client->readv_writev_blocks(iov, iovcnt, offset_blocks, num_blocks, is_write, task_ctx, disk_ioch_ctx->pipe_fd[1]);
}

static void bdev_photon_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io) {
    SPDK_INFOLOG(bdev_photon, "bdev_photon_submit_request, type is %d\n", bdev_io->type);

    switch (bdev_io->type) {
    case SPDK_BDEV_IO_TYPE_READ:
        bdev_photon_rwv(
            (struct photon_bdev*)bdev_io->bdev->ctxt,
            (struct photon_task_context*)bdev_io->driver_ctx,
            ch,
            bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
            bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
            false
        );
        break;
    case SPDK_BDEV_IO_TYPE_WRITE:
        bdev_photon_rwv(
            (struct photon_bdev*)bdev_io->bdev->ctxt,
            (struct photon_task_context*)bdev_io->driver_ctx,
            ch,
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
    SPDK_INFOLOG(bdev_photon, "bdev_photon_io_type_supported\n");
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
    SPDK_INFOLOG(bdev_photon, "bdev_photon_get_io_channel\n");
    struct photon_bdev *pt_bdev = (struct photon_bdev*)io_device;
    return spdk_get_io_channel(pt_bdev);
}

static void bdev_photon_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w) {
    SPDK_INFOLOG(bdev_photon, "bdev_photon_write_json_config\n");
    return;
}

static const struct spdk_bdev_fn_table bdev_photon_fn_table = {
	.destruct		= bdev_photon_destruct,
	.submit_request		= bdev_photon_submit_request,
	.io_type_supported	= bdev_photon_io_type_supported,
	.get_io_channel		= bdev_photon_get_io_channel,
	.write_config_json	= bdev_photon_write_json_config,
};


static int disk_io_channel_create_cb(void* io_device, void* ctx_buf) {
    SPDK_INFOLOG(bdev_photon, "disk_io_channel_create_cb\n");

    struct photon_bdev* pt_bdev = (struct photon_bdev*)io_device; (void)pt_bdev;

    struct disk_io_channel_context* disk_ioch_ctx = (struct disk_io_channel_context*)ctx_buf;
    disk_ioch_ctx->module_ioch_ctx = (struct module_io_channel_context*)spdk_io_channel_get_ctx(spdk_get_io_channel(&photon_if));

    if (pipe(disk_ioch_ctx->pipe_fd) < 0) {
        SPDK_ERRLOG("bdev_photon: failed to create pipe_fd\n");
        spdk_put_io_channel(spdk_io_channel_from_ctx(disk_ioch_ctx->module_ioch_ctx));
        return -1;
    }

    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.ptr = disk_ioch_ctx;

    if (epoll_ctl(disk_ioch_ctx->module_ioch_ctx->epoll_fd, EPOLL_CTL_ADD, disk_ioch_ctx->pipe_fd[0], &event) < 0) {
        SPDK_ERRLOG("bdev_photon: epoll_ctl failed\n");
        close(disk_ioch_ctx->pipe_fd[0]);
        close(disk_ioch_ctx->pipe_fd[1]);
        spdk_put_io_channel(spdk_io_channel_from_ctx(disk_ioch_ctx->module_ioch_ctx));
        return -1;
    }

    return 0;
}

static void disk_io_channel_destroy_cb(void* io_device, void* ctx_buf) {
    SPDK_INFOLOG(bdev_photon, "disk_io_channel_destroy_cb\n");
    struct disk_io_channel_context* disk_ioch_ctx = (struct disk_io_channel_context*)ctx_buf;
    assert(disk_ioch_ctx->module_ioch_ctx != NULL);
    if (epoll_ctl(disk_ioch_ctx->module_ioch_ctx->epoll_fd, EPOLL_CTL_DEL, disk_ioch_ctx->pipe_fd[0], NULL) < 0) {
        SPDK_ERRLOG("epoll_ctl EPOLL_CTL_DEL failed\n");
    }
    close(disk_ioch_ctx->pipe_fd[0]);
    close(disk_ioch_ctx->pipe_fd[1]);
    spdk_put_io_channel(spdk_io_channel_from_ctx(disk_ioch_ctx->module_ioch_ctx));
    return;
}

static void* create_rpc_client(void* arg) {
    SPDK_INFOLOG(bdev_photon, "create_rpc_client begin\n");
    struct photon_bdev* pt_bdev = (struct photon_bdev*)arg;
    assert(pt_bdev->client == NULL);
    pt_bdev->client = new Client();
    SPDK_INFOLOG(bdev_photon, "create_rpc_client done\n");
    return nullptr;
}

extern "C" int bdev_photon_create(struct spdk_bdev **bdev, uint64_t num_blocks) {
    SPDK_INFOLOG(bdev_photon, "bdev_photon_create\n");

    struct photon_bdev* pt_bdev = (struct photon_bdev* )calloc(1, sizeof(struct photon_bdev));
    if (!bdev) {
        SPDK_ERRLOG("bdev_photon_create: calloc failed\n");
        return -ENOMEM;
    }

    spdk_call_unaffinitized(create_rpc_client, pt_bdev);
    assert(pt_bdev->client != NULL);

    uint32_t block_size = 0;
    pt_bdev->client->init_device(num_blocks, &block_size);
    if (block_size == 0) {
        SPDK_ERRLOG("bdev_photon_create: init_device failed\n");
        return -1;
    }

    pt_bdev->bdev.name = spdk_sprintf_alloc("Photon%d", bdev_photon_count);
    pt_bdev->bdev.product_name = strdup("Photon implemented Disk");
    pt_bdev->bdev.write_cache = 0;
    pt_bdev->bdev.blocklen = 512;
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
    SPDK_INFOLOG(bdev_photon, "bdev_photon_delete\n");

    if (!bdev || bdev->module != &photon_if) {
        cb_fn(cb_arg, -ENODEV);
        return;
    }

    spdk_bdev_unregister(bdev, cb_fn, cb_arg);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_photon)