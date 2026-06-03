---
sidebar_position: 1
toc_max_heading_level: 4
---

# 生态

`ecosystem/` 目录包含基于 Photon 核心模块构建的更高层客户端与解析器。它们为可选项，通过 CMake 选项 `PHOTON_ENABLE_ECOSYSTEM` 启用。

### 命名空间

`photon::`

### 头文件

`<photon/ecosystem/...>`

## Redis 客户端

`<photon/ecosystem/redis.h>` —— 协程感知的 Redis 客户端。

- 直接在 `ISocketStream` 上实现 RESP（Redis Serialization Protocol）。
- 支持字符串、哈希、列表、集合、有序集合、键操作、脚本、pub/sub、事务。
- 使用 `Delegate` 回调进行响应解析，避免中间缓冲拷贝。
- 连接复用通过 `net/` 中的标准 socket pool 原语提供。

源码：`ecosystem/redis.h`、`ecosystem/redis.cpp`。

## OSS 客户端

`<photon/ecosystem/oss.h>` —— 阿里云对象存储 OSS 客户端。

- 支持 bucket 和 object 操作：put、get、delete、list、multipart upload、copy、metadata。
- 使用 `common/checksum` 和 `common/digest` 辅助，以 OSS V1/V4 签名对请求进行签名。
- 流式上传下载使用 `iovector` 实现零拷贝 I/O。
- XML 响应使用 `simple_dom` 解析。

源码：`ecosystem/oss.h`、`ecosystem/oss.cpp`、`ecosystem/oss_constants.h`。

## simple_dom

`<photon/ecosystem/simple_dom.h>` —— 轻量级、无分配的 JSON、XML、YAML DOM 解析器。

- 三种格式共享同一棵树形表示。
- `SimpleDOM::Document` 从缓冲区解析，通过路径式查询访问节点。
- 适合配置解析和较小的响应体，不适合大型流式文档。
- `rapidjson` 和 `rapidxml`（作为后端使用）的补丁保存在 `ecosystem/patches/`。

源码：`ecosystem/simple_dom.h`、`ecosystem/simple_dom.cpp`、`ecosystem/simple_dom_impl.h`。

## 构建

仅在 `PHOTON_ENABLE_ECOSYSTEM=ON` 时构建生态模块：

```bash
cmake -B build -D PHOTON_BUILD_TESTING=ON -D PHOTON_ENABLE_ECOSYSTEM=ON
cmake --build build -j 8
```

## 相关模块

- [网络](../api/network.md) —— `ISocketStream`、HTTP 客户端、TLS 流，作为传输层使用。
- [通用工具](../api/common.md) —— `iovector`、`Delegate`、校验和、`estring`，被广泛使用。
- [文件系统与 I/O](../api/filesystem-and-io.md) —— 部分生态代码通过 `fs/httpfs` 将远端存储视为文件系统。
