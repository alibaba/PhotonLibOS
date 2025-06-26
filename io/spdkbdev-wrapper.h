#pragma once

#include <spdk/bdev.h>
#include <spdk/env.h>
#include <spdk/event.h>

// dpdk include syslog.h lead to macro defination conflict with alog
// so undefine the macro in syslog.h temporarily here
#ifdef LOG_INFO
#undef LOG_INFO
#endif
#ifdef LOG_DEBUG
#undef LOG_DEBUG
#endif

#include <photon/photon.h>
#include <photon/thread/awaiter.h>
#include <photon/common/tuple-assistance.h>


namespace photon {

namespace spdk {

void bdev_env_init(int argc, char** argv);
void bdev_env_init(const char* json_cfg_path);

void bdev_env_fini();

int bdev_open_ext(const char* bdev_name, bool write, struct spdk_bdev_desc** desc);

void bdev_close(struct spdk_bdev_desc* desc);

struct spdk_io_channel* bdev_get_io_channel(struct spdk_bdev_desc* desc);

void bdev_put_io_channel(struct spdk_io_channel* ch);


int bdev_read(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
            void *buf, uint64_t offset, uint64_t nbytes);

int bdev_read_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
            void *buf, uint64_t offset_blocks, uint64_t num_blocks);

int bdev_readv(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
            struct iovec *iov, int iovcnt,
		    uint64_t offset, uint64_t nbytes);

int bdev_readv_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
            struct iovec *iov, int iovcnt,
            uint64_t offset_blocks, uint64_t num_blocks);

int bdev_write(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
            void *buf, uint64_t offset, uint64_t nbytes);

int bdev_write_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			void *buf, uint64_t offset_blocks, uint64_t num_blocks);

int bdev_writev(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    struct iovec *iov, int iovcnt,
		    uint64_t offset, uint64_t len);

int bdev_writev_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			struct iovec *iov, int iovcnt,
			uint64_t offset_blocks, uint64_t num_blocks);


extern spdk_thread* g_app_thread;

struct _MsgCtxBase {
    Awaiter<PhotonContext> awaiter;
    bool success = false;
    int rc = 0;
    static void cb_fn(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);    // spdk_bdev_io_completion_cb
};

// F must be able to invoke as func(args..., spdk_bdev_io_completion_cb cb, void* ctx);
// and cb will be _MsgCtxBase::cb_fn
template <typename F, typename... Args>
int bdev_call(F func, Args... args) {
    struct MsgCtx : public _MsgCtxBase {
        F func;
        std::tuple<Args...> args;
        MsgCtx(F func, Args... args) : func(func),
            args(std::forward<Args>(args)...) { }
    };

    auto msg_fn = [](void* msg_ctx) {                   // spdk_msg_fn
        auto ctx = static_cast<MsgCtx*>(msg_ctx);
        int rc = tuple_assistance::apply([&](Args... args){
            return ctx->func(args..., ctx->cb_fn, ctx);
        }, ctx->args);

        ctx->rc = rc;
        if (rc != 0) {
            ctx->awaiter.resume();
        }
    };

    MsgCtx ctx(func, args...);
    spdk_thread_send_msg(g_app_thread, msg_fn, &ctx);
    ctx.awaiter.suspend();
    return ctx.rc;
}

}   // namespace spdk
}   // namespace photon