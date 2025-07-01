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

using qpm = ObjectCache<struct spdk_nvme_ctrlr*, struct nvme_qpair*>;
static qpm* get_qpair_manager() {
    LOG_DEBUG("into get_qpair_manager");
    thread_local static std::unique_ptr<qpm> qpair_manager;
    auto destroy_qpair_manager = []{
        LOG_DEBUG("into destroy_qpair_manager");
        if (qpair_manager != nullptr) {
            LOG_DEBUG("destroy");
            qpair_manager.reset();
        }
        LOG_DEBUG("out destroy_qpair_manager");
    };
    if (qpair_manager == nullptr) {
        LOG_DEBUG("create and add destroy hook");
        qpair_manager = std::make_unique<qpm>(3000000);
        photon::fini_hook(destroy_qpair_manager);
    }
    LOG_DEBUG("out get_qpair_manager");
    return qpair_manager.get();
}


struct CBContextBase{
    int rc = 0;
    Awaiter<PhotonContext> awaiter;
    static void cb_fn(void *cb_ctx, const struct spdk_nvme_cpl *cpl);

    // for vector io
    struct iovec *iov;  // origin iov
    int iovcnt;         // origin iovcnt
    IOVector iovec;
    static void reset_sgl_fn(void *cb_ctx, uint32_t offset);
    static int next_sge_fn(void *cb_ctx, void **address, uint32_t *length);
};
void CBContextBase::cb_fn(void *cb_ctx, const struct spdk_nvme_cpl *cpl) {
    auto ctx = static_cast<CBContextBase*>(cb_ctx);
    if (spdk_nvme_cpl_is_error(cpl)) {
        LOG_ERROR("error: `", spdk_nvme_cpl_get_status_string(&cpl->status));
        ctx->rc = -1;
    }
    ctx->awaiter.resume();
}

void CBContextBase::reset_sgl_fn(void *cb_ctx, uint32_t offset) {
    LOG_DEBUG("get into reset_sgl_fn", VALUE(offset));
    auto ctx = static_cast<CBContextBase*>(cb_ctx);
    ctx->iovec.clear();
    for (int i=0; i<ctx->iovcnt; i++) ctx->iovec.push_back(ctx->iov[i]);
    ctx->iovec.extract_front(offset);
}

int CBContextBase::next_sge_fn(void *cb_ctx, void **address, uint32_t *length) {
    auto ctx = static_cast<CBContextBase*>(cb_ctx);
    if (ctx->iovec.sum() > 0) {
        *address = ctx->iovec.front().iov_base;
        *length = ctx->iovec.front().iov_len;
        LOG_DEBUG("get into next_sge_fn", VALUE(*length));
        ctx->iovec.pop_front();
        return 0;
    }
    else {
        LOG_ERROR_RETURN(0, -1, "ctx->iov_view is empty");
    }
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
    return ctrlr;
}

int nvme_detach(struct spdk_nvme_ctrlr* ctrlr) {
    LOG_DEBUG("get into nvme_detach");
    spdk_nvme_detach(ctrlr);
    LOG_DEBUG("get out nvme_detach");
    return 0;
}

struct spdk_nvme_ns* nvme_get_namespace(struct spdk_nvme_ctrlr* ctrlr, uint32_t nsid) {
    return spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
}

struct spdk_nvme_qpair* nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr* ctrlr, struct spdk_nvme_io_qpair_opts* opts, size_t opts_size) {
    LOG_DEBUG("before acquired qpair");
    auto m_qpair = get_qpair_manager()->acquire(ctrlr, [&]() -> struct nvme_qpair* {
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
    get_qpair_manager()->release(ctrlr, true);
    LOG_DEBUG("after release qpair");
    return 0;
}

typedef int (*spdk_nvme_ns_cmd_rw) (struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *payload,
			                    uint64_t lba, uint32_t lba_count,
                                spdk_nvme_cmd_cb cb_fn, void *cb_arg,
                                uint32_t io_flags);

typedef int (*spdk_nvme_ns_cmd_rwv) (struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
                                uint64_t lba, uint32_t lba_count,
                                spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
                                spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
                                spdk_nvme_req_next_sge_cb next_sge_fn);


static int io_helper(struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, void* buffer, uint64_t lba, uint32_t lba_count, uint32_t io_flags, spdk_nvme_ns_cmd_rw rw_fn) {
    CBContextBase ctx;
    int rc = rw_fn(ns, qpair, buffer, lba, lba_count, CBContextBase::cb_fn, &ctx, io_flags);
    if (rc != 0) return rc;
    ctx.awaiter.suspend();
    return ctx.rc;
}

int nvme_ns_cmd_write(struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, void* buffer, uint64_t lba, uint32_t lba_count, uint32_t io_flags) {
    return io_helper(ns, qpair, buffer, lba, lba_count, io_flags, &spdk_nvme_ns_cmd_write);
}

int nvme_ns_cmd_read(struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, void* buffer, uint64_t lba, uint32_t lba_count, uint32_t io_flags) {
    return io_helper(ns, qpair, buffer, lba, lba_count, io_flags, &spdk_nvme_ns_cmd_read);
}

static int vec_io_helper(struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count, uint32_t io_flags, spdk_nvme_ns_cmd_rwv rwv_fn) {
    CBContextBase ctx;
    ctx.iov = iov;
    ctx.iovcnt = iovcnt;
    int rc = rwv_fn(ns, qpair, lba, lba_count, CBContextBase::cb_fn, &ctx, io_flags, CBContextBase::reset_sgl_fn, CBContextBase::next_sge_fn);
    if (rc != 0) return rc;
    ctx.awaiter.suspend();
    return ctx.rc;
}

int nvme_ns_cmd_writev(struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count, uint32_t io_flags) {
    return vec_io_helper(ns, qpair, iov, iovcnt, lba, lba_count, io_flags, &spdk_nvme_ns_cmd_writev);
}

int nvme_ns_cmd_readv(struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count, uint32_t io_flags) {
    return vec_io_helper(ns, qpair, iov, iovcnt, lba, lba_count, io_flags, &spdk_nvme_ns_cmd_readv);
}

}   // namespace spdk
}   // namespace photon