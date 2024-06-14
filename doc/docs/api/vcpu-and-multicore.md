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