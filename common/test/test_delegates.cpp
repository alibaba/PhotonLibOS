#include <gflags/gflags.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <photon/common/alog-functionptr.h>
#include <photon/common/delegates.h>

#include <cstdio>
// examples

struct example {
    int val = 313;
    int foo() {
        LOG_INFO("void foo()");
        return val;
    }
    void foo(char) { LOG_INFO("void foo(char)"); }
    void bar(int) { LOG_INFO("void bar(int)"); }

    void bar(uint64_t) { LOG_INFO("void bar(uint64_t)"); }

    example &operator<<(int) {
        LOG_INFO("example& operator<<(int)");
        return *this;
    }

    bool operator!() {
        LOG_INFO("bool operator!()");
        return true;
    }

    operator bool() {
        LOG_INFO("operator bool()");
        return true;
    }

    void wow() { LOG_INFO("void wow()"); }

    bool op_bool() {
        LOG_INFO("bind operator bool");
        return true;
    }
};

void wow(void *) { LOG_INFO("free function void wow(void*)"); }

DEFINE_DELEGATE_FUNCTION(MemFoo, foo);
DEFINE_DELEGATE_FUNCTION(MemBar, bar);
DEFINE_DELEGATE_CAST_OP(CastBool, bool);

struct example2 : example {
    void foo() { LOG_INFO("foo in ex2"); }
};

TEST(Delegates, basic_example) {
    using namespace photon;
    using namespace DFeature;

    example e;

    using D =
        Delegates<Feature<MemFoo, int(), void(char)>,
                  Feature<MemBar, void(uint64_t)>, Feature<LSH, SelfRef(int)>,
                  Feature<CastBool, bool()>, Feature<LNOT, bool()>>;
    D facade(e);

    facade.bar(1);
    EXPECT_EQ(313, facade.foo());
    facade.foo('a');
    facade << 1 << 1;

    DUtils::bind_func<MemFoo, void()>(facade, nullptr, &wow);
    facade.foo();

    DUtils::bind_func<MemFoo, void()>(facade, &e, &example::wow);
    facade.foo();

    DUtils::bind_func<CastBool>(facade, &e, &example::op_bool);

    LOG_INFO(VALUE(!facade));
    LOG_INFO(VALUE((bool)facade));

    D f2;

    DUtils::bind_func<MemFoo, void(char)>(
        f2, &e, &example::foo);  // if have multiple overloads
    DUtils::bind_func<MemFoo, void()>(f2, nullptr, &wow);
    DUtils::bind_func<MemBar>(f2, &e, &example::bar);
    DUtils::bind_func<LSH>(f2, &e, &example::operator<<);
    DUtils::bind_obj(f2, &e);

    auto lshd = DUtils::get_func<LSH, SelfRef(int)>(f2);
    EXPECT_EQ(lshd._obj, &e);
    LOG_INFO(VALUE(lshd._obj), VALUE(lshd._func));
    auto l2 = DUtils::get_func<LSH>(f2);
    EXPECT_EQ(l2._obj, &e);
    LOG_INFO(VALUE(l2._obj), VALUE(l2._func));
    auto foofirst = DUtils::get_func<MemFoo, int()>(f2);
    EXPECT_EQ(foofirst._obj, &e);
    LOG_INFO(VALUE(foofirst._obj), VALUE(foofirst._func));
    auto foosecond = DUtils::get_func<MemFoo, void(char)>(f2);
    EXPECT_EQ(foosecond._obj, &e);
    LOG_INFO(VALUE(foosecond._obj), VALUE(foosecond._func));

    f2.foo();                                      /// wow1
    f2.foo('a');                                   /// foochar
    Delegate<void, char>(&e, &example::foo)('a');  /// foochar
    f2.bar(1);
    f2.foo();
    f2.foo('a');
    f2 << 1 << 1;

    EXPECT_TRUE(DUtils::bind_obj_and_func(f2, e));
    f2.foo();
    f2.foo(1);
    f2.bar(1);
    f2.foo();
    f2.foo('a');
    f2 << 1 << 1;
    LOG_INFO(VALUE(!f2));
    LOG_INFO(VALUE((bool)f2));
}

class Interface {
    public:
    virtual void foo() = 0;
    virtual int bar(const char *) = 0;
    virtual int foo(char) = 0;
    virtual Interface &operator<<(int) = 0;
};

class Mock : public Interface {
public:
    int cnt[4];
    void foo() {
        cnt[0] ++;
    }
    int bar(const char*) {
        cnt[1] ++;
        return cnt[1];
    }
    int foo(char) {
        cnt[2] ++;
        return cnt[2];
    }
    Interface &operator<<(int) {
        cnt[3] ++;
        return *this;
    }
};

TEST(Delegates, virtuals) {
    using namespace photon;
    using namespace DFeature;
    using D = Delegates<Feature<MemFoo, void(), int(char)>,
                        Feature<MemBar, int(const char *)>,
                        Feature<LSH, SelfRef(int)>>;
    Interface *o = new Mock();
    D d(*o);

    d.foo();
    d.foo('a');
    d.bar("Hello");
    d << 1;

    for (int i=0;i<4;i++) {
        EXPECT_EQ(1, ((Mock*)o)->cnt[i]);
    }
}

int main(int argc, char **arg) {
    ::testing::InitGoogleTest(&argc, arg);
    gflags::ParseCommandLineFlags(&argc, &arg, true);
    return RUN_ALL_TESTS();
}
