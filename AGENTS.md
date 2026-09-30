# AGENTS.md

This file contains the coding conventions, build instructions, and common pitfalls that apply across the repository. Detailed architecture and per-module design documentation lives in `doc/docs/` — see the index at the bottom.

## Coding Principles

### .h and .cpp files
- .h files should include only things that are supposed to be shared with outside. Try best to minimize other things in .h files.
- For classes defined entirely within a .cpp file (not exposed via header), write all method bodies inline inside the class body. Do not separate declarations and out-of-class definitions. Let all members be public.
- .cc extension is used for assistant programs that are not included in normal compilation.

### Logging
- Log lines can be written up to 2x the normal code line width, reducing unnecessary line breaks.

#### Format string
- Use backtick (`` ` ``) as a variable placeholder inside format strings to separate text from variables. Example: ``LOG_WARN("count ` total ", VALUE(count), VALUE(total)``.
- Trailing backticks at the end of a format string are unnecessary — extra arguments beyond the number of backticks are output automatically. Example: ``"failed, ", VALUE(st)`` not ``"failed, `", VALUE(st)``.
- Formats are not actually defined in format-strings, instead they are defined by wrappers of the variables. Example: ``LOG_WARN("count ` total ", DEC(count).comma(true), DEC(total).comma(true)`` will output the integers with commas.
- Users can define custom output operators for their variables / objects.

#### VALUE() macro
- ``VALUE(x)`` outputs both the variable name and its value (e.g. ``VALUE(st)`` → ``st=3``), which aids debugging.
- Never add redundant name prefixes before ``VALUE()`` since it already includes the variable name. Example: use ``"failed, ", VALUE(st)`` not ``"failed, st=", VALUE(st)``.
- Use judgment on whether ``VALUE()`` is needed: opaque values like status codes benefit from ``VALUE()`` (e.g. ``VALUE(st)``), but context-clear values like loop indices or counts can omit it.

#### LOG_ERROR_RETURN / LOG_ERRNO_RETURN
- When ``LOG_ERROR`` and ``return`` are used together (possibly with ``errno`` set in between), prefer ``LOG_ERROR_RETURN(errno_val, retval, ...)`` instead.
- If the log message should also print the current errno, use ``LOG_ERRNO_RETURN(errno_val, retval, ...)``.
- **Log at every error detection point**: every place that detects an error (sets errno and returns an error code) must use ``LOG_ERROR_RETURN`` to output a log and set errno — no silent error returns.
- **Exception — simple wrappers don't repeat**: if a function is a thin wrapper that just propagates the return value of a callee which already logged the error, do not log again or re-set errno. Example: `if (sub_func() < 0) return -1;` where `sub_func` already used ``LOG_ERROR_RETURN``.
- **Abstraction boundary = new log**: when the abstraction level changes (e.g. QAT hardware error → LZ4 compress failure), a new ``LOG_ERROR_RETURN`` is appropriate to re-describe the error at the higher level, and errno may be re-set. If the lower layer already set a meaningful errno, use ``LOG_ERRNO_RETURN`` to output it.
- **if + LOG_ERROR_RETURN on separate lines**: when an `if` body consists solely of `LOG_ERROR_RETURN` or `LOG_ERRNO_RETURN`, omit braces and put the macro on its own line. Example:
  ```cpp
  if (!ptr)
      LOG_ERROR_RETURN(ENOMEM, -1, "allocation failed");
  ```
  Not: `if (!ptr) { LOG_ERROR_RETURN(ENOMEM, -1, "allocation failed"); }`

### RAII
Use RAII as long as it is not troublesome.

#### DEFER macro usage
- Use `DEFER` macro for resource cleanup in initialization functions (e.g. `create()` patterns) to avoid repetitive manual cleanup on each error path.
- Use the `bool ok = false` pattern: each DEFER checks `!ok` before cleaning up; on success set `ok = true` to cancel all deferred cleanups.
- When multiple consecutive `DEFER` statements serve the same logical purpose, combine them into a single `DEFER({ ... })` block.

#### SCOPED_LOCK
- Use `SCOPED_LOCK(lock)` instead of manual `lock.lock()`/`lock.unlock()` to ensure the lock is always released, even on early returns or exceptions.
- Applies to any lock type that `photon::locker` supports (spinlock, mutex, etc.).

### Types
- Prefer fixed-width integer types (`uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`, and their signed counterparts) over verbose built-in names (`unsigned char`, `unsigned short`, `unsigned int`, `unsigned long`, `unsigned long long`).
- The only common exceptions are `size_t` for byte counts / array indices (where the width tracks the address space) and `int` for small loop counters or values where a specific width has no semantic meaning.
- For reading/writing a small fixed number of bytes (1, 2, 4, or 8) at a pointer, prefer a C-style cast over `memcpy`/`memset`/`std::copy`. Example: `*(uint32_t*)dst = value;` to write, `uint32_t v = *(const uint32_t*)src;` to read.
- This is clearer and produces tighter code than the equivalent `memcpy(&v, src, 4)`, and avoids the verbosity of `*reinterpret_cast<uint32_t*>(dst)` for such a common idiom.
- `memset`/`memcpy` remain appropriate for variable-length data and for zeroing structures larger than 8 bytes.
- Alignment and byte order are the caller's responsibility; on little-endian hosts (x86_64, arm64) this reads/writes little-endian values directly, which matches many network and on-disk wire formats.

### String concatenation
- When concatenating multiple parts into a string, prefer `estring().appends(a, b, c)` over `std::string(a) + b + c`.
- `estring::appends` avoids creating intermediate `std::string` temporaries and accepts mixed types (string_view, integers, etc.) without manual conversion.
- Example: `std::string key = estring().appends(host, ":", port);` not `std::string key = std::string(host) + ":" + std::to_string(port);`
- For conditional parts, use `estring::make_conditional_cat_list(cond, parts...)` which appends only when `cond` is true:
  ```cpp
  estring().appends(
      path,
      estring::make_conditional_cat_list(has_query, "?", query));
  ```

### IStream: `read`/`write` vs `recv`/`send`
- `read(buf, count)` and `write(buf, count)` guarantee completion of the full `count` bytes unless an error or EOF occurs. They are atomic from the caller's perspective.
- `recv(buf, count)` and `send(buf, count)` perform a single receive/send operation and may return fewer bytes than requested.
- When an exact amount must be transferred, prefer `read`/`write`. Use `recv`/`send` only when partial transfer is acceptable.

### Code Modification
- Separate moves from changes: when moving code, only move. Do not rename, restructure, or optimize at the same time.
- Understand the original purpose before making changes. The existing layout is not accidental.
- When modifying or optimizing existing code, respect the original design decisions — data structures, interfaces, implementation patterns, etc. Only change what's necessary.

### Trimming white space(s)
- Trim trailing white space(s) of every line.
- Trim excessive new lines at the end of every file.

## Build & Test

### Prerequisites

- Debian/Ubuntu
```bash
sudo apt-get install git cmake libssl-dev libcurl4-openssl-dev libaio-dev zlib1g-dev libgtest-dev libgmock-dev libgflags-dev libfuse-dev libgsasl7-dev nasm
```

- RHEL/CentOS/Fedora
```bash
sudo dnf install git cmake openssl-devel libcurl-devel libaio-devel zlib-devel gtest-devel gmock-devel gflags-devel fuse-devel gsasl-devel nasm
```

- macOS
```bash
brew install cmake openssl@1.1 pkg-config gflags googletest gsasl nasm
```

### Configure
```bash
# Linux
cmake -B build -D PHOTON_BUILD_TESTING=ON -D CMAKE_BUILD_TYPE=MinSizeRel

# macOS
cmake -B build -D PHOTON_BUILD_TESTING=ON -D CMAKE_BUILD_TYPE=MinSizeRel -D PHOTON_CXX_STANDARD=17 -D OPENSSL_ROOT_DIR=/usr/local/opt/openssl@1.1
```

### Build
```bash
cmake --build build -j 8
# Output: build/output/
```

### Test
```bash
cd build && ctest
```

## Include Paths

The `include/photon/` directory contains symlinks to source headers, mirroring the source tree structure. All headers are included as:

```cpp
#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/thread/thread.h>
#include <photon/net/socket.h>
```

**Internal includes** (within the same module) may use relative paths: `#include "../alog.h"`. Cross-module includes always use `<photon/module/header.h>`.

The build system sets `include/` as a public include directory for `photon_shared` and `photon_static` targets. Test and example targets link against `photon_shared` to inherit the include path.

## Testing

**Framework:** Google Test (gtest). Tests include `../../test/gtest.h` which wraps `<gtest/gtest.h>` and suppresses sign-compare warnings. Some tests also include `../../test/ci-tools.h` for CI helpers.

**Location:** Each module has a `test/` subdirectory (e.g., `common/test/`, `thread/test/`, `net/http/test/`). Test files are named `test_<feature>.cpp` or `test-<feature>.cpp`.

**CMake pattern:** Each `test/` directory has its own `CMakeLists.txt` following this template:

```cmake
add_executable(test-<name> <name>.cpp)
target_link_libraries(test-<name> PRIVATE photon_shared)
add_test(NAME test-<name> COMMAND $<TARGET_FILE:test-<name>>)
```

**To add a new test:**
1. Create `test_foo.cpp` in the appropriate `<module>/test/` directory
2. Include module headers via relative paths (`"../foo.h"`) and cross-module headers via `<photon/module/header.h>`
3. Add the three-line CMake block above to that directory's `CMakeLists.txt`
4. Build and run: `cmake --build build -j 8 && cd build && ctest -R test-foo`

**Test initialization:** Tests that exercise coroutine or I/O features must call `photon::init()` / `photon::fini()` in their `main()` or in a gtest fixture's `SetUp`/`TearDown`.

## Common Pitfalls

**Using `std::mutex` instead of `photon::mutex` in coroutines:** `std::mutex` blocks the OS thread (vCPU), stalling all coroutines on that vCPU. Always use `photon::mutex` (or `photon_std::mutex`) inside photon threads.

**Calling photon APIs before `photon::init()`:** The current OS thread must be initialized as a vCPU before any photon API (thread creation, sync primitives, I/O) can be used. Call `photon::init()` first. If using WorkPool, each worker OS thread is initialized automatically.

**Forgetting `photon::fini()` on exit:** Skips cleanup of ancillary threads (timestamp updater, etc.) and registered `fini_hook` callbacks.

**Blocking syscalls in coroutines:** Direct `sleep()`, `poll()`, `select()`, or blocking `read()`/`write()` on non-photon file descriptors will block the vCPU. Use photon's coroutine-aware I/O or `thread_usleep()` instead.

**Stack overflow:** Default coroutine stack is 8MB. Deep recursion or large stack-allocated buffers may exceed it. Use `thread_create11()` with an explicit `stack_size` parameter for coroutines that need more.

## Conventions

- **Ownership:** `ownership` parameter (typically `bool`) controls whether wrapper objects delete the wrapped object on destruction
- **Timeout:** All I/O operations accept `Timeout` objects integrating with `photon::now` (microsecond-precision global timestamp)
- **Error handling:** Functions return -1 or nullptr on error, set `errno`, and log via `LOG_ERROR_RETURN`
- **Thread safety:** Photon sync primitives (mutex, semaphore, condition_variable) are coroutine-aware; std:: equivalents block the OS thread

## Detailed Documentation

Descriptive architecture and per-module design documentation lives under `doc/docs/` (source for the Docusaurus site). Read these on demand when working on the corresponding area:

| When working on... | Read |
|--------------------|------|
| Overall architecture, module relationships | `doc/docs/introduction/photon-architecture.md` |
| Coroutine runtime, scheduler, sync primitives | `doc/docs/api/thread.md`, `doc/docs/api/lock-and-synchronization.md`, `doc/docs/api/vcpu-and-multicore.md` |
| Event engines (epoll / io_uring / kqueue), signal handling | `doc/docs/api/vcpu-and-multicore.md` (Event Engines section) |
| Socket, HTTP client/server, TLS, connection pool | `doc/docs/api/network.md` |
| Filesystem VFS, local fs, HTTP fs, cache layer | `doc/docs/api/filesystem-and-io.md` |
| RPC framework, zero-copy serialization | `doc/docs/api/rpc.md` |
| Logging, string utilities, containers, delegates | `doc/docs/api/common.md` |
| Redis / OSS / simple_dom (JSON/XML/YAML) | `doc/docs/ecosystem/overview.md` |
| Delegate / Callback internals | `doc/docs/api/delegate_callback.md` |
| std-compatible API | `doc/docs/api/std-compatible-api.md` |
| Building and integrating | `doc/docs/introduction/how-to-build.md`, `doc/docs/introduction/how-to-integrate.md` |
| Debugging | `doc/docs/miscellaneous/debugging.md` |
