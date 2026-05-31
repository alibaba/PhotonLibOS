---
sidebar_position: 9
toc_max_heading_level: 4
---

# RPC 框架

`rpc/` 模块提供高性能 RPC 框架，具备零拷贝序列化、连接池和乱序异步执行能力。以 "zBuffer" 为题发表于 PPoPP'26。

### 命名空间

`photon::rpc::`

### 头文件

`<photon/rpc/rpc.h>`、`<photon/rpc/serialize.h>`、`<photon/rpc/out-of-order-execution.h>`

## 协议

定义于 `<photon/rpc/rpc.h>`。

### 报文头

每条 RPC 消息起始于 40 字节头部：

| 字段 | 描述 |
|------|------|
| MAGIC | 常数 `0x87de5d02e6ab95c7` |
| VERSION | 协议版本 |
| size | 消息总长 |
| FunctionID | interface + method 的组合 |
| tag | 每条请求的相关标识 |

### Stub 与 Skeleton

- **`Stub`** —— RPC 客户端实体。`call<Operation>(req, resp, timeout)` 以零拷贝方式发起请求。
- **`Skeleton`** —— RPC 服务端实体。`add_function()` 注册处理函数，`register_service<Operations...>()` 批量注册，`serve(IStream*)` 运行分发循环。
- **`StubPool`** —— 带 TTL 过期和单次调用超时的连接池。提供 `get_stub(endpoint)`、`put_stub()`、`acquire()`。

## 序列化

定义于 `<photon/rpc/serialize.h>`。类型系统构建在 `iovector` 之上，实现零拷贝。

### 字段类型

| 类型 | 描述 |
|------|------|
| `buffer` | 变长字节缓冲 |
| `fixed_buffer<T>` | 定长类型化缓冲 |
| `array<T>` | 变长类型化数组 |
| `string` | 基于切片的字符串（在 iovector 中的 offset + length） |
| `slice` | 通用切片引用 |
| `iovec_array` | iovec 数组 |
| `sorted_map<K,V>` | 基于序列化数据的二分查找索引 map |

### Archive

- `SerializerIOV` —— 序列化到 iovector。
- `DeserializerIOV` —— 反序列化并提取为 iovector。

### Message 基类

`rpc::Message` 定义 `serialize_fields()` 方法，以及 `add_checksum()` / `validate_checksum()`。`CheckedMessage<Hasher>` 自动加入 CRC32C 完整性校验。

字段注册使用 `PROCESS_FIELDS(...)` 宏配合 `ArchiveBase<Derived>` CRTP，为每种支持的类型提供 `process_field()` 重载。

## 乱序执行

定义于 `<photon/rpc/out-of-order-execution.h>`。在对外呈现同步接口的同时，支持多个异步操作流水线执行。

- **`OutOfOrderContext`** —— 每个操作的上下文，包含 `do_issue`、`do_completion`、`do_collect` 回调，以及 tag、phase、timeout。
- `ooo_issue_operation()` —— 发起异步操作。
- `ooo_wait_completion()` —— 等待完成。
- `ooo_issue_wait()` —— 组合的 issue + wait。
- `ooo_result_collected()` —— 标记结果已被消费。
- `new_ooo_execution_engine()` —— 并发乱序执行引擎工厂。

## 工厂函数

| 工厂 | 描述 |
|------|------|
| `new_rpc_stub()` | 创建 RPC 客户端 stub |
| `new_stub_pool()` | 创建连接池化的 stub pool |
| `new_uds_stub_pool()` | Unix Domain Socket stub pool |
| `new_skeleton()` | 创建 RPC 服务端 skeleton |

## 设计决策

- **零拷贝序列化。** `SerializerIOV` / `DeserializerIOV` 直接在 iovector 上工作，无中间缓冲拷贝。`sorted_map` 在序列化数据上进行二分查找，而不是反序列化为 `std::map`。
- **基于切片的字符串。** 序列化后的字符串保存的是在 iovector 中的 offset + length，而不是副本，从而避免反序列化时的分配。
- **CRC32C 校验。** `CheckedMessage` 使用硬件加速的 CRC32C 进行消息完整性校验。
- **乱序执行。** 多个 RPC 调用可以在单连接上流水线化 —— 同时发起多个请求，再按到达顺序收集响应。
- **StubPool。** 基于 TTL 的连接复用，减少 TCP 握手开销。

## 相关模块

- [网络](./network.md) —— `ISocketStream` 提供 RPC 传输，`TCPSocketPool` 作为 `StubPool` 的底层。
- [通用工具](./common.md) —— `iovector` 用于零拷贝序列化，`crc32c` 用于校验，`Delegate` 用于回调。
