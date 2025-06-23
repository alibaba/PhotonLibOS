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


extern spdk_thread* g_app_thread;

template <typename... Args>
struct MsgCtx {
    struct spdk_bdev_desc* desc;
    struct spdk_io_channel* ch;
    bool* success;
    int* rc;
    Awaiter<PhotonContext>* awaiter;
    std::tuple<Args...> args;

    template <typename... Ts>
    MsgCtx(struct spdk_bdev_desc* desc, struct spdk_io_channel* ch, bool* success, int* rc, Awaiter<PhotonContext>* awaiter, Ts... args)
        : desc(desc), ch(ch), success(success), rc(rc), awaiter(awaiter), args(std::forward<Ts>(args)...) {}
};

// spdk_bdev_io_completion_cb
template <typename... Args>
void cb_fn(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg) {
    // std::cout << "get into cb_fn" << std::endl;
    spdk_bdev_free_io(bdev_io);
    // std::cout << "after spdk_bdev_free_io" << std::endl;
    auto ctx = reinterpret_cast<MsgCtx<Args...>*>(cb_arg);
    *ctx->success = success;
    // std::cout << "before resume" << std::endl;
    ctx->awaiter->resume();
    // std::cout << "after resume" << std::endl;
}


template <typename F, typename Tup, size_t... I>
int call(F f, struct spdk_bdev_desc* desc, struct spdk_io_channel* ch, void* cb_arg, Tup tup, std::index_sequence<I...>) {
    return f(desc, ch, cb_arg, std::get<I>(std::forward<Tup>(tup))...);
}

#define DEFINE_BDEV_IO_FUNC(name) \
/* spdk_msg_fn */  \
template <typename... Args> \
void msg_##name(void* arg) { \
    auto ctx = reinterpret_cast<MsgCtx<Args...>*>(arg); \
    int rc = call([](struct spdk_bdev_desc* desc, struct spdk_io_channel* ch, void* cb_arg, Args... args) {  \
        return spdk_bdev_##name(desc, ch, args..., cb_fn, cb_arg); \
    }, ctx->desc, ctx->ch, arg, ctx->args, std::make_index_sequence<sizeof...(Args)>{}); \
    *ctx->rc = rc; \
    if (rc != 0) { \
        ctx->awaiter->resume(); \
    } \
}\
template <typename... Args> \
int bdev_##name(struct spdk_bdev_desc* desc, struct spdk_io_channel* ch, Args... args) { \
    bool success = false; \
    int rc = 0; \
    Awaiter<PhotonContext> awaiter; \
    MsgCtx<Args...> ctx(desc, ch, &success, &rc, &awaiter, args...); \
    spdk_thread_send_msg(g_app_thread, msg_##name<Args...>, &ctx); \
    awaiter.suspend(); \
    return rc; \
}


DEFINE_BDEV_IO_FUNC(read);
DEFINE_BDEV_IO_FUNC(read_blocks);
DEFINE_BDEV_IO_FUNC(read_blocks_with_md);
DEFINE_BDEV_IO_FUNC(readv);
DEFINE_BDEV_IO_FUNC(readv_blocks);
DEFINE_BDEV_IO_FUNC(readv_blocks_with_md);

DEFINE_BDEV_IO_FUNC(write);
DEFINE_BDEV_IO_FUNC(write_blocks);
DEFINE_BDEV_IO_FUNC(write_blocks_with_md);
DEFINE_BDEV_IO_FUNC(writev);
DEFINE_BDEV_IO_FUNC(writev_blocks);
DEFINE_BDEV_IO_FUNC(writev_blocks_with_md);

DEFINE_BDEV_IO_FUNC(compare_blocks);
DEFINE_BDEV_IO_FUNC(compare_blocks_with_md);
DEFINE_BDEV_IO_FUNC(comparev_blocks);
DEFINE_BDEV_IO_FUNC(comparev_blocks_with_md);
DEFINE_BDEV_IO_FUNC(comparev_and_writev_blocks);

DEFINE_BDEV_IO_FUNC(zcopy_start);
DEFINE_BDEV_IO_FUNC(zcopy_end);

DEFINE_BDEV_IO_FUNC(write_zeroes);
DEFINE_BDEV_IO_FUNC(write_zeroes_blocks);

DEFINE_BDEV_IO_FUNC(unmap);
DEFINE_BDEV_IO_FUNC(unmap_blocks);

DEFINE_BDEV_IO_FUNC(flush);
DEFINE_BDEV_IO_FUNC(flush_blocks);

DEFINE_BDEV_IO_FUNC(reset);
DEFINE_BDEV_IO_FUNC(abort);

DEFINE_BDEV_IO_FUNC(nvme_admin_passthru);
DEFINE_BDEV_IO_FUNC(nvme_io_passthru);
DEFINE_BDEV_IO_FUNC(nvme_io_passthru_md);

    
}   // namespace spdk
}   // namespace photon