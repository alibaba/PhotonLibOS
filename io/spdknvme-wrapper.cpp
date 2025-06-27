#include "spdknvme-wrapper.h"
#include <photon/common/alog-stdstring.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#include <photon/common/iovector.h>

namespace photon {
namespace spdk {

struct nvme_qpair {
    struct spdk_nvme_qpair* qpair = nullptr;
    int is_complete = 0;
    photon::join_handle* jh = nullptr;
};
thread_local struct nvme_qpair* local_nvme_qpair = nullptr;

int nvme_env_init() {
    struct spdk_env_opts opts;
    spdk_env_opts_init(&opts);
    opts.name = "photon_spdk_nvme_pcie";

    int rc = 0;
    if ((rc = spdk_env_init(&opts)) < 0) {
        LOG_ERROR_RETURN(0, rc, "spdk_env_init failed");
    }

    return rc;
}

void nvme_env_fini() {
    spdk_env_fini();
}

struct spdk_nvme_ctrlr* nvme_probe_attach(const char* trid_str) {
    auto probe_cb = [](void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr_opts *opts) -> bool {
        LOG_INFO("probe_cb: do attach in default ", VALUE(trid->traddr));
        return true;
    };

    auto attach_cb = [](void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts) {
        LOG_INFO("attach_cb: attach ", VALUE(trid->traddr));
        auto ret_ctrlr = (struct spdk_nvme_ctrlr**)cb_ctx;
        *ret_ctrlr = ctrlr;
    };

    struct spdk_nvme_transport_id trid;
    spdk_nvme_transport_id_parse(&trid, trid_str);

    struct spdk_nvme_ctrlr *ctrlr = nullptr;
    spdk_nvme_probe(&trid, &ctrlr, probe_cb, attach_cb, nullptr);

    return ctrlr;
}

int nvme_detach(struct spdk_nvme_ctrlr* ctrlr) {
    return spdk_nvme_detach(ctrlr);
}

struct spdk_nvme_ns* nvme_get_namespace(struct spdk_nvme_ctrlr* ctrlr, uint32_t nsid) {
    return spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
}

struct spdk_nvme_qpair* nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr* ctrlr, struct spdk_nvme_io_qpair_opts* opts, size_t opts_size) {
    if (!local_nvme_qpair) {
        struct spdk_nvme_qpair* qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, opts, opts_size);
        local_nvme_qpair = new struct nvme_qpair;
        local_nvme_qpair->qpair = qpair;
        local_nvme_qpair->is_complete = 0;

        local_nvme_qpair->jh = photon::thread_enable_join(photon::thread_create11([]{
            while (!local_nvme_qpair->is_complete) {
                spdk_nvme_qpair_process_completions(local_nvme_qpair->qpair, 0);
                thread_yield();
            }
        }));
    }
    return local_nvme_qpair->qpair;
}

int nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair* qpair) {
    if (local_nvme_qpair) {
        local_nvme_qpair->is_complete = 1;
        thread_join(local_nvme_qpair->jh);
        spdk_nvme_ctrlr_free_io_qpair(local_nvme_qpair->qpair);
        delete local_nvme_qpair;
        local_nvme_qpair = nullptr;
        return 0;
    }
    LOG_ERROR_RETURN(0, -1, "local_nvme_qpair is nullptr");
}

int nvme_ns_cmd_read(struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, void* buffer, uint64_t lba, uint32_t lba_count, uint32_t io_flags) {
    struct MyCtx {
        int rc = 0;
        Awaiter<PhotonContext> awaiter;
    };

    auto cb_fn = [](void *cb_ctx, const struct spdk_nvme_cpl *cpl) {   // spdk_nvme_cmd_cb
        auto ctx = static_cast<MyCtx*>(cb_ctx);
        if (spdk_nvme_cpl_is_error(cpl)) {
            LOG_ERROR("error: `", spdk_nvme_cpl_get_status_string(&cpl->status));
            ctx->rc = -1;
        }
        ctx->awaiter.resume();
    };

    MyCtx ctx;
    spdk_nvme_ns_cmd_read(ns, qpair, buffer, lba, lba_count, cb_fn, &ctx, io_flags);
    ctx.awaiter.suspend();

    return ctx.rc;
}

int nvme_ns_cmd_write(struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, void* buffer, uint64_t lba, uint32_t lba_count, uint32_t io_flags) {
    struct MyCtx {
        int rc = 0;
        Awaiter<PhotonContext> awaiter;
    };

    auto cb_fn = [](void *cb_ctx, const struct spdk_nvme_cpl *cpl) {   // spdk_nvme_cmd_cb
        auto ctx = static_cast<MyCtx*>(cb_ctx);
        if (spdk_nvme_cpl_is_error(cpl)) {
            LOG_ERROR("error: `", spdk_nvme_cpl_get_status_string(&cpl->status));
            ctx->rc = -1;
        }
        ctx->awaiter.resume();
    };

    MyCtx ctx;
    spdk_nvme_ns_cmd_write(ns, qpair, buffer, lba, lba_count, cb_fn, &ctx, io_flags);
    ctx.awaiter.suspend();

    return ctx.rc;
}

int nvme_ns_cmd_writev(struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count, uint32_t io_flags) {
    struct MyCtx {
        int rc = 0;
        Awaiter<PhotonContext> awaiter;
        iovector_view iov_view;
        size_t idx = 0;
        size_t off = 0;
    };

    auto reset_sgl_fn = [](void *cb_ctx, uint32_t offset) {                     // spdk_nvme_req_reset_sgl_cb
        auto ctx = static_cast<MyCtx*>(cb_ctx);
        assert(offset < ctx->iov_view.sum());
        ctx->idx = 0;
        size_t remain = offset;
        while (remain) {
            size_t provide = ctx->iov_view[ctx->idx].iov_len;
            if (provide <= remain) {
                ctx->idx++;
                remain -= provide;
            }
            else {
                break;
            }
        }
        ctx->off = remain;
    };

    auto next_sge_fn = [](void *cb_ctx, void **address, uint32_t *length) -> int {     // spdk_nvme_req_next_sge_cb
        auto ctx = static_cast<MyCtx*>(cb_ctx);
        *address = (char*)(ctx->iov_view[ctx->idx].iov_base) + ctx->off;
        *length = ctx->iov_view[ctx->idx].iov_len - ctx->off;
        ctx->off = 0;
        ctx->idx++;
        if (ctx->idx < ctx->iov_view.elements_count()) return 0;
        else return -1;
    };

    auto cb_fn = [](void *cb_ctx, const struct spdk_nvme_cpl *cpl) {   // spdk_nvme_cmd_cb
        auto ctx = static_cast<MyCtx*>(cb_ctx);
        if (spdk_nvme_cpl_is_error(cpl)) {
            LOG_ERROR("error: `", spdk_nvme_cpl_get_status_string(&cpl->status));
            ctx->rc = -1;
        }
        ctx->awaiter.resume();
    };

    MyCtx ctx;
    ctx.iov_view.assign(iov, iovcnt);
    spdk_nvme_ns_cmd_writev(ns, qpair, lba, lba_count, cb_fn, &ctx, io_flags, reset_sgl_fn, next_sge_fn);
    ctx.awaiter.suspend();

    return ctx.rc;
}

int nvme_ns_cmd_readv(struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count, uint32_t io_flags) {
    struct MyCtx {
        int rc = 0;
        Awaiter<PhotonContext> awaiter;
        iovector_view iov_view;
        size_t idx = 0;
        size_t off = 0;
    };

    auto reset_sgl_fn = [](void *cb_ctx, uint32_t offset) {                     // spdk_nvme_req_reset_sgl_cb
       auto ctx = static_cast<MyCtx*>(cb_ctx);
        assert(offset < ctx->iov_view.sum());
        ctx->idx = 0;
        size_t remain = offset;
        while (remain) {
            size_t provide = ctx->iov_view[ctx->idx].iov_len;
            if (provide <= remain) {
                ctx->idx++;
                remain -= provide;
            }
            else {
                break;
            }
        }
        ctx->off = remain;
    };

    auto next_sge_fn = [](void *cb_ctx, void **address, uint32_t *length) -> int {     // spdk_nvme_req_next_sge_cb
        auto ctx = static_cast<MyCtx*>(cb_ctx);
        *address = (char*)(ctx->iov_view[ctx->idx].iov_base) + ctx->off;
        *length = ctx->iov_view[ctx->idx].iov_len - ctx->off;
        ctx->off = 0;
        ctx->idx++;
        if (ctx->idx < ctx->iov_view.elements_count()) return 0;
        else return -1;
    };

    auto cb_fn = [](void *cb_ctx, const struct spdk_nvme_cpl *cpl) {   // spdk_nvme_cmd_cb
        auto ctx = static_cast<MyCtx*>(cb_ctx);
        if (spdk_nvme_cpl_is_error(cpl)) {
            LOG_ERROR("error: `", spdk_nvme_cpl_get_status_string(&cpl->status));
            ctx->rc = -1;
        }
        ctx->awaiter.resume();
    };

    MyCtx ctx;
    ctx.iov_view.assign(iov, iovcnt);
    spdk_nvme_ns_cmd_readv(ns, qpair, lba, lba_count, cb_fn, &ctx, io_flags, reset_sgl_fn, next_sge_fn);
    ctx.awaiter.suspend();

    return ctx.rc;
}

}   // namespace spdk
}   // namespace photon