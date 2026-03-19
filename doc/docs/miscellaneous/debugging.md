---
sidebar_position: 1
toc_max_heading_level: 5
---

# Debugging

`photongdb.py` is a GDB extension for debugging processes with Photon threads. It supports both live debugging and coredump analysis.

It provides extra GDB commands to inspect Photon thread status and view stack traces of background threads.

## Build

Build with `-D CMAKE_BUILD_TYPE=Debug` and `-fno-omit-frame-pointer` for best debugging experience.

## Usage

When using GDB for debugging (attach to process or analyze coredump), load the script to enable this extension:

```gdb
(gdb) source <repo_dir>/tools/photongdb.py 
INFO Photon-GDB-extension v2 loaded
INFO Commands: photon_ls, photon_fr <n>, photon_bt [n], photon_ps, photon_current
INFO Use photon_fr to select thread, then photon_bt to show its backtrace
```

### Commands

| Command | Description |
|---------|-------------|
| `photon_ls` | List all Photon threads across all vCPUs |
| `photon_fr <n>` | Select thread n for subsequent operations |
| `photon_bt [n]` | Show backtrace of selected (or specified) thread |
| `photon_ps` | Show all threads with their backtraces |
| `photon_current` | Show currently selected Photon thread info |

Legacy commands (`photon_init`, `photon_fini`, `photon_rst`) are kept for backward compatibility but no longer needed.

#### `photon_ls`

List all Photon threads on all vCPUs. Output format is similar to GDB's `info threads`.

```gdb
(gdb) photon_ls
  Thread 0, 0xffffad92cf80 (CURRENT) vCPU 1 (0xaaab0e9b4e00)
      at photon::vcpu_ctx::run () at thread/thread.cpp:1295
  Thread 1, 0xaaab0eb78030 (STANDBY) vCPU 1 (0xaaab0e9b4e00)
      at photon::thread_stub () at thread/thread.cpp:502
  Thread 2, 0xaaab0ec98030 (SLEEP) vCPU 1 (0xaaab0e9b4e00)
      at photon::thread_usleep_defer () at thread/thread.cpp:952
* Thread 3, 0xaaab0ed44030 (SLEEP) vCPU 1 (0xaaab0e9b4e00)
      at photon::mutex::lock () at thread/thread.cpp:1605
```

The `*` marks the currently selected thread (via `photon_fr`).

#### `photon_fr`

Select a Photon thread for subsequent `photon_bt` and `photon_current` commands.

```gdb
(gdb) photon_fr 3
[Switching to Thread 3, 0xaaab0ed44030 (SLEEP) vCPU 1 (0xaaab0e9b4e00)]

(gdb) photon_fr
Current selected thread: 3
```

#### `photon_bt`

Show backtrace of the selected thread (or specify a thread index). Output format is similar to GDB's `backtrace`.

```gdb
(gdb) photon_bt
#0  0x0000ffffaf79bde0 in photon::thread_usleep_defer ()
#1  0x0000ffffaf79b410 in photon::mutex::lock ()
#2  0x0000aaab0e9f6a48 in TestFunc () at test.cpp:42

(gdb) photon_bt 0
#0  0x0000ffffaf7a9e58 in photon::vcpu_ctx::run () at thread/thread.cpp:1295
#1  0x0000ffffaf7aa0c4 in photon::vcpu_task () at thread/thread.cpp:1342
```

#### `photon_ps`

Show all Photon threads with their backtraces.

```gdb
(gdb) photon_ps
Thread 0, 0xffffad92cf80 (CURRENT) vCPU 1 (0xaaab0e9b4e00)
#0  0x0000ffffaf7a9e58 in photon::vcpu_ctx::run ()
#1  0x0000ffffaf7aa0c4 in photon::vcpu_task ()

Thread 1, 0xaaab0eb78030 (STANDBY) vCPU 1 (0xaaab0e9b4e00)
#0  0x0000ffffaf79b7f4 in photon::thread_stub ()

Thread 2, 0xaaab0ec98030 (SLEEP) vCPU 1 (0xaaab0e9b4e00)
#0  0x0000ffffaf79bde0 in photon::thread_usleep_defer ()
#1  0x0000aaab0e9f5c20 in main ()
```

#### `photon_current`

Show the currently selected Photon thread info.

```gdb
(gdb) photon_current
SLEEP [3] 0xaaab0ed44030 vCPU 1 (0xaaab0e9b4e00)
```

### Coredump Debugging

The extension fully supports coredump debugging. Simply load the script and use the same commands:

```bash
gdb /path/to/executable /path/to/core.12345
(gdb) source /path/to/photongdb.py
(gdb) photon_ls
(gdb) photon_ps
```

### Notes

- For best stack unwinding results, compile with `-fno-omit-frame-pointer`
- CURRENT threads show the actual OS thread stack from GDB (live debugging)
- RUNNING state is shown for coredump analysis where GDB context is unavailable
- Works with multiple vCPUs in multi-threaded applications
