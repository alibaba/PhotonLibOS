---
sidebar_position: 1
toc_max_heading_level: 4
---

# 通用工具

`common/` 模块提供 PhotonLibOS 全局使用的基础工具：日志、字符串、容器、可调用对象抽象、I/O 向量、异步辅助设施、校验和等。

### 命名空间

`photon::`

### 头文件

所有头文件均位于 `<photon/common/...>`。

## 日志

`<photon/common/alog.h>` —— 类型安全、编译期格式化的日志系统。

### 宏

- 日志级别宏：`LOG_DEBUG`、`LOG_INFO`、`LOG_WARN`、`LOG_ERROR`、`LOG_FATAL`
- 限流变体：`LOG_EVERY_T`、`LOG_EVERY_N`、`LOG_FIRST_N`、`LOG_FIRST_N_EVERY_T`
- 错误返回辅助宏：`LOG_ERROR_RETURN(errno, retval, ...)`、`LOG_ERRNO_RETURN(errno, retval, ...)`
- 值回显：`VALUE(x)` 输出 `x=<value>`，适合调试不透明的值

### 格式串

alog 使用反引号（`` ` ``）作为变量占位符，由编译器处理，而不是像 printf 那样在运行时解析。

- 格式并不由格式串本身决定，而是由变量外层的包装器决定。
- 格式串末尾的反引号是多余的 —— 超出反引号数量的参数会自动输出。

```cpp
LOG_WARN("count ` total ", DEC(count).comma(true), DEC(total).comma(true));
LOG_ERROR("failed, ", VALUE(st));   // 不需要结尾反引号
```

### 格式化包装器

| 包装器 | 输出 |
|--------|------|
| `HEX(x)` | 十六进制 |
| `DEC(x)` | 十进制，可选 `.comma(true)` |
| `OCT(x)` | 八进制 |
| `BIN(x)` | 二进制 |
| `FP(x)` | 浮点数 |

### 后端

`ILogOutput` 是抽象接口。内置后端可输出到文件、stderr、stdout 或异步队列，通过 `set_log_output()` 设置。

## 字符串工具

### estring

`<photon/common/estring.h>` —— `estring` 扩展自 `std::string`，`estring_view` 扩展自 `std::string_view`。

额外提供：trim、大小写无关比较、数值转换、惰性 `split()`、高效拼接 `appends()`。

```cpp
std::string key = estring().appends(host, ":", port);

estring().appends(
    path,
    estring::make_conditional_cat_list(has_query, "?", query));
```

`estring::appends` 不会产生中间的 `std::string` 临时对象，并可混合接受 string_view、整数等多种类型。

### 编译期字符串

`<photon/common/conststr.h>` —— 编译期字符串操作：`TString`、`TStrArray`、`DEFINE_ENUM_STR`。

### 字符串构建器

`<photon/common/strbuilder.h>` —— 栈分配的字符串构建器 `strBuilder<N>`。

### 字符串键容器

`<photon/common/string-keyed.h>` —— 以字符串为键的 map/set，避免在查找时构造临时 `std::string`：`unordered_map_string_key`、`map_string_key`、`unordered_map_string_kv`。

## 容器与数据结构

### 侵入式链表

`<photon/common/intrusive_list.h>` —— 侵入式双向链表，支持 `round_robin_next()` 和按谓词拆分。每个节点无堆分配。

### 环形缓冲区

`<photon/common/ring.h>`：
- `RingQueue<T>` —— SPSC 阻塞队列
- `RingBuffer` —— char 特化的环形缓冲区，支持 `readv`/`writev`

### 无锁队列

`<photon/common/lockfree_queue.h>` —— 无锁 MPMC、批量 MPMC、SPSC 队列。`RingChannel<QueueType>` 增加光子协程感知的阻塞，采用可配置的"先 yield 后信号量"策略。基于 turn 的槽位标记避免 ABA 问题。

### 一致性哈希

`<photon/common/consistent-hash-map.h>` —— 基于 vector 的一致性哈希环。

### 有序区间

`<photon/common/ordered_span.h>` —— 有序 span，二分查找插入/查询。

### TTL 容器

`<photon/common/expirecontainer.h>` —— 由光子定时器驱动的自动过期带键容器。`ObjectCache` 提供引用计数的 `Borrow` RAII 访问。

### Identity Pool

`<photon/common/identity-pool.h>` —— 支持构造/析构回调和自动扩缩的对象池，作为 `ThreadPool` 的底层实现。

## 可调用对象抽象

### Delegate

`<photon/common/callback.h>` —— `Delegate<R, Ts...>` 是对自由函数、成员函数、lambda 的零开销可调用包装。无堆分配、无类型擦除。

- `Callback<Ts...>` 是 `Delegate<int, Ts...>` 的别名。
- `Closure<ARGS...>` 是自删除回调。
- `TempDelegate` 绑定临时 lambda。

成员函数调用在 x86_64 与 aarch64 上均能正确处理虚表。

### Delegates 门面

`<photon/common/delegates.h>` —— `Delegates<Convs...>` 是类型安全的 delegate 门面，配合 `DEFINE_DELEGATE_FUNCTION` 宏在编译期进行方法分发。

### PMF 辅助

`<photon/common/PMF.h>` —— `get_member_function_address()` 从成员函数指针提取原始函数指针，处理 x86_64 与 aarch64 上的虚表布局。

## I/O 与流

### IStream

`<photon/common/stream.h>` —— 抽象的 `read`/`readv`/`write`/`writev` 接口。提供 `ReadAll` RAII 辅助以及函数指针自省（`_and_read`、`_and_write`）以判断底层是否真正实现 I/O。

### IMessage

`<photon/common/message.h>` —— 基于 iovec 的数据报 send/recv。

### iovector

`<photon/common/iovector.h>` —— `iovector_view`（非拥有）和 `iovector`（拥有）。提供丰富的 iovec 数组操作：extract、slice、pipe、内置分配器。被 RPC 层和 HTTP 层广泛用于零拷贝 I/O。

### IOAlloc

`<photon/common/io-alloc.h>` —— 基于回调的缓冲区分配器。

### IMessageChannel

`<photon/common/message-channel.h>` —— 消息传递，支持 `OUT_OF_ORDER`、`PIPELINING`、`ROUND_TRIP` 标志。

## 核心工具

### DEFER 与作用域辅助

`<photon/common/utility.h>`：
- `DEFER` —— 作用域退出清理
- `NewObj<T>` —— 两阶段构造模式
- `likely` / `unlikely` —— 分支提示
- 对齐与饱和算术辅助
- `WITH` / `WITH_Release` —— 作用域资源块

### Timeout

`<photon/common/timeout.h>` —— `Timeout` 是基于 `photon::now`（微秒精度全局时间戳）的过期定时器。通过 `expired()`、`timeout_us()`、`timeout_ms()`、`timeout_s()` 查询。

### retval

`<photon/common/retval.h>` —— `retval<T>` 包装返回值并捕获当前 `errno`。

### Range Lock

`<photon/common/range-lock.h>` —— `RangeLock` 提供字节范围锁并检测冲突。`ScopedRangeLock` 是 RAII 守卫。

### Throttle

`<photon/common/throttle.h>` —— 带优先级的令牌桶限流器。

### SingleFlight

`<photon/common/singleflight.h>` —— 请求合并，防止缓存击穿。

### RCUPtr

`<photon/common/rcuptr.h>` —— `RCUPtr<T>` 是用户态 QSBR RCU 保护指针。

### Generator

`<photon/common/generator.h>` —— `Generator<Derived, ValueType>` 是基于 CRTP 的惰性迭代器生成器。

### EventLoop

`<photon/common/event-loop.h>` —— 通用事件循环，提供 `Wait4Events` / `OnEvents` 回调。

### 性能计数器

`<photon/common/perf_counter.h>` —— `REGISTER_PERF` / `REPORT_PERF` 宏，支持 `TOTAL`、`ACCUMULATE`、`AVERAGE` 类型。

### Hash 组合器

`<photon/common/hash_combine.h>` —— Boost 风格的可变参数 hash 组合器。

### UUID

`<photon/common/uuid.h>` 和 `uuid4.h` —— UUID v4 生成、解析、字符串转换。

## 校验和

所有校验和头文件位于 `<photon/common/checksum/...>`。

| 头文件 | 描述 |
|--------|------|
| `crc32c.h` | CRC32C，自动检测硬件加速（SSE4.2 / ARM CRC）。支持 series、combine、trim。 |
| `crc64ecma.h` | CRC64-ECMA，自动选择硬件/软件实现。 |
| `xxhash.h` | xxHash 32-bit（纯软件）。 |
| `digest.h` | 基于 OpenSSL EVP 的摘要：`sha1`、`sha256`、`sha512`、`md5`、`HMAC_*`。 |

## 设计决策

- **编译期格式串。** alog 在编译期处理基于反引号的格式串，而不是像 printf 那样在运行时解析。
- **Delegate 优于 `std::function`。** `Delegate` 零开销（无堆分配、无类型擦除），支持成员函数并能正确处理虚表分发，且在编译期显式拒绝绑定临时 lambda 以降低生命周期风险。
- **侵入式容器。** 侵入式链表和无锁队列避免逐节点堆分配，被调度器、reset handle 和对象池广泛使用。
- **无锁队列。** 基于 turn 的槽位标记避免 ABA 问题。`RingChannel` 增加光子协程感知的阻塞，采用"先 yield 后信号量"策略。
