#ifndef SPDK_BDEV_PHOTON_H
#define SPDK_BDEV_PHOTON_H

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/env.h"

int bdev_photon_create(struct spdk_bdev **bdev, uint64_t num_blocks);

void bdev_photon_delete(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BDEV_PHOTON_H */