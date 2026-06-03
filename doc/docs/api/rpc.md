---
sidebar_position: 9
toc_max_heading_level: 4
---

# RPC Framework

The `rpc/` module provides a high-performance RPC framework with zero-copy serialization, connection pooling, and out-of-order async execution. Published in PPoPP'26 as "zBuffer".

### Namespace

`photon::rpc::`

### Headers

`<photon/rpc/rpc.h>`, `<photon/rpc/serialize.h>`, `<photon/rpc/out-of-order-execution.h>`

## Protocol

Defined in `<photon/rpc/rpc.h>`.

### Wire header

Each RPC message begins with a 40-byte header:

| Field | Description |
|-------|-------------|
| MAGIC | Constant `0x87de5d02e6ab95c7` |
| VERSION | Protocol version |
| size | Total message size |
| FunctionID | Composite of interface + method |
| tag | Per-request correlation tag |

### Stub and Skeleton

- **`Stub`** — the RPC client entity. `call<Operation>(req, resp, timeout)` performs a request with zero-copy serialization.
- **`Skeleton`** — the RPC server entity. `add_function()` registers handlers, `register_service<Operations...>()` registers a batch, `serve(IStream*)` runs the dispatch loop.
- **`StubPool`** — connection pool with TTL-based expiration and per-call timeout. `get_stub(endpoint)`, `put_stub()`, `acquire()`.

## Serialization

Defined in `<photon/rpc/serialize.h>`. The type system is built on top of `iovector` for zero-copy.

### Field types

| Type | Description |
|------|-------------|
| `buffer` | Variable-length byte buffer |
| `fixed_buffer<T>` | Fixed-size typed buffer |
| `array<T>` | Variable-length typed array |
| `string` | Slice-based string (offset + length into iovector) |
| `slice` | Generic slice reference |
| `iovec_array` | Array of iovecs |
| `sorted_map<K,V>` | Binary-search indexed map over serialized data |

### Archives

- `SerializerIOV` — serializes into an iovector.
- `DeserializerIOV` — deserializes with iovector extraction.

### Message base

`rpc::Message` defines the `serialize_fields()` method plus `add_checksum()` / `validate_checksum()`. `CheckedMessage<Hasher>` adds automatic CRC32C integrity checking.

Field registration uses the `PROCESS_FIELDS(...)` macro together with the `ArchiveBase<Derived>` CRTP, which provides `process_field()` overloads for each supported type.

## Out-of-Order Execution

Defined in `<photon/rpc/out-of-order-execution.h>`. Enables pipelining multiple async operations while presenting a synchronous interface to the caller.

- **`OutOfOrderContext`** — per-operation context with `do_issue`, `do_completion`, `do_collect` callbacks, plus tag, phase, and timeout.
- `ooo_issue_operation()` — issue an async operation.
- `ooo_wait_completion()` — wait for completion.
- `ooo_issue_wait()` — combined issue + wait.
- `ooo_result_collected()` — signal that the result has been collected.
- `new_ooo_execution_engine()` — factory for a concurrent out-of-order engine.

## Factory Functions

| Factory | Description |
|---------|-------------|
| `new_rpc_stub()` | Create an RPC client stub |
| `new_stub_pool()` | Create a connection-pooled stub pool |
| `new_uds_stub_pool()` | Unix Domain Socket stub pool |
| `new_skeleton()` | Create an RPC server skeleton |

## Design Decisions

- **Zero-copy serialization.** `SerializerIOV` / `DeserializerIOV` work directly on iovector, with no intermediate buffer copies. `sorted_map` performs binary search over serialized data rather than deserializing into `std::map`.
- **Slice-based strings.** Serialized strings store offset + length into the iovector, not copies, avoiding allocation during deserialization.
- **CRC32C checksums.** `CheckedMessage` uses hardware-accelerated CRC32C for message integrity.
- **Out-of-order execution.** Multiple RPC calls can be pipelined on a single connection — issue multiple requests, then collect responses as they arrive.
- **StubPool.** Reuses connections with TTL-based expiration, reducing TCP handshake overhead.

## Related Modules

- [Network](./network.md) — `ISocketStream` provides the RPC transport, `TCPSocketPool` backs `StubPool`.
- [Common Utilities](./common.md) — `iovector` for zero-copy serialization, `crc32c` for checksums, `Delegate` for callbacks.
