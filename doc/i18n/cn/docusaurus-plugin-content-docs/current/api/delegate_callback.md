---
sidebar_position: 8
toc_max_heading_level: 4
---

# Delegate 与 Callback

Photon 中的 ```Delegate<...>``` 是一种用于高效回调的特定构造，比 ```std::function``` 更高效，同时同样方便。Delegate 是扩展的函数指针，（大多情况下）对自由函数和类成员函数通用。它 ***由两个成员组成***：目标函数的地址（自由函数或类成员函数均可），以及会作为第一个参数传给目标函数的值。对于自由函数，该参数是用户提供的 ```void*``` 类型的任意值；对于类成员函数，该参数则是所涉对象的隐式 ```this``` 指针。创建 Delegate 不涉及内存分配。

它定义在 <photon/common/callback.h>，原型为：

```cpp
template<typename ResultType, typename...ArgumentTypes>
struct Delegate;
```

它定义了一个回调函数，结果类型为 ```ResultType```，参数类型为 ```ArgumentTypes```。目标函数可以是任意类或结构体 ```T``` 的类型为 ```ResultType (T::*)(ArgumentTypes...)``` 的成员函数，也可以是类型为 ```ResultType (*)(void*, ArgumentTypes...)``` 的自由函数。此时第一个参数为 ```void*``` 指针，用来区分不同的回调调用，这在基于函数指针的回调机制中很常见。

下面是一个 Delegate 示例。假设我们有一个 delegate 类型执行某种文件打开操作，并返回一个抽象文件对象的指针。

```cpp
class File;

using OpenCallback = Delegate<File*, const std::string&, const char*> ;
```

我们有一个文件处理函数，通过调用 ```OpenCallback``` delegate 打开文件。

```cpp
void ProcessFiles(vector<string> fileNames, OpenCallback opencb) {
    for (auto& fileName : fileNames) {
        auto file = opencb(fileName, "r");  // 调用 delegate
        // ...
        delete file;
    }
}
```

作为 ```ProcessFiles()``` 的使用者，我们有多种方式提供打开文件的回调。第一种是类成员函数：

```cpp
class MyClass {
    // ...
public:
    File* OpenFile_MF(const string& fileName, const char* mode) { ... }
    //...
};

MyClass myClass;

ProcessFiles(fileNames, { &myClass, &MyClass::OpenFile_MF });
```

第二种是自由函数。本例中 void* 的第一个参数是无用的，但在其他场景下可能有用。

```cpp
File* OpenFile_FF(void*, const string& fileName, const char* mode);

ProcessFiles(fileNames, {nullptr, &OpenFile_FF});
```

第三种是 lambda 函数。

```cpp
auto lambda = [&](const string& fileName, const char* mode) {
    // ...
};

ProcessFiles(fileNames, lambda);
```

注意 ```Delegate<...>``` 显式禁止绑定到临时 lambda 或 functor，以降低 lambda 对象生命周期管理上的风险。

## 相关类型

`<photon/common/callback.h>` 还定义了：

| 类型 | 描述 |
|------|------|
| `Delegate<R, Ts...>` | 零开销可调用：自由函数、成员函数、lambda |
| `Callback<Ts...>` | `Delegate<int, Ts...>` 的别名 |
| `Closure<ARGS...>` | 自删除回调（用于一次性异步操作） |
| `TempDelegate` | 绑定临时 lambda |

`<photon/common/delegates.h>` 定义 `Delegates<Convs...>`，是类型安全的 delegate 门面，配合 `DEFINE_DELEGATE_FUNCTION` 宏在编译期进行方法分发。

`<photon/common/PMF.h>` 提供 `get_member_function_address()`，从成员函数指针提取原始函数指针，处理 x86_64 与 aarch64 上的虚表布局。

## 为什么选 Delegate 而不是 std::function

- **零堆分配。** `Delegate` 就是一对指针（函数地址 + 上下文），从不分配内存。
- **无类型擦除。** 自由函数和 lambda 的分发在编译期确定，成员函数则在构造时（伴随虚表查询）确定。
- **成员函数感知。** 实现在 x86_64 和 aarch64 上都能正确区分普通成员函数与虚成员函数，提取正确的入口点。
- **生命周期安全。** 在编译期显式拒绝绑定临时 lambda，避免 `std::function` 最常见的一类误用。
