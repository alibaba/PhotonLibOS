#pragma once

#include <spdk/nvme.h>
#include <spdk/env.h>

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
#include <photon/common/iovector.h>

namespace photon {
namespace spdk {

int nvme_env_init();

void nvme_env_fini();

struct spdk_nvme_ctrlr* nvme_probe_attach(const char* trid_str);

int nvme_detach(spdk_nvme_ctrlr* ctrlr);

struct spdk_nvme_ns* nvme_get_namespace(struct spdk_nvme_ctrlr* ctrlr, uint32_t nsid);

struct spdk_nvme_qpair* nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr* ctrlr, struct spdk_nvme_io_qpair_opts* opts, size_t opts_size);

int nvme_ctrlr_free_io_qpair(struct spdk_nvme_ctrlr* ctrlr, struct spdk_nvme_qpair* qpair);

int nvme_ns_cmd_write(struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, void* buffer, uint64_t lba, uint32_t lba_count, uint32_t io_flags);

int nvme_ns_cmd_read(struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, void* buffer, uint64_t lba, uint32_t lba_count, uint32_t io_flags);

int nvme_ns_cmd_writev(struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count, uint32_t io_flags);

int nvme_ns_cmd_readv(struct spdk_nvme_ns* ns, struct spdk_nvme_qpair* qpair, struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count, uint32_t io_flags);

}   // namespace spdk
}   // namespace photon