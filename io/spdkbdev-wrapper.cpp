#include "spdkbdev-wrapper.h"
#include <photon/common/alog-stdstring.h>

namespace photon {
namespace spdk {

spdk_thread* g_app_thread;
static std::thread* bg_thread;

static void bdev_env_init_impl(struct spdk_app_opts* opts) {
    struct MsgCtx : public _MsgCtxBase {
        struct spdk_app_opts* opts;
        MsgCtx(struct spdk_app_opts* opts) : opts(opts) {}
    };

    MsgCtx ctx(opts);

    bg_thread = new std::thread([](MsgCtx* ctx) {
        auto start_fn = [](void* arg) {
            auto ctx = static_cast<MsgCtx*>(arg);
            g_app_thread = spdk_get_thread();
            ctx->awaiter.resume();
        };

        int rc = spdk_app_start(ctx->opts, start_fn, ctx);
        LOG_DEBUG("spdk app start ", VALUE(rc));
        if (rc != 0) {
            ctx->awaiter.resume();
        }
        else {
            spdk_app_fini();
        }
    }, &ctx);

    LOG_DEBUG("bdev env init before wait");
    ctx.awaiter.suspend();
    LOG_DEBUG("bdev env init after wait");
}

void bdev_env_init(int argc, char** argv) {
    struct spdk_app_opts opts;
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "photon_spdk_bdev";

    if (spdk_app_parse_args(argc, argv, &opts, nullptr, nullptr, nullptr, nullptr) != SPDK_APP_PARSE_ARGS_SUCCESS) {
        exit(-1);
    }

    bdev_env_init_impl(&opts);
}

void bdev_env_init(const char* json_cfg_path) {
    struct spdk_app_opts opts;
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "photon_spdk_bdev";
    opts.json_config_file = json_cfg_path;

    bdev_env_init_impl(&opts);
}

void bdev_env_fini() {
    LOG_DEBUG("bdev env fini get into");

    spdk_thread_send_critical_msg(g_app_thread, [](void* arg){
        spdk_app_stop(0);
    });

    if (bg_thread) {
        bg_thread->join();
        delete bg_thread;
    }

    LOG_DEBUG("bdev env fini exit");
}

static int _open_ext(const char* bdev_name, bool write,
        struct spdk_bdev_desc** desc, spdk_bdev_io_completion_cb, void* ctx) {
    auto cb = [](enum spdk_bdev_event_type, struct spdk_bdev*, void*) { };
    int ret = spdk_bdev_open_ext(bdev_name, write, cb, nullptr, desc);
    static_cast<_MsgCtxBase*>(ctx)->awaiter.resume();
    return ret;
}

int bdev_open_ext(const char* bdev_name, bool write, struct spdk_bdev_desc** desc) {
    return bdev_call(&_open_ext, bdev_name, write, desc);
}

static int _get_io_channel(struct spdk_bdev_desc* desc, struct spdk_io_channel** ch,
                    spdk_bdev_io_completion_cb, void* ctx) {
    *ch = spdk_bdev_get_io_channel(desc);
    static_cast<_MsgCtxBase*>(ctx)->awaiter.resume();
    return 0;
}

struct spdk_io_channel* bdev_get_io_channel(spdk_bdev_desc* desc) {
    struct spdk_io_channel* ch = nullptr;
    bdev_call(&_get_io_channel, desc, &ch);
    return ch;
}

static int _close(struct spdk_bdev_desc* desc, spdk_bdev_io_completion_cb, void* ctx) {
    spdk_bdev_close(desc);
    static_cast<_MsgCtxBase*>(ctx)->awaiter.resume();
    return 0;
}

void bdev_close(struct spdk_bdev_desc* desc) {
    bdev_call(&_close, desc);
}

static int _put_io_channel(struct spdk_io_channel* ch, spdk_bdev_io_completion_cb, void* ctx) {
    spdk_put_io_channel(ch);
    static_cast<_MsgCtxBase*>(ctx)->awaiter.resume();
    return 0;
}

void bdev_put_io_channel(struct spdk_io_channel* ch) {
    bdev_call(&_put_io_channel, ch);
}

int bdev_read(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    void *buf, uint64_t offset, uint64_t nbytes)
{
    return bdev_call(&spdk_bdev_read, desc, ch, buf, offset, nbytes);
}

int bdev_read_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			void *buf, uint64_t offset_blocks, uint64_t num_blocks)
{
    return bdev_call(&spdk_bdev_read_blocks, desc, ch, buf, offset_blocks, num_blocks);
}

int bdev_readv(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    struct iovec *iov, int iovcnt,
		    uint64_t offset, uint64_t nbytes)
{
    return bdev_call(&spdk_bdev_readv, desc, ch, iov, iovcnt, offset, nbytes);
}

int bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			struct iovec *iov, int iovcnt,
			uint64_t offset_blocks, uint64_t num_blocks)
{
    return bdev_call(&spdk_bdev_readv_blocks, desc, ch, iov, iovcnt, offset_blocks, num_blocks);

}

int bdev_write(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    void *buf, uint64_t offset, uint64_t nbytes)
{
    return bdev_call(&spdk_bdev_write, desc, ch, buf, offset, nbytes);
}

int bdev_write_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			void *buf, uint64_t offset_blocks, uint64_t num_blocks)
{
    return bdev_call(&spdk_bdev_write_blocks, desc, ch, buf, offset_blocks, num_blocks);
}

int bdev_writev(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    struct iovec *iov, int iovcnt,
		    uint64_t offset, uint64_t len)
{
    return bdev_call(&spdk_bdev_writev, desc, ch, iov, iovcnt, offset, len);
}

int bdev_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			struct iovec *iov, int iovcnt,
			uint64_t offset_blocks, uint64_t num_blocks)
{
    return bdev_call(&spdk_bdev_writev_blocks, desc, ch, iov, iovcnt, offset_blocks, num_blocks);
}

void _MsgCtxBase::cb_fn(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg) {
    spdk_bdev_free_io(bdev_io);
    auto ctx = static_cast<_MsgCtxBase*>(cb_arg);
    ctx->success = success;
    LOG_DEBUG("bdev_io_completion_cb: before resume");
    ctx->awaiter.resume();
    LOG_DEBUG("bdev_io_completion_cb: after resume");
}

}   // namespace spdk
}   // namespace photon