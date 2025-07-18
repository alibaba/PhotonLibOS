#include "bdev_photon.h"

#include "spdk/rpc.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/string.h"

struct rpc_create_bdev_photon_requset {
    uint64_t num_blocks;
    uint32_t nsid;
    char* trid;

    char* ip;
    uint16_t port;
    uint64_t expiration;
    uint64_t timeout;
};

struct rpc_delete_bdev_photon_requset {
    char* name;
};

static const struct spdk_json_object_decoder rpc_bdev_photon_create_decoders[] = {
    {"num_blocks", offsetof(struct rpc_create_bdev_photon_requset, num_blocks), spdk_json_decode_uint64},
    {"nsid", offsetof(struct rpc_create_bdev_photon_requset, nsid), spdk_json_decode_uint32},
    {"trid", offsetof(struct rpc_create_bdev_photon_requset, trid), spdk_json_decode_string},
    {"ip", offsetof(struct rpc_create_bdev_photon_requset, ip), spdk_json_decode_string},
    {"port", offsetof(struct rpc_create_bdev_photon_requset, port), spdk_json_decode_uint16},
    {"expiration", offsetof(struct rpc_create_bdev_photon_requset, expiration), spdk_json_decode_uint64},
    {"timeout", offsetof(struct rpc_create_bdev_photon_requset, timeout), spdk_json_decode_uint64}
};

static const struct spdk_json_object_decoder rpc_bdev_photon_delete_decoders[] = {
    {"name", offsetof(struct rpc_delete_bdev_photon_requset, name), spdk_json_decode_string},
};

static void rpc_bdev_photon_create(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params) {
    SPDK_NOTICELOG("rpc_bdev_photon_create\n");

    struct rpc_create_bdev_photon_requset req = {};
    if (spdk_json_decode_object(params, rpc_bdev_photon_create_decoders, SPDK_COUNTOF(rpc_bdev_photon_create_decoders), &req)) {
        SPDK_NOTICELOG("spdk_json_decode_object failed\n");
        spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "spdk_json_decode_object failed");
        free(req.trid);
        free(req.ip);
        return;
    }

    SPDK_NOTICELOG("num_blocks=%lu, nsid=%u, trid=%s, ip=%s, port=%u, expiration=%lu, timeout=%lu\n", req.num_blocks, req.nsid, req.trid, req.ip, req.port, req.expiration, req.timeout);

    struct spdk_bdev *bdev;
    int rc = bdev_photon_create(&bdev, req.trid, req.nsid, req.num_blocks, req.ip, req.port, req.expiration, req.timeout);
    if (rc) {
        spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
        free(req.trid);
        free(req.ip);
        return;
    }

    // response
    struct spdk_json_write_ctx *w;
    w = spdk_jsonrpc_begin_result(request);
    spdk_json_write_string(w, spdk_bdev_get_name(bdev));
    spdk_jsonrpc_end_result(request, w);

    free(req.trid);
    free(req.ip);
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

    // response in cb
    bdev_photon_delete(bdev, rpc_bdev_photon_delete_cb, request);
    free(req.name);
}
SPDK_RPC_REGISTER("bdev_photon_delete", rpc_bdev_photon_delete, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_photon_delete, delete_photon_bdev)