---
sidebar_position: 3
toc_max_heading_level: 5
---

# vCPU and Multi-core

### Concept

Photon vCPU == native OS thread

Each vCPU has a scheduler, executing and switching [threads](thread).

### Enable multi-core

Currently there are only two ways in Photon to utilize multiple cores:

### 1. Create OS thread maually, and initialize the Env

```cpp
new std::thread([&]{
	photon::init();
	DEFER(photon::fini());
});
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
	- \>0 will create photon threads in a `thread_pool` of this size

#### Public Method

##### Get vCPU number

```cpp
int get_vcpu_num();
```

:::note
The number is only for the WorkPool. The main OS thread doesn't count.
:::

##### Thread Migrate

```cpp
int thread_migrate(photon::thread* th = CURRENT, size_t index = -1UL);
```

- `th` Photon thread that going to migrate
- `index` Which vcpu in pool to migrate to. if index is not in range [0, vcpu_num), 
  it will choose the next one in pool (round-robin).
     
Returns 0 for success, and <0 means failed to migrate.