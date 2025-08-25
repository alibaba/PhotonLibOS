---
sidebar_position: 8
toc_max_heading_level: 4
---

# Delegate and Callback

```Delegate<...>``` in Photon is a specific construct for
performing efficient callbacks, more efficient than ```std::function```,
while similarly convenient. Delegate is an extended function pointer
that is (mostly) universal to free functions and class member functions.
It ***consists of two members***: the address of the target function
(either free function or class member function), and the first argument
that will be passed to the target function. In case of free functions,
the argument is any value of type ```void*``` provided by the user;
and in case of class member functions, it is the implicit ```this```
pointer of the involved object. Creating a delegate doesn't involve
memory allocation.

It is defined in <photon/common/callback.h>, and it has a prototype as:

```cpp
template<typename ResultType, typename...ArgumentTypes>
struct Delegate;
```

That defines a callback function with a result type ```ResultType```
and argument type(s) ```ArgumentTypes```. The target function can be
a member function of any class or struct ```T``` of type ```ResultType (T::*)(ArgumentTypes...)```. It can also be a free function of type
```ResultType (*)(void*, ArgumentTypes...)```. In this case the first
argument is a pointer of ```void*``` to distinguish individual callback
invocations, as usually found in function pointer based callback mechanisms.

Here is an example of a delegate. Suppose we have a delegate type that
performs some sort of file open operation, and returns a pointer to an
abstract file object.

```cpp
class File;

using OpenCallback = Delegate<File*, const std::string&, const char*> ;
```

And we have a file processing function that opens the files by invoking
a delegate of ```OpenCallback```.

```cpp
void ProcessFiles(vector<string> fileNames, OpenCallback opencb) {
    for (auto& fileName : fileNames) {
        auto file = opencb(fileName, "r");  // invoke the delegate
        // ...
        delete file;
    }
}
```

As the user of ```ProcessFiles()```, we have several ways to provide
the callback for opening files. The first is class member function.

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

And the second is free function. The first parameter of void* is
useless in this example, but it can be useful in other cases.

```cpp
File* OpenFile_FF(void*, const string& fileName, const char* mode);

ProcessFiles(fileNames, {nullptr, &OpenFile_FF});
```

The third is lambda function.

```cpp
auto lambda = [&](const string& fileName, const char* mode) {
    // ...
};

ProcessFiles(fileNames, lambda);
```

Note that ```Delegate<...>``` explicitly forbids binding to
a temporary lambda or functor, in order to minimize the risk
of life-cycle management of the lambda object.
