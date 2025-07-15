#include "bdev_photon.h"

#include "spdk/rpc.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/string.h"

struct rpc_create_bdev_photon_requset {
    uint64_t num_blocks;
};

struct rpc_delete_bdev_photon_requset {
    char *name;
};

static const struct spdk_json_object_decoder rpc_bdev_photon_create_decoders[] = {
    {"num_blocks", offsetof(struct rpc_create_bdev_photon_requset, num_blocks), spdk_json_decode_uint64},
};

static const struct spdk_json_object_decoder rpc_bdev_photon_delete_decoders[] = {
    {"name", offsetof(struct rpc_delete_bdev_photon_requset, name), spdk_json_decode_string},
};

static void rpc_bdev_photon_create(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params) {
    SPDK_NOTICELOG("rpc_bdev_photon_create\n");

    struct rpc_create_bdev_photon_requset req;
    if (spdk_json_decode_object(params, rpc_bdev_photon_create_decoders, SPDK_COUNTOF(rpc_bdev_photon_create_decoders), &req)) {
        SPDK_NOTICELOG("spdk_json_decode_object failed\n");
        spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "spdk_json_decode_object failed");
        return;
    }

    SPDK_NOTICELOG("num_blocks=%ld\n", req.num_blocks);

    struct spdk_bdev *bdev;
    int rc = bdev_photon_create(&bdev, req.num_blocks);
    if (rc) {
        spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
        return;
    }

    struct spdk_json_write_ctx *w;
    w = spdk_jsonrpc_begin_result(request);
    spdk_json_write_string(w, spdk_bdev_get_name(bdev));
    spdk_jsonrpc_end_result(request, w);

}
SPDK_RPC_REGISTER("bdev_photon_create", rpc_bdev_photon_create, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_photon_create, construct_photon_bdev)


static void rpc_bdev_photon_delete_cb(void *cb_arg, int rc) {
    struct spdk_jsonrpc_request *request = cb_arg;
    spdk_jsonrpc_send_bool_response(request, rc == 0);
}

static void rpc_bdev_photon_delete(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params) {
    SPDK_NOTICELOG("rpc_bdev_photon_delete\n");

    struct rpc_delete_bdev_photon_requset req = {NULL};
    if (spdk_json_decode_object(params, rpc_bdev_photon_delete_decoders, SPDK_COUNTOF(rpc_bdev_photon_delete_decoders), &req)) {
        SPDK_NOTICELOG("spdk_json_decode_object failed\n");
        spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "spdk_json_decode_object failed");
        free(req.name);
        return;
    }

    SPDK_NOTICELOG("name=%s\n", req.name);

    struct spdk_bdev *bdev = spdk_bdev_get_by_name(req.name);
    if (bdev == NULL) {
        SPDK_NOTICELOG("bdev '%s' does not exist\n", req.name);
        spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
        free(req.name);
        return;
    }

    bdev_photon_delete(bdev, rpc_bdev_photon_delete_cb, request);
    free(req.name);
}
SPDK_RPC_REGISTER("bdev_photon_delete", rpc_bdev_photon_delete, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_photon_delete, delete_photon_bdev)