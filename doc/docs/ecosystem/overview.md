---
sidebar_position: 1
toc_max_heading_level: 4
---

# Ecosystem

The `ecosystem/` directory contains higher-level clients and parsers built on top of Photon's core modules. These are optional; enable with the `PHOTON_ENABLE_ECOSYSTEM` CMake option.

### Namespace

`photon::`

### Headers

`<photon/ecosystem/...>`

## Redis Client

`<photon/ecosystem/redis.h>` — coroutine-aware Redis client.

- Speaks RESP (Redis Serialization Protocol) directly over `ISocketStream`.
- Supports strings, hashes, lists, sets, sorted sets, key operations, scripting, pub/sub, and transactions.
- Uses `Delegate` callbacks for response parsing, avoiding intermediate buffer copies.
- Connection pooling is provided through the standard socket pool primitives in `net/`.

Source: `ecosystem/redis.h`, `ecosystem/redis.cpp`.

## OSS Client

`<photon/ecosystem/oss.h>` — Alibaba Cloud Object Storage Service client.

- Supports bucket and object operations: put, get, delete, list, multipart upload, copy, metadata.
- Signs requests with OSS V1/V4 signatures using the `common/checksum` and `common/digest` helpers.
- Streaming uploads and downloads use `iovector` for zero-copy I/O.
- XML responses are parsed with `simple_dom`.

Source: `ecosystem/oss.h`, `ecosystem/oss.cpp`, `ecosystem/oss_constants.h`.

## simple_dom

`<photon/ecosystem/simple_dom.h>` — lightweight, allocation-free DOM parser for JSON, XML, and YAML.

- Single tree representation shared across all three formats.
- `SimpleDOM::Document` parses from a buffer; nodes are accessed via path-like queries.
- Designed for configuration parsing and small response bodies, not for large streaming documents.
- Patches for `rapidjson` and `rapidxml` (used as backends) are kept in `ecosystem/patches/`.

Source: `ecosystem/simple_dom.h`, `ecosystem/simple_dom.cpp`, `ecosystem/simple_dom_impl.h`.

## Build

The ecosystem is built only when `PHOTON_ENABLE_ECOSYSTEM=ON`:

```bash
cmake -B build -D PHOTON_BUILD_TESTING=ON -D PHOTON_ENABLE_ECOSYSTEM=ON
cmake --build build -j 8
```

## Related Modules

- [Network](../api/network.md) — `ISocketStream`, HTTP client, and TLS streams used as transport.
- [Common Utilities](../api/common.md) — `iovector`, `Delegate`, checksums, and `estring` used throughout.
- [Filesystem and IO](../api/filesystem-and-io.md) — some ecosystem code paths treat remote storage as a filesystem via `fs/httpfs`.
