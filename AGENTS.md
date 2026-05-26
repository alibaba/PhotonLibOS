# AGENTS.md

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
