---
sidebar_position: 3
toc_max_heading_level: 5
---

# vCPU and Multi-core

### Concept

Photon vCPU == native OS thread

Each vCPU has a scheduler, executing and switching [threads](thread).

### Enable multi-core

Currently, there are only two ways in Photon to utilize multiple cores:

### 1. Create OS thread manually, and initialize the Env

`thread_migrate` could be used to migrate thread to other vCPU.

```cpp
std::thread([]{
	photon::init();
	DEFER(photon::fini());
    
    auto th = photon::thread_create11(func);
    photon::thread_migrate(th, vcpu);
}).detach();
```

### 2. Use `WorkPool`

#### Headers

`<photon/thread/workerpool.h>`

#### Description

Create a WorkPool to manage multiple vCPUs, and utilize multi-core.

#### Constructor

```cpp
WorkPool(size_t vcpu_num, int ev_engine = 0, int io_engine = 0, int thread_mod = -1);
```

- `vcpu_num` How many vCPUs to be created for this workpool
- `ev_engine` How to init event engine for these vCPUs
- `io_engine` How to init io engine for these vCPUs
- `thread_mod` Threads working mode:
	- -1 for non-thread mode
	- 0 will create a photon thread for every task
	- \>0 will create photon threads from a `thread_pool`. Pool size equals to this number.

#### Public Method

##### 1. Async Call

```cpp
template <typename Task>
int WorkPool::async_call(Task* task);
```

- `async_call` uses an MPMC Queue to deliver messages to multiple vCPUs inside the WorkPool for execution. 
The caller does not wait for execution to complete.
- task is usually a new-ed lambda function. It will be automatically deleted after execution.

See this example:

```cpp
photon::WorkPool pool(4, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE, 32768);
photon::semephore sem;

pool.async_call(new auto ([&]{
    photon::thread_sleep(1);
    sem.signal(1);
}));
```

##### 2. Get vCPU number

```cpp
int get_vcpu_num();
```

:::note
The number is only for the WorkPool. The main OS thread doesn't count.
:::

##### 3. Thread Migrate

WorkPool thread migrate relies on the basic coroutine migrate, not the MPMC Queue.

```cpp
int thread_migrate(photon::thread* th = CURRENT, size_t index = -1UL);
```

- `th` Photon thread that going to migrate
- `index` Which vCPU in pool to migrate to. if index is not in range [0, vcpu_num), for instance, the default value -1UL, 
  it will choose the next vCPU in pool (round-robin).
     
Returns 0 for success, and <0 means failed to migrate.

## Event Engines

Each vCPU has a `MasterEventEngine` that blocks on I/O events and wakes sleeping photon threads via `thread_interrupt()`. Defined in `<photon/io/fd-events.h>`.

### MasterEventEngine

Per-vCPU, drives the idle loop:

- `wait_for_fd(fd, interests, timeout)` — suspend current thread until FD event
- `wait_and_fire_events(timeout)` — block and fire events by calling `thread_interrupt(thread*, EOK)`
- `cancel_wait()` — wake the engine (via eventfd or equivalent)

### CascadingEventEngine

For complex multi-FD waits. Does NOT block the vCPU — only the calling photon thread:

- `add_interest(Event)` / `rm_interest(Event)` — manage FD registrations
- `wait_for_events(data[], count, timeout)` — wait for multiple events

### Event flags

```cpp
EVENT_READ    = 1
EVENT_WRITE   = 2
EVENT_ERROR   = 4
EDGE_TRIGGERED = 0x4000
ONE_SHOT       = 0x8000
```

## Backends

### epoll

Headers: `<photon/io/epoll.cpp>`, `<photon/io/epoll-ng.cpp>`.

- `epoll_create(1)` + eventfd for `cancel_wait`
- `_inflight_events` vector indexed by fd
- ONE_SHOT interests with `data=CURRENT` (photon thread pointer)
- `wait_and_fire_events()`: `epoll_wait()` → `thread_interrupt((thread*)data, EOK)` per event
- `epoll-ng.cpp` is the edge-triggered variant

### io_uring

Header: `<photon/io/iouring-wrapper.h>`.

The most feature-rich backend:

- SQPoll mode, IOPoll mode, registered files
- `io_uring_prep_poll_add` / `multishot` for fd events
- `async_io()` template for: read, write, send, recv, connect, accept, open, mkdir, close, splice, fsync
- Cancellation via `io_uring_prep_cancel`
- Kernel version detection (5.11, 5.15, 5.18, 5.19) for feature gating
- `IouringFixedFileFlag` (bit 32) marks registered FDs

### kqueue

Header: `<photon/io/kqueue.cpp>`. BSD/macOS backend:

- `EVFILT_USER` for self-wakeup (`cancel_wait`)
- `EV_ONESHOT` for single-event waits

### select

Inline stub. Fallback for platforms without epoll/kqueue.

## Async I/O Backends

### libaio

Header: `<photon/io/aio-wrapper.h>`.

- `libaio_pread` / `preadv` / `pwrite` / `pwritev` — requires `O_DIRECT`, aligned buffers
- Requires `fd_events_init()` for event notification

### POSIX AIO

- `posixaio_pread` / `pwrite` / `fsync` / `fdatasync`

## Userspace Networking

| Header | Description |
|--------|-------------|
| `<photon/io/fstack-dpdk.h>` | F-Stack / DPDK: `fstack_socket` / `connect` / `bind` / `listen` / `accept` / `send` / `recv` / `close` |
| `<photon/io/spdknvme-wrapper.h>` | SPDK NVMe block device access |
| `<photon/io/spdkbdev-wrapper.h>` | SPDK generic block device |

## Signal Handling

Header: `<photon/io/signal.h>`.

- `sync_signal_init()` and `sync_signal(signum, handler)` — signal handlers run in a dedicated photon thread
- `block_all_signal()` blocks all signals except `SIGSTOP` / `SIGKILL`

`<photon/io/reset_handle.h>` defines `ResetHandle`, an intrusive list node with a `reset()` virtual method. `reset_all_handle()` is called during photon reinitialization.

## Design Decisions

- **ONE_SHOT semantics.** All backends use one-shot event registration to avoid thundering herd and ensure each event wakes exactly one thread.
- **EOK convention.** Events fire with `EOK` (ENXIO, "Event of NeXt I/O"); the interrupted thread checks `error_number` to distinguish event wakeup from timeout.
- **Thread pointer as event data.** `data=CURRENT` stores the photon thread pointer in the event, enabling direct `thread_interrupt()` without lookup.
- **Separate Master and Cascading engines.** Master drives the idle loop (blocks the vCPU); Cascading handles per-thread multi-FD waits without blocking others.
