#include "spdknvme-wrapper.h"
#include <photon/common/alog-stdstring.h>
#include <photon/common/expirecontainer.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>

#include <unordered_map>


namespace photon {
namespace spdk {

struct nvme_qpair {
    bool is_complete = false;
    struct spdk_nvme_qpair* qpair = nullptr;
    photon::join_handle* jh = nullptr;

    nvme_qpair(struct spdk_nvme_ctrlr* ctrlr, struct spdk_nvme_io_qpair_opts* opts, size_t opts_size) {
        assert(ctrlr != nullptr);
        LOG_DEBUG("nvme qpair get into constructor");
        qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, opts, opts_size);
        LOG_DEBUG("after spdk alloc io qpair");
        jh = thread_enable_join(thread_create11([this]{
            while (!is_complete) {
                spdk_nvme_qpair_process_completions(qpair, 0);
                thread_yield();
            }
        }));
        LOG_DEBUG("nvme qpair get out constructor");
    }

    ~nvme_qpair() {
        LOG_DEBUG("nvme qpair get into destructor");
        is_complete = true;
        LOG_DEBUG("before join");
        thread_join(jh);
        LOG_DEBUG("after join");
        spdk_nvme_ctrlr_free_io_qpair(qpair);
        LOG_DEBUG("nvme qpair get out destructor");
    }
};

static std::unordered_map<struct spdk_nvme_ctrlr*, std::shared_ptr<ObjectCache<vcpu_base*, nvme_qpair*>>> g_qpair_manager;

void CBContextBase::cb_fn(void *cb_ctx, const struct spdk_nvme_cpl *cpl) {
    auto ctx = static_cast<CBContextBase*>(cb_ctx);
    if (spdk_nvme_cpl_is_error(cpl)) {
        LOG_ERROR("error: `", spdk_nvme_cpl_get_status_string(&cpl->status));
        ctx->rc = -1;
    }
    ctx->awaiter.resume();
}

void CBContextBase::reset_sgl_fn(void *cb_ctx, uint32_t offset) {
    auto ctx = static_cast<CBContextBase*>(cb_ctx);
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
}

int CBContextBase::next_sge_fn(void *cb_ctx, void **address, uint32_t *length) {
    auto ctx = static_cast<CBContextBase*>(cb_ctx);
    *address = (char*)(ctx->iov_view[ctx->idx].iov_base) + ctx->off;
    *length = ctx->iov_view[ctx->idx].iov_len - ctx->off;
    ctx->off = 0;
    ctx->idx++;
    return 0;
}

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
    LOG_DEBUG("get into nvme_env_fini");
    spdk_env_fini();
    LOG_DEBUG("get out nvme_env_fini");
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

    if (ctrlr != nullptr) {
        g_qpair_manager.emplace(ctrlr, new ObjectCache<vcpu_base*, nvme_qpair*>(3000000));
        return ctrlr;
    }
    else {
        LOG_ERROR_RETURN(0, nullptr, "nvme_probe_attach failed");
    }
}

int nvme_detach(struct spdk_nvme_ctrlr* ctrlr) {
    LOG_DEBUG("get into nvme_detach");
    g_qpair_manager.erase(ctrlr);
    spdk_nvme_detach(ctrlr);
    LOG_DEBUG("get out nvme_detach");
    return 0;
}

struct spdk_nvme_ns* nvme_get_namespace(struct spdk_nvme_ctrlr* ctrlr, uint32_t nsid) {
    return spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
}

struct spdk_nvme_qpair* nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr* ctrlr, struct spdk_nvme_io_qpair_opts* opts, size_t opts_size) {
    LOG_DEBUG("before acquired qpair");
    auto m_qpair = g_qpair_manager[ctrlr]->acquire(get_vcpu(), [&]() -> struct nvme_qpair* {
        LOG_DEBUG("before new struct nvme_qpair");
        return new nvme_qpair(ctrlr, opts, opts_size);
    });
    LOG_DEBUG("after acquired qpair");
    if (m_qpair != nullptr) {
        return m_qpair->qpair;
    }
    else {
        LOG_ERROR_RETURN(0, nullptr, "failed to allocate qpair");
    }
}

int nvme_ctrlr_free_io_qpair(struct spdk_nvme_ctrlr* ctrlr, struct spdk_nvme_qpair* qpair) {
    LOG_DEBUG("before release qpair");
    g_qpair_manager[ctrlr]->release(get_vcpu(), true);
    LOG_DEBUG("after release qpair");
    return 0;
}


static int io_helper(bool is_write, struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, void* buffer, uint64_t lba, uint32_t lba_count, uint32_t io_flags, void* ctx) {
    if (is_write) {
        return spdk_nvme_ns_cmd_write(ns, qpair, buffer, lba, lba_count, CBContextBase::cb_fn, ctx, io_flags);
    } else {
        return spdk_nvme_ns_cmd_read(ns, qpair, buffer, lba, lba_count, CBContextBase::cb_fn, ctx, io_flags);
    }
}

int nvme_ns_cmd_write(struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, void* buffer, uint64_t lba, uint32_t lba_count, uint32_t io_flags) {
    return nvme_call(&io_helper, true, ns, qpair, buffer, lba, lba_count, io_flags);
}

int nvme_ns_cmd_read(struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, void* buffer, uint64_t lba, uint32_t lba_count, uint32_t io_flags) {
    return nvme_call(&io_helper, false, ns, qpair, buffer, lba, lba_count, io_flags);
}

static int vec_io_helper(bool is_write, struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count, uint32_t io_flags, void* ctx) {
    auto ctx_rwv = static_cast<CBContextBase*>(ctx);
    ctx_rwv->iov_view.assign(iov, iovcnt);
    if (is_write) {
        return spdk_nvme_ns_cmd_writev(ns, qpair, lba, lba_count, CBContextBase::cb_fn, ctx_rwv, io_flags, CBContextBase::reset_sgl_fn, CBContextBase::next_sge_fn);
    }
    else {
        return spdk_nvme_ns_cmd_readv(ns, qpair, lba, lba_count, CBContextBase::cb_fn, ctx_rwv, io_flags, CBContextBase::reset_sgl_fn, CBContextBase::next_sge_fn);
    }
}

int nvme_ns_cmd_writev(struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count, uint32_t io_flags) {
    return nvme_call(&vec_io_helper, true, ns, qpair, iov, iovcnt, lba, lba_count, io_flags);
}

int nvme_ns_cmd_readv(struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count, uint32_t io_flags) {
    return nvme_call(&vec_io_helper, false, ns, qpair, iov, iovcnt, lba, lba_count, io_flags);
}

}   // namespace spdk
}   // namespace photon