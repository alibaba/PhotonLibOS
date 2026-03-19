---
sidebar_position: 1
toc_max_heading_level: 5
---

# 调试

`photongdb.py` 是一个 GDB 扩展，用于调试使用 Photon 线程的进程。支持实时调试和 coredump 分析。

它提供了额外的 GDB 命令来查看 Photon 线程状态和后台线程的调用栈。

## 构建

使用 `-D CMAKE_BUILD_TYPE=Debug` 和 `-fno-omit-frame-pointer` 编译以获得最佳调试体验。

## 使用方法

使用 GDB 调试时（附加到进程或分析 coredump），加载脚本以启用此扩展：

```gdb
(gdb) source <repo_dir>/tools/photongdb.py 
INFO Photon-GDB-extension v2 loaded
INFO Commands: photon_ls, photon_fr <n>, photon_bt [n], photon_ps, photon_current
INFO Use photon_fr to select thread, then photon_bt to show its backtrace
```

### 命令

| 命令 | 描述 |
|------|------|
| `photon_ls` | 列出所有 vCPU 上的所有 Photon 线程 |
| `photon_fr <n>` | 选择第 n 个线程用于后续操作 |
| `photon_bt [n]` | 显示选中（或指定）线程的调用栈 |
| `photon_ps` | 显示所有线程及其调用栈 |
| `photon_current` | 显示当前选中的 Photon 线程信息 |

旧版命令（`photon_init`, `photon_fini`, `photon_rst`）为向后兼容保留，但不再需要。

#### `photon_ls`

列出所有 vCPU 上的所有 Photon 线程。输出格式类似于 GDB 的 `info threads`。

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

`*` 标记当前选中的线程（通过 `photon_fr` 选择）。

#### `photon_fr`

选择一个 Photon 线程，用于后续的 `photon_bt` 和 `photon_current` 命令。

```gdb
(gdb) photon_fr 3
[Switching to Thread 3, 0xaaab0ed44030 (SLEEP) vCPU 1 (0xaaab0e9b4e00)]

(gdb) photon_fr
Current selected thread: 3
```

#### `photon_bt`

显示选中线程（或指定线程索引）的调用栈。输出格式类似于 GDB 的 `backtrace`。

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

显示所有 Photon 线程及其调用栈。

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

显示当前选中的 Photon 线程信息。

```gdb
(gdb) photon_current
SLEEP [3] 0xaaab0ed44030 vCPU 1 (0xaaab0e9b4e00)
```

### Coredump 调试

该扩展完全支持 coredump 调试。只需加载脚本并使用相同的命令：

```bash
gdb /path/to/executable /path/to/core.12345
(gdb) source /path/to/photongdb.py
(gdb) photon_ls
(gdb) photon_ps
```

### 注意事项

- 为获得最佳栈展开效果，请使用 `-fno-omit-frame-pointer` 编译
- CURRENT 线程显示 GDB 获取的实际操作系统线程栈（实时调试）
- RUNNING 状态用于 coredump 分析，此时 GDB 上下文不可用
- 支持多线程应用程序中的多个 vCPU
