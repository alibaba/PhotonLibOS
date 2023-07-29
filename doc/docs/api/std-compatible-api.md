---
sidebar_position: 7
toc_max_heading_level: 4
---

# std-compatible API

We provide a set of Photon API that are fully compatible to C++ std.

Please refer to https://en.cppreference.com/w/cpp/thread for the official documents.

### Namespace

Use `photon_std::` instead of `std::`

### Headers

`<photon/thread/std-compat.h>`

### Supported Classes

- `thread`
- `mutex`
- `condition_variable`
- `recursive_mutex`
- `timed_mutex`
- `lock_guard`
- `unique_lock`

### Supported Functions

- `this_thread::yield()`
- `this_thread::get_id()`
- `this_thread::sleep_for()`
- `this_thread::sleep_until()`

### Extended Functions

- `work_pool_init` Create a global [WorkPool](vcpu-and-multicore#2-use-workpool)
- `work_pool_fini` Destroy the WorkPool
- `this_thread::migrate()` Migrate current thread to another vCPU in the WorkPool

### Example Code

```cpp
int main() {
	photon::init(event_engine, io_engine);
	DEFER(photon::fini());

	photon_std::work_pool_init(8, event_engine, io_engine);
	DEFER(photon_std::work_pool_fini());
}
```	
