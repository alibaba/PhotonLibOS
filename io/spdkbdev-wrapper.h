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

#include <photon/common/alog-stdstring.h>
#include <photon/common/tuple-assistance.h>

#include <photon/photon.h>
#include <photon/thread/awaiter.h>

#include <string>
#include <tuple>


namespace photon {

namespace spdk {

void bdev_env_init(int argc, char** argv);
void bdev_env_init(const char* json_cfg_path);

void bdev_env_fini();

int bdev_open_ext(const char* bdev_name, bool write, struct spdk_bdev_desc** desc);

struct spdk_io_channel* bdev_get_io_channel(struct spdk_bdev_desc* desc);

void bdev_put_io_channel(struct spdk_io_channel* ch);

void bdev_close(struct spdk_bdev_desc* desc);


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



// internal

extern spdk_thread* g_app_thread;

template <typename F, typename... Args>
struct MsgCtx {
    F func;
    struct spdk_bdev_desc* desc;
    struct spdk_io_channel* ch;
    bool* success;
    int* rc;
    Awaiter<PhotonContext> awaiter;
    std::tuple<Args...> args;

    template <typename FuncType, typename... ArgsType>
    MsgCtx(FuncType func, struct spdk_bdev_desc* desc, struct spdk_io_channel* ch, bool* success, int* rc, ArgsType... args)
        : func(func), desc(desc), ch(ch), success(success), rc(rc), args(std::forward<ArgsType>(args)...) {}
};

template <typename F, typename... Args>
int bdev_call(F func, struct spdk_bdev_desc* desc, struct spdk_io_channel* ch, Args... args) {

    auto msg_fn = [](void* msg_ctx) {                                                       // spdk_msg_fn
        auto cb_fn = [](struct spdk_bdev_io *bdev_io, bool success, void *cb_arg) {         // spdk_bdev_io_completion_cb
            spdk_bdev_free_io(bdev_io);
            auto ctx = reinterpret_cast<MsgCtx<F, Args...>*>(cb_arg);
            *ctx->success = success;
            LOG_DEBUG("bdev_io_completion_cb: before resume");
            ctx->awaiter.resume();
            LOG_DEBUG("bdev_io_completion_cb: after resume");
        };

        auto ctx = reinterpret_cast<MsgCtx<F, Args...>*>(msg_ctx);
        int rc = tuple_assistance::apply([&](Args... args){
            return ctx->func(ctx->desc, ctx->ch, args..., cb_fn, msg_ctx);
        }, ctx->args);

        *ctx->rc = rc;
        if (rc != 0) {
            ctx->awaiter.resume();
        }
    };

    bool success = false;
    int rc = 0;
    MsgCtx<F, Args...> ctx(func, desc, ch, &success, &rc, args...);
    spdk_thread_send_msg(g_app_thread, msg_fn, &ctx);
    ctx.awaiter.suspend();
    return rc;
}

    
}   // namespace spdk
}   // namespace photon