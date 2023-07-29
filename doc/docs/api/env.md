---
sidebar_position: 1
toc_max_heading_level: 3
---

# Env

### Concept

Photon Env consists of different kinds of engines and a simulated coroutine stack in userspace.

### Namespace

`photon::`

### Headers

`<photon/photon.h>`

## API

### init

```cpp
int photon::init(uint64_t event_engine = INIT_EVENT_DEFAULT, 
				 uint64_t io_engine = INIT_IO_DEFAULT);
```

#### Description

Initialize the coroutine stack on current [vCPU](vcpu-and-multicore). Next, you can create multiple Photon [threads](thread) via `thread_create`, and migrate them to other vCPUs. You should **NOT** invode blocking function calls from now on.

The `event_engine` is platform independent. It cooperates with the scheduler and decides the way it polls fd and processes events. Usually we would do a non-blocking `wait` for events on a thread, making the caller thread fall to `SLEEPING`. After the events being processed, it's the engine's responsibility to interrupt the caller thread and make it `READY`.

The `io_engine` will setup ancillary threads running in the background, if necessary.

#### Parameters

- `event_engine` Supported types are:
	
	- `INIT_EVENT_NONE` None, only used in test.
	- `INIT_EVENT_DEFAULT` It will first try `io_uring`, then choose `epoll` if io_uring failed.
	- `INIT_EVENT_EPOLL`
	- `INIT_EVENT_IOURING`
	- `INIT_EVENT_KQUEUE` Only avalaible on macOS or FreeBSD.

:::info
Running an `IOURING` event engine would need the kernel version to be greater than 5.4.
We encourage you to upgrade to the latest kernel so that you could enjoy the extraordinary performance.
:::	

- `io_engine` Supported types are:

	- `INIT_IO_NONE` Don't need any additional IO engines. Just use `libc`'s read/write, or `io_uring`'s async IO if its `event_engine` is set.
	- `INIT_IO_LIBAIO`
	- `INIT_IO_LIBCURL`
	- `INIT_IO_SOCKET_EDGE_TRIGGER`
	- `INIT_IO_EXPORTFS`
	- `INIT_IO_FSTACK_DPDK`

#### Return

Returns 0 on success, returns -1 on error.

----

### fini

```cpp
int photon::fini();
```

#### Description

Destroy and join the ancillary threads of the io engine. Stop the event engine. Deallocate the coroutine stack.

#### Parameters

None

#### Return

Returns 0 on success, returns -1 on error.