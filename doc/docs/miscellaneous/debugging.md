---
sidebar_position: 1
toc_max_heading_level: 5
---

# Debugging

`photongdb.py` is a gdb extension to help debugging running process with photon threads.

It provides a group of extra gdb command to inspect photon thread status, even able to switch to background photon
stack to inspect stack frames and variables if needed.

## Build

Build with `-D CMAKE_BUILD_TYPE=Debug`

## Usage

When using GDB to debugging (debug running or attaching), load the script to enable this extension

```gdb
(gdb) source <repo_dir>/tools/photongdb.py 
INFO Photon-GDB-extension loaded
```
It will print hint to tells that extension is loaded.
Extra gdb-commands are added to environment:

:::note
The `repo_dir` in debugging machine should be the same as the one in build machine.
:::

```gdb
photon_current
photon_ls
photon_init
photon_fini
photon_rst
photon_fr
```

### Commands

#### `photon_init`

Initializing photon thread lookup mode.

This command will store current stack registers in gdb value `$saved_rsp`, `$saved_rbp`, `$saved_rip`, and enabling
commands in photon thread lookup mode.

Since to inspect background photon threads needs simulating stack switch for gdb, it will change some registers.
All commands in this group should be called only after entered photon debugging mode.

During photon debugging mode, user should NEVER try to continue running (even step in) before exit the mode, or
process running status will be messed up, result in unpredictable failure.

```gdb
(gdb) photon_init
WARNING Entered photon thread lookup mode. PLEASE do not trying step-in or continue before `photon_fini`

(gdb) p $saved_rsp
$1 = 140737488343040
(gdb) p $saved_rbp
$2 = 140737488343040
(gdb) p $saved_rip
$3 = 4358880

(gdb) photon_ls
CURRENT [0] 0x5555558a9e20 0x7fffffffdb30 0x7fffffffe1a0 0x55555556a029
READY [1] 0x7ffff6afb340 0x7ffff6afb1b0 0x7ffff6afb1d0 0x5555555696e0
SLEEP [2] 0x5555558c0740 0x5555558c0310 0x5555558c0310 0x5555558c0740
SLEEP [3] 0x7ffff6165b40 0x7ffff6165690 0x7ffff6165690 0x7ffff6165b40
```

:::info
There are two situations:
1. You have set a `breakpoint` in a specific line of your code.
2. You suspend the `gdb` by `ctrl + C`, and the program halts inside libc or syscall.

For situation 2, you MUST first switch to the Photon code, for instance, by the command of `frame 3`.
You should make sure the current stack has the `photon::CURRENT` symbol when initializing.

After init, you MUST go back to the top frame by the command of `frame 0`.
:::


#### `photon_current`

Print out `CURRENT` running photon thread struct. No side effects, can be called any time.

```gdb
(gdb) photon_current
CURRENT {<intrusive_list_node<photon::thread>> = {<__intrusive_list_node> = {__prev_ptr = 0x7ffff2bf9bc0, __next_ptr = 0x7ffff2bf9bc0}, <No data fields>}, state = photon::RUNNING, error_number = 0, idx = -172836528, flags = 0, reserved = 7564776, joinable = false, shutting_down = false, lock = {_lock = {<std::__atomic_flag_base> = {_M_i = false}, <No data fields>}}, waitq = 0x0, vcpu = 0x7ffff7db5600 <photon::vcpus>, start = 0x736de8, arg = 0x0, retval = 0x736fe8, buf = 0x7ffff7dd46c0 <(anonymous namespace)::c_locale_impl> "\020", stack = {_ptr = 0x7fffffffd0c8}, ts_wakeup = 0, cond = {<photon::waitq> = {q = {th = 0x0, lock = {_lock = {<std::__atomic_flag_base> = {_M_i = false}, <No data fields>}}}}, <No data fields>}}
```

#### `photon_ls`

List all photon threads on current vCPU. No side effects, can be called any time.

```gdb
(gdb) photon_ls
CURRENT [0] 0x5555558a9e20 0x7fffffffdb30 0x7fffffffe1a0 0x55555556a029
READY [1] 0x7ffff6afb340 0x7ffff6afb1b0 0x7ffff6afb1d0 0x5555555696e0
SLEEP [2] 0x5555558c0740 0x5555558c0310 0x5555558c0310 0x5555558c0740
SLEEP [3] 0x7ffff6165b40 0x7ffff6165690 0x7ffff6165690 0x7ffff6165b40
```

:::info
This command will only work after `photon_init`
:::

#### `photon_fr`

Look into any photon thread stack. Use with gdb `bt`.

```gdb
(gdb) photon_fr 1
SWITCH to 0x7ffff2bf9ac8 0x7ffff2bf9ae0 0x7ffff5014ceb
(gdb) bt
#0  photon::switch_context (from=0x7ffff2bf9bc0, to=0x736c50) at photon/thread.cpp:478
#1  0x00007ffff5014d22 in photon::switch_context (from=0x7ffff2bf9bc0, new_state=photon::READY, to=0x736c50) at photon/thread.cpp:482
#2  0x00007ffff5010b51 in photon::thread_yield_to (th=0x736c50) at photon/thread.cpp:575
#3  0x00007ffff5010465 in photon::thread_stub () at photon/thread.cpp:386
#4  0x0000000000000000 in ?? ()
(gdb) photon_fr 0
SWITCH to 0x7fffffffd000 0x7fffffffd000 0x4282e0
(gdb) bt
#0  0x00000000004282e0 in <lambda(ILogOutput*)>::operator()(ILogOutput *) const (__closure=0x7fffffffd044, __output___LINE__=0x706280 <_log_output_null>)
    at test/test.cpp:132
#1  0x00000000004488ba in LogBuilder<log_format()::<lambda(ILogOutput*)> >::~LogBuilder(void) (this=0x7fffffffd040, __in_chrg=<optimized out>)
    at test/../alog.h:519
#2  0x0000000000428398 in log_format () at test/test.cpp:133
#3  0x00000000004283c9 in ALog_fmt_perf_1m_Test::TestBody (this=0x736c00) at test/test.cpp:140
#4  0x00000000004b6d13 in void testing::internal::HandleExceptionsInMethodIfSupported<testing::Test, void>(testing::Test*, void (testing::Test::*)(), char const*) ()
#5  0x00000000004abeaa in testing::Test::Run() ()
#6  0x00000000004abff8 in testing::TestInfo::Run() ()
#7  0x00000000004ac0d5 in testing::TestCase::Run() ()
#8  0x00000000004ac3c0 in testing::internal::UnitTestImpl::RunAllTests() ()
#9  0x00000000004b7223 in bool testing::internal::HandleExceptionsInMethodIfSupported<testing::internal::UnitTestImpl, bool>(testing::internal::UnitTestImpl*, bool (testing::internal::UnitTestImpl::*)(), char const*) ()
#10 0x00000000004ac6a3 in testing::UnitTest::Run() ()
#11 0x0000000000457f24 in RUN_ALL_TESTS () at ../../.dep_create_cache/var/alicpp/apsara/alicpp/built/gcc-4.9.2/gtest-1.7.0/include/gtest/gtest.h:2288
#12 0x0000000000447737 in main (argc=1, argv=0x7fffffffd4d8) at test/test.cpp:1761
```

:::info
This command will only work after `photon_init`
:::

#### `photon_fini`

Finish photon thread lookup mode, restore registers

This will restore registers, exit photon thread lookup mode, so it is able to continue running after `photon_init` called.

```gdb
(gdb) photon_fini
WARNING Finished photon thread lookup mode.
```