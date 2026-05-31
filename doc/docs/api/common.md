---
sidebar_position: 1
toc_max_heading_level: 4
---

# Common Utilities

The `common/` module provides foundational utilities used throughout PhotonLibOS: logging, string handling, containers, callable abstractions, I/O vectors, async helpers, checksums, and more.

### Namespace

`photon::`

### Headers

All headers are under `<photon/common/...>`.

## Logging

`<photon/common/alog.h>` — type-safe, compile-time formatted logging system.

### Macros

- Level macros: `LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR`, `LOG_FATAL`
- Rate-limiting variants: `LOG_EVERY_T`, `LOG_EVERY_N`, `LOG_FIRST_N`, `LOG_FIRST_N_EVERY_T`
- Error-return helpers: `LOG_ERROR_RETURN(errno, retval, ...)`, `LOG_ERRNO_RETURN(errno, retval, ...)`
- Value introspection: `VALUE(x)` prints `x=<value>`, useful for debugging opaque values

### Format strings

alog uses backtick-based format strings processed at compile time, not printf-style runtime parsing.

- Backtick (`` ` ``) is the variable placeholder. Formats are defined by wrappers around the variables, not by the format string itself.
- Trailing backticks are unnecessary — extra arguments beyond the number of backticks are output automatically.

```cpp
LOG_WARN("count ` total ", DEC(count).comma(true), DEC(total).comma(true));
LOG_ERROR("failed, ", VALUE(st));   // no trailing backtick needed
```

### Formatters

| Wrapper | Output |
|---------|--------|
| `HEX(x)` | Hexadecimal |
| `DEC(x)` | Decimal, with optional `.comma(true)` |
| `OCT(x)` | Octal |
| `BIN(x)` | Binary |
| `FP(x)` | Floating point |

### Backends

`ILogOutput` is the abstract interface. Built-in backends write to file, stderr, stdout, or an async queue. Set via `set_log_output()`.

## String Utilities

### estring

`<photon/common/estring.h>` — `estring` extends `std::string`, `estring_view` extends `std::string_view`.

Adds: trim, case-insensitive compare, numeric conversion, lazy `split()`, and `appends()` for efficient concatenation.

```cpp
std::string key = estring().appends(host, ":", port);

estring().appends(
    path,
    estring::make_conditional_cat_list(has_query, "?", query));
```

`estring::appends` avoids creating intermediate `std::string` temporaries and accepts mixed types (string_view, integers, etc.) without manual conversion.

### Compile-time strings

`<photon/common/conststr.h>` — compile-time string manipulation: `TString`, `TStrArray`, `DEFINE_ENUM_STR`.

### String builder

`<photon/common/strbuilder.h>` — stack-allocated string builder `strBuilder<N>`.

### String-keyed containers

`<photon/common/string-keyed.h>` — maps and sets keyed by string that avoid constructing a temporary `std::string` on lookup: `unordered_map_string_key`, `map_string_key`, `unordered_map_string_kv`.

## Containers and Data Structures

### Intrusive list

`<photon/common/intrusive_list.h>` — intrusive doubly-linked list with `round_robin_next()` and predicate-based splitting. No per-node heap allocation.

### Ring buffer

`<photon/common/ring.h>`:
- `RingQueue<T>` — SPSC blocking queue
- `RingBuffer` — char-specialized ring with `readv`/`writev`

### Lock-free queues

`<photon/common/lockfree_queue.h>` — lock-free MPMC, batch MPMC, and SPSC queues. `RingChannel<QueueType>` adds photon-aware blocking with a configurable yield-then-semaphore strategy. Turn-based slot marking avoids ABA problems.

### Consistent hash

`<photon/common/consistent-hash-map.h>` — vector-based consistent hash ring.

### Sorted span

`<photon/common/ordered_span.h>` — sorted span with binary search insert/lookup.

### TTL containers

`<photon/common/expirecontainer.h>` — keyed containers with auto-expiration driven by a photon timer. `ObjectCache` provides ref-counted `Borrow` RAII access to cached objects.

### Identity pool

`<photon/common/identity-pool.h>` — object pool with constructor/destructor callbacks and autoscaling. Used as the backing store for `ThreadPool`.

## Callable Abstractions

### Delegate

`<photon/common/callback.h>` — `Delegate<R, Ts...>` is a zero-overhead callable wrapper for free functions, member functions, and lambdas. It has no heap allocation and no type erasure.

- `Callback<Ts...>` is an alias for `Delegate<int, Ts...>`.
- `Closure<ARGS...>` is a self-deleting callback.
- `TempDelegate` binds to a temporary lambda.

Member functions are dispatched with vtable-aware handling on x86_64 and aarch64.

### Delegates facade

`<photon/common/delegates.h>` — `Delegates<Convs...>` is a type-safe delegate facade with `DEFINE_DELEGATE_FUNCTION` macros for compile-time method dispatch.

### PMF helper

`<photon/common/PMF.h>` — `get_member_function_address()` extracts a raw function pointer from a member function pointer, handling the vtable layout on x86_64 and aarch64.

## I/O and Streams

### IStream

`<photon/common/stream.h>` — abstract `read`/`readv`/`write`/`writev` interface. Includes `ReadAll` RAII helper and function pointer introspection (`_and_read`, `_and_write`) to detect whether the underlying implementation actually performs I/O.

### IMessage

`<photon/common/message.h>` — datagram send/recv over iovecs.

### iovector

`<photon/common/iovector.h>` — `iovector_view` (non-owning) and `iovector` (owning). Rich iovec array manipulation: extract, slice, pipe, internal allocator. Used throughout the RPC and HTTP layers for zero-copy I/O.

### IOAlloc

`<photon/common/io-alloc.h>` — callback-based buffer allocator.

### IMessageChannel

`<photon/common/message-channel.h>` — message passing with `OUT_OF_ORDER`, `PIPELINING`, `ROUND_TRIP` flags.

## Core Utilities

### DEFER and scope helpers

`<photon/common/utility.h>`:
- `DEFER` — scope-exit cleanup
- `NewObj<T>` — two-phase construction pattern
- `likely` / `unlikely` — branch hints
- alignment and saturating arithmetic helpers
- `WITH` / `WITH_Release` — scoped resource blocks

### Timeout

`<photon/common/timeout.h>` — `Timeout` is an expiration timer based on `photon::now` (microsecond-precision global timestamp). Query via `expired()`, `timeout_us()`, `timeout_ms()`, `timeout_s()`.

### retval

`<photon/common/retval.h>` — `retval<T>` wraps a return value and captures the current `errno`.

### Range lock

`<photon/common/range-lock.h>` — `RangeLock` provides byte-range locking with conflict detection. `ScopedRangeLock` is the RAII guard.

### Throttle

`<photon/common/throttle.h>` — token bucket rate limiter with priority levels.

### SingleFlight

`<photon/common/singleflight.h>` — request coalescing to prevent cache stampedes.

### RCUPtr

`<photon/common/rcuptr.h>` — `RCUPtr<T>` is a userspace QSBR RCU-protected pointer.

### Generator

`<photon/common/generator.h>` — `Generator<Derived, ValueType>` is a CRTP-based lazy iterator generator.

### EventLoop

`<photon/common/event-loop.h>` — generic event loop with `Wait4Events` / `OnEvents` callbacks.

### Perf counters

`<photon/common/perf_counter.h>` — `REGISTER_PERF` / `REPORT_PERF` macros, with `TOTAL`, `ACCUMULATE`, `AVERAGE` types.

### Hash combiner

`<photon/common/hash_combine.h>` — Boost-style variadic hash combiner.

### UUID

`<photon/common/uuid.h>` and `uuid4.h` — UUID v4 generation, parsing, string conversion.

## Checksums

All checksums live under `<photon/common/checksum/...>`.

| Header | Description |
|--------|-------------|
| `crc32c.h` | CRC32C with hardware-acceleration auto-detection (SSE4.2 / ARM CRC). Supports series, combine, trim. |
| `crc64ecma.h` | CRC64-ECMA with hardware/software auto-selection. |
| `xxhash.h` | xxHash 32-bit (software). |
| `digest.h` | OpenSSL EVP-based digest: `sha1`, `sha256`, `sha512`, `md5`, `HMAC_*`. |

## Design Decisions

- **Compile-time format strings.** alog processes backtick-based format strings at compile time rather than parsing them at runtime like printf.
- **Delegate over `std::function`.** `Delegate` has zero overhead (no heap allocation, no type erasure), supports member functions with vtable-aware dispatch, and explicitly forbids binding to a temporary lambda to reduce lifetime risk.
- **Intrusive containers.** Intrusive lists and lock-free queues avoid per-node heap allocation and are used pervasively in the scheduler, reset handles, and object pools.
- **Lock-free queues.** Turn-based slot marking avoids ABA problems. `RingChannel` adds photon-aware blocking with a yield-then-semaphore strategy.
