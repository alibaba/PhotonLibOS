# The thread local variable for coroutines

As we all know, C++11 introduced the `thread_local` keyword to replace the `__thread` provided by the compiler,
or the `specific key` related functions provided by the `pthread` library.

Here is a typical example of using thread_local.

```c++
#include <thread>

static thread_local int i = 0;

int main() {
    auto th = std::thread([]{
        i = 1;
    });
    th.join();
    
    assert(i == 0);
}
```

Photon begins to support TLS for coroutines since version 0.4.0. Due to some limitations, Photon cannot achieve the
same syntax as `thread_local`, but implements it in a close way.

```c++
#include <photon/thread/std-compat.h>

static photon::thread_local_ptr<int, int> pI(0);

int main() {
    if (photon::init())
        abort();
    DEFER(photon::fini());
    
    auto th = photon::std::thread([]{
        *pI = 1;
    });
    th.join();
    
    assert(*pI == 0);
}
```

In this code above, `thread_local_ptr` is a template class that provides pointer-like operators.
You need to pass the appropriate constructor type to its template parameter, which in this example, is also a int.

When users access it in different coroutines, they will always get a separate value.

Below is a more complicated example:

```c++
class Value {
public:
    explicit Value(std::string s) : m_s(std::move(s)) {}
    size_t size() { return m_s.size(); }
private:
    std::string m_s;
};

class A {
public:
    void func();
private:
    static photon::thread_local_ptr<Value, std::string> m_value;
};

static photon::thread_local_ptr<Value, std::string> m_value("123");

void A::func() {
    std::cout << "Value size " << m_value->size() << std::endl;
}
```
