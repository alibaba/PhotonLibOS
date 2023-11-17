/*
Copyright 2022 The Photon Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#define protected public
#define private public

#include "../generator.h"
#include "../estring.h"
#include "../alog.cpp"
#include "../alog-audit.h"
#include "../identity-pool.h"
#include "../ring.cpp"
#include "../alog-stdstring.h"
#include "../alog-functionptr.h"
#include "../utility.h"
#include "../consistent-hash-map.h"
#include "../string-keyed.h"
#include "../range-lock.h"
#include "../expirecontainer.h"
#include <photon/thread/timer.h>
#include <photon/thread/thread11.h>

#undef private
#undef protected
#include "../iovector.cpp"

#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#include <memory>
#include <string>
//#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>
//#include <malloc.h>
#ifndef __clang__
#include <gnu/libc-version.h>
#endif

using namespace std;

char str[] = "2018/01/05 21:53:28|DEBUG| 2423423|test.cpp:254|virtual void LOGPerf_1M_memcpy_Test::TestBody():aksdjfj 234:^%$#@341234  hahah `:jksld88423CACE::::::::::::::::::::::::::::::::::::::::::::::::::::";

TEST(ring, round_up_to_exp2)
{
    EXPECT_EQ(RingBase::round_up_to_exp2(0), 1);
    EXPECT_EQ(RingBase::round_up_to_exp2(1), 1);

    uint32_t i = 2;
    for (uint32_t exp2 = 2; exp2 <= (1<<25); exp2 *= 2)
        for ((void)i; i <= exp2; ++i)
            EXPECT_EQ(RingBase::round_up_to_exp2(i), exp2);
}

int rq_step = 0;
bool rq_test_exited;
void* ring_queue_pusher(void* arg)
{
    rq_test_exited = false;
    DEFER(rq_test_exited = true);
    auto* q = (RingQueue<int>*)arg;
    for (int i=0; i<10; ++i)
    {
        ++rq_step;
        LOG_DEBUG("pushing back ", VALUE(rq_step));
        q->push_back(rq_step);
        LOG_DEBUG("pushed");
    }
    return nullptr;
}

TEST(ring, queue)
{
    rq_step = 0;
    RingQueue<int> q(8);
    thread_create(&ring_queue_pusher, &q);
    for (int i = 0; i < 7; ++i)
    {
        LOG_DEBUG("poping front");
        int v;
        q.pop_front(v);
        LOG_DEBUG("poped ", VALUE(v));
        EXPECT_EQ(v, i + 1);
//        EXPECT_EQ(rq_step, 8);
    }
    for (int i = 0; i < 3; ++i)
    {
        LOG_DEBUG("poping front");
        int v;
        q.pop_front(v);
        LOG_DEBUG("poped ", VALUE(v));
        EXPECT_EQ(v, i + 8);
//        EXPECT_EQ(rq_step, 10);
    }
}

void* ring_queue_writer(void* arg)
{
    rq_test_exited = false;
    DEFER(rq_test_exited = true);
    auto* b = (RingBuffer*)arg;
    for (int i = 0; i< 10; ++i)
    {
        LOG_DEBUG("write");
        auto ret = b->write(str, LEN(str));
        EXPECT_EQ(ret, LEN(str));
        LOG_DEBUG("written ", VALUE(ret));
    }
    LOG_DEBUG("exit");
    return nullptr;
}

TEST(ring, buffer)
{
    RingBuffer buf(32);
    thread_create(&ring_queue_writer, &buf);
    thread_yield();
    EXPECT_FALSE(rq_test_exited);

    int j = 0;
    int total = LEN(str) * 10;
    while(total > 0)
    {
        char b[LEN(str)/4 + 11];
        static_assert(LEN(str) % LEN(b) != 0, "asdf");
        int len = LEN(b);
        if (len > total)
            len = total;
        LOG_DEBUG("read");
        auto ret = buf.read(b, len);
        EXPECT_EQ(ret, len);
        LOG_DEBUG("read ", VALUE(ret));
        total -= len;

        for (int i=0; i < len; ++i, j = (j+1) % LEN(str))
            EXPECT_EQ(b[i], str[j]);
    }
    LOG_DEBUG("exit");
}

template<typename T>
void do_randops(IdentityPool0<T>* pool)
{
    int GET_TIMES = 64;
    pool->put(nullptr);
    vector<T *> list;
    for (int i = 0; i < GET_TIMES; i++){
        auto x = pool->get();
        LOG_DEBUG("get: `", *x);
        list.push_back(x);
    }
    for (auto x : list)
        pool->put(x);
}

/*
TEST(identity_pool, example)
{
    __example_of_identity_pool__();
    IdentityPool<int, 32> pool;
    auto x = pool.get();
    LOG_DEBUG("pool.get(): `", VALUE(*x));
    pool.put(x);
    x = pool.get();
    LOG_DEBUG("pool.get(): `", VALUE(*x));
}
*/

TEST(identity_pool, do_ops)
{
    int asdf = 323;
    auto _ctor = [&](int **ptr)-> int {
        *ptr = new int(asdf++);
        return 0;
    };
    auto _dtor = [&](int *ptr)-> int {
       delete ptr;
       return 0;
    };

    IdentityPool<int, 32> pool(_ctor, _dtor);
    do_randops(&pool);
    auto base0 = new_identity_pool<int>(32, _ctor, _dtor);
    DEFER(delete_identity_pool(base0));
    do_randops(base0);
    auto base1 = new_identity_pool<int>(32);
    DEFER(delete_identity_pool(base1));
    do_randops(base1);
}

TEST(Callback, virtual_function)
{
    const int RET = -1430789;
    class AA
    {
    public:
        double aa[10];
        virtual int foo(int x) { return RET + x; }
        int bar(int x) { return  RET - x; }
    };

    static void* THIS;
    class BB
    {
    public:
        int bb[10];
        BB() { bb[3] = RET; }
        virtual int foo2(int x)
        {
            EXPECT_EQ(THIS, this);
            return bb[3] + x*2;
        }
    };

    class CC : public AA, public BB
    {
    };

    auto lambda = [&](int x)
    {
        return RET + x/2;
    };

    AA a;
    CC c;
    Callback<int> ca(&a, &AA::foo);
    Callback<int> cb(&a, &AA::bar);
    Callback<int> cc(&c, &CC::foo2);
    Callback<int> dd(lambda);
//    Callback<int> ee([&](int x){ return RET + x/2; });

    THIS = (BB*)&c;
    LOG_DEBUG(VALUE(THIS), VALUE(&c));

    for (int i=0; i<100; ++i)
    {
        int x = rand();
        EXPECT_EQ(ca(x), RET + x);
        EXPECT_EQ(cb(x), RET - x);
        EXPECT_EQ(cc(x), RET + x*2);
        EXPECT_EQ(dd(x), RET + x/2);
    }
}

TEST(iovector_view, test1)
{
    const int N = 99;
    iovec iovs[N];
    iovector_view va(iovs), vb(iovs, N), vc;
    EXPECT_EQ(va, vb);
    EXPECT_EQ(va.elements_count(), N);
    EXPECT_FALSE(va.empty());
    EXPECT_TRUE(vc.empty());
    EXPECT_EQ(va.begin(), &iovs[0]);
    EXPECT_EQ(&va.front(), &iovs[0]);
    EXPECT_EQ(va.end(), &iovs[N]);
    EXPECT_EQ(&va.back(), &iovs[N-1]);

    va.pop_front();
    EXPECT_EQ(va.begin(), &iovs[1]);
    EXPECT_EQ(va.end(), &iovs[N]);

    vb.pop_back();
    EXPECT_EQ(vb.begin(), &iovs[0]);
    EXPECT_EQ(vb.end(), &iovs[N-1]);

    EXPECT_EQ(&va[3], &vb[4]);
    EXPECT_EQ(vc.sum(), 0);
}

iovec mk_iovec(uint64_t addr, size_t len)
{
    return iovec{(void*)addr, len};
}

TEST(iovector_view, test2)
{
    static iovec _iovs[] = {{(void*)0, 10}, {(void*)1, 20}, {(void*)2, 30}, {(void*)3, 40},
                            {(void*)4, 50}, {(void*)5, 60}, {(void*)6, 70}, {(void*)7, 80}, };
    {
        iovec iovs[LEN(_iovs)];
        memcpy(iovs, _iovs, sizeof(iovs));
        iovector_view va(iovs);
        EXPECT_EQ(va.sum(), 360);

        auto ret = va.shrink_to(400);
        EXPECT_EQ(ret, 360);
        EXPECT_EQ(va.sum(), 360);
        va.extract_back(80);
        EXPECT_EQ(va.back(), mk_iovec(6, 70));

        ret = va.shrink_to(200);
        EXPECT_EQ(ret, 200);
        EXPECT_EQ(va.sum(), 200);
        EXPECT_EQ(va.iov, iovs);
        EXPECT_EQ(va.iovcnt, 6);
        EXPECT_EQ(va[5], mk_iovec(5, 50));

        EXPECT_EQ(va.extract_front(0), 0);
        va.extract_front(5);
        EXPECT_EQ(va.iov, iovs);
        EXPECT_EQ(va.iovcnt, 6);
        EXPECT_EQ(va.front(), mk_iovec(5, 5));

        va.extract_front(5);
        EXPECT_EQ(va.iov, iovs + 1);
        EXPECT_EQ(va.iovcnt, 5);
        EXPECT_EQ(va.front(), mk_iovec(1, 20));

        va.extract_front(22);
        EXPECT_EQ(va.iov, iovs + 2);
        EXPECT_EQ(va.iovcnt, 4);
        EXPECT_EQ(va.front(), mk_iovec(4, 28));

        va.extract_back(66);
        EXPECT_EQ(va.iov, iovs + 2);
        EXPECT_EQ(va.iovcnt, 3);
        EXPECT_EQ(va.front(), mk_iovec(4, 28));
        EXPECT_EQ(va.back(), mk_iovec(4, 34));

        ret = va.shrink_to(0);
        EXPECT_EQ(ret, 0);
    }
    {
        iovec iovs[LEN(_iovs)], iovs2[10];
        memcpy(iovs, _iovs, sizeof(iovs));
        iovector_view va(iovs), vb(iovs2);
        EXPECT_EQ(va.sum(), 360);

        auto ret = va.extract_front(45, &vb);
        EXPECT_EQ(ret, 45);
        EXPECT_EQ(va.iovcnt, 6);
        EXPECT_EQ(&va.front(), &iovs[2]);
        EXPECT_EQ(va.front(), mk_iovec(17, 15));
        EXPECT_EQ(vb.iovcnt, 3);
        EXPECT_EQ(vb[0], iovs[0]);
        EXPECT_EQ(vb[1], iovs[1]);
        EXPECT_EQ(vb[2], mk_iovec(2, 15));

        vb.iovcnt = 1;
        ret = va.extract_front(20, &vb);
        EXPECT_EQ(ret, -1);
        EXPECT_EQ(va.iovcnt, 5);
        EXPECT_EQ(va.front(), mk_iovec(3, 40));
        EXPECT_EQ(vb.iovcnt, 1);
        EXPECT_EQ(vb.front(), mk_iovec(17, 15));

        vb.assign(iovs2);
        ret = va.extract_back(189, &vb);
        EXPECT_EQ(ret, 189);
        EXPECT_EQ(va.iovcnt, 3);
        EXPECT_EQ(va.back(), mk_iovec(5, 21));
        EXPECT_EQ(vb.iovcnt, 3);
        EXPECT_EQ(vb[0], mk_iovec(26, 39));
        EXPECT_EQ(vb[1], iovs[6]);
        EXPECT_EQ(vb[2], iovs[7]);

        vb.iovcnt = 1;
        ret = va.extract_back(30, &vb);
        EXPECT_EQ(ret, -1);
        EXPECT_EQ(va.iovcnt, 2);
        EXPECT_EQ(va.back(), mk_iovec(4, 50));
        EXPECT_EQ(vb.iovcnt, 1);
        EXPECT_EQ(vb.back(), mk_iovec(5, 21));
    }
}

bool do_memcmp(void* ptr)
{
    return true;
}

template<typename...Ts>
bool do_memcmp(void* ptr_, size_t n, char c, Ts...xs)
{
    auto ptr = (char*)ptr_;
    for (size_t i = 0; i < n; ++i)
        if (ptr[i] != c)
            return false;
    return do_memcmp(ptr + n, xs...);
}

#define DEFINE_BUF(x, N)      \
    char buf##x[N];           \
    memset(buf##x, #x[0], N);

TEST(iovector_view, test3)
{
    DEFINE_BUF(a, 23);
    DEFINE_BUF(b, 36);
    DEFINE_BUF(c, 84);
    DEFINE_BUF(d, 63);
    DEFINE_BUF(e, 37);
    DEFINE_BUF(f, 56);
    iovec _iovs[] = {
        {bufa, LEN(bufa)}, {bufb, LEN(bufb)}, {bufc, LEN(bufc)},
        {bufd, LEN(bufd)}, {bufe, LEN(bufe)}, {buff, LEN(buff)}};

    {
        char buf[1000];
        iovec iovs[LEN(_iovs)];
        memcpy(iovs, _iovs, sizeof(iovs));
        iovector_view va(iovs);
        auto ret = va.extract_front(11, buf);
        EXPECT_EQ(ret, 11);
        EXPECT_TRUE(do_memcmp(buf, 11, 'a'));

        ret = va.extract_front(20, buf);
        EXPECT_EQ(ret, 20);
        EXPECT_TRUE(do_memcmp(buf, 12, 'a', 8, 'b'));

        ret = va.extract_front(120, buf);
        EXPECT_EQ(ret, 120);
        EXPECT_TRUE(do_memcmp(buf, 28, 'b', 84, 'c', 8, 'd'));

        ret = va.extract_front(200, buf);
        EXPECT_EQ(ret, 148);
        EXPECT_TRUE(do_memcmp(buf, 55, 'd', 37, 'e', 56, 'f'));
    }
    {
        char buf[1000];
        iovec iovs[LEN(_iovs)];
        memcpy(iovs, _iovs, sizeof(iovs));
        iovector_view va(iovs);
        auto ret = va.extract_back(0, buf);
        EXPECT_EQ(ret, 0);

        ret = va.extract_back(11, buf);
        EXPECT_EQ(ret, 11);
        EXPECT_TRUE(do_memcmp(buf, 11, 'f'));

        ret = va.extract_back(50, buf);
        EXPECT_EQ(ret, 50);
        EXPECT_TRUE(do_memcmp(buf, 5, 'e', 45, 'f'));

        ret = va.extract_back(120, buf);
        EXPECT_EQ(ret, 120);
        EXPECT_TRUE(do_memcmp(buf, 25, 'c', 63, 'd', 32, 'e'));

        ret = va.extract_back(200, buf);
        auto len = 23 + 36 + 84 - 25;
        EXPECT_EQ(ret, len);
        EXPECT_TRUE(do_memcmp(buf + 200 - len, 23, 'a', 36, 'b', 84-25, 'c'));

    }
}

TEST(iovector_view, memcpy)
{
    log_output_level = 0;
    DEFINE_BUF(a, 23);
    DEFINE_BUF(b, 36);
    DEFINE_BUF(c, 84);
    DEFINE_BUF(d, 63);
    DEFINE_BUF(e, 37);
    DEFINE_BUF(f, 56);
    iovec _iovs[] = {
        {bufa, LEN(bufa)}, {bufb, LEN(bufb)}, {bufc, LEN(bufc)},
        {bufd, LEN(bufd)}, {bufe, LEN(bufe)}, {buff, LEN(buff)}};
    char buf[1024]{};
    iovec iovs[LEN(_iovs)];
    memcpy(iovs, _iovs, sizeof(iovs));
    iovector_view va(iovs);
    auto ret = va.memcpy_to(buf, 16);
    EXPECT_EQ(ret, 16);
    EXPECT_TRUE(do_memcmp(buf, 16, 'a'));
    ret = va.memcpy_to(buf, -1);
    EXPECT_EQ(ret, 299);
    memset(buf, 'y', sizeof(buf));
    ret = va.memcpy_from(buf, 0);
    EXPECT_EQ(ret, 0);
    ret = va.memcpy_from(buf, 23);
    EXPECT_EQ(ret, 23);
    ret = va.memcpy_from(buf, -1);
    EXPECT_EQ(ret, 299);
    memset(buf, 0, sizeof(buf));
    ret = va.extract_front(-1, buf);
    EXPECT_EQ(ret, ret);
    EXPECT_TRUE(do_memcmp(buf, ret, 'y'));
}

TEST(iovector, test1)
{
    IOVector iov;
    EXPECT_EQ(iov.sum(), 0);
    EXPECT_TRUE(iov.empty());
    EXPECT_EQ(iov.front_free_iovcnt(), IOVector::default_preserve);
    EXPECT_EQ(iov.back_free_iovcnt(), IOVector::capacity - IOVector::default_preserve);
    EXPECT_EQ(iov.begin(), iov.iovec());

    iovec v{nullptr, 33}, v2{nullptr, 44};
    iov.push_front(v);
    EXPECT_EQ(iov.front(), v);
    EXPECT_EQ(iov.back(), v);
    iov.push_front(55);
    EXPECT_EQ(iov.front().iov_len, 55);
    EXPECT_TRUE(iov.front().iov_base);
    EXPECT_EQ(iov.back(), v);
    iov.push_front(nullptr, 44);
    EXPECT_EQ(iov.front(), v2);
    EXPECT_EQ(iov.back(), v);
    EXPECT_EQ(iov.iovcnt(), 3);
    EXPECT_EQ(iov.front_free_iovcnt(), IOVector::default_preserve - 3);
    EXPECT_EQ(iov.back_free_iovcnt(), IOVector::capacity - IOVector::default_preserve);
    EXPECT_EQ(iov.sum(), 44+55+33);

    iov.push_back(77);
    EXPECT_EQ(iov.front(), v2);
    EXPECT_EQ(iov.back().iov_len, 77);
    EXPECT_TRUE(iov.back().iov_base);
    iov.push_back(nullptr, 44);
    EXPECT_EQ(iov.front(), v2);
    EXPECT_EQ(iov.back(), v2);
    iov.push_back(v);
    EXPECT_EQ(iov.front(), v2);
    EXPECT_EQ(iov.back(), v);
    EXPECT_EQ(iov.iovcnt(), 6);
    EXPECT_EQ(iov.front_free_iovcnt(), IOVector::default_preserve - 3);
    EXPECT_EQ(iov.back_free_iovcnt(), IOVector::capacity - IOVector::default_preserve - 3);
    EXPECT_EQ(iov.sum(), 44+55+33 + 77+44+33);

    EXPECT_EQ(iov.pop_front(), 44);
    EXPECT_EQ(iov.pop_back(), 33);
    EXPECT_EQ(iov.sum(), 55+33 + 77+44);

    auto s = iov.truncate(55+33 + 77+44 - 22);
    EXPECT_EQ(s, 55+33 + 77+44 - 22);
    EXPECT_EQ(iov.iovcnt(), 4);
    EXPECT_EQ(iov.back(), (iovec{nullptr, 22}));

    auto s2 = iov.truncate(55+33 + 77+44 - 50);
    EXPECT_EQ(s2, 55+33 + 77+44 - 50);
    EXPECT_EQ(iov.iovcnt(), 3);
    EXPECT_EQ(iov.back().iov_len, 71);

    IOVector iov1;
    iov1.push_back(234);
    iov1.push_back(456);
    iov1.push_back(789);
    IOVector iov2 = std::move(iov1);
    EXPECT_TRUE(iov1.empty());
    EXPECT_TRUE(iov1.nbases == 0);

}

TEST(iovector, test2)
{
    DEFINE_BUF(a, 23);
    DEFINE_BUF(b, 36);
    DEFINE_BUF(c, 84);
    DEFINE_BUF(d, 63);
    DEFINE_BUF(e, 37);
    DEFINE_BUF(f, 56);
    iovec _iovs[] = {
        {bufa, LEN(bufa)}, {bufb, LEN(bufb)}, {bufc, LEN(bufc)},
        {bufd, LEN(bufd)}, {bufe, LEN(bufe)}, {buff, LEN(buff)}};
    {
        IOVector iov(&_iovs[0], LEN(_iovs));
        EXPECT_EQ(iov.iovcnt(), LEN(_iovs));
        EXPECT_EQ(iov[0], _iovs[0]);
        EXPECT_EQ(iov[4], _iovs[4]);

        EXPECT_EQ(iov.extract_front(20), 20);
        EXPECT_EQ(iov[0], (iovec{bufa+20, 3}));
        EXPECT_EQ(iov.iovcnt(), LEN(_iovs));

        char buf[1000];
        EXPECT_EQ(iov.extract_front(20, buf), 20);
        EXPECT_EQ(iov.iovcnt(), LEN(_iovs) - 1);
        EXPECT_TRUE(do_memcmp(buf, 3, 'a', 17, 'b'));

        iovec bufv[10];
        iovector_view view(bufv);
        EXPECT_EQ(iov.extract_front(20, &view), 20);
        EXPECT_EQ(view.iovcnt, 2);
        EXPECT_EQ(view[0], (iovec{bufb + 17, 19}));
        EXPECT_EQ(view[1], (iovec{bufc, 1}));
        EXPECT_EQ(iov.iovcnt(), LEN(_iovs) - 2);
        EXPECT_EQ(iov[0], (iovec{bufc+1, LEN(bufc)-1}));

        int* t = iov.extract_front<int>();
        EXPECT_EQ(iov.iovcnt(), LEN(_iovs) - 2);
        EXPECT_EQ(iov[0], (iovec{bufc+5, LEN(bufc)-5}));
        EXPECT_TRUE(do_memcmp(t, 4, 'c'));

        auto ptr = iov.extract_front_continuous(90);
        EXPECT_EQ(iov.iovcnt(), LEN(_iovs) - 3);
        EXPECT_EQ(iov[0], (iovec{bufd+11, LEN(bufd)-11}));
        EXPECT_TRUE(ptr);
        EXPECT_TRUE(do_memcmp(ptr, 79, 'c', 11, 'd'));
    }
    {
        IOVector iov(&_iovs[0], LEN(_iovs));
        EXPECT_EQ(iov.extract_back(20), 20);
        EXPECT_EQ(iov.back(), (iovec{buff, LEN(buff)-20}));
        EXPECT_EQ(iov.iovcnt(), LEN(_iovs));

        iovec bufv[10];
        iovector_view view(bufv);
        EXPECT_EQ(iov.extract_back(40, &view), 40);
        EXPECT_EQ(view.iovcnt, 2);
        EXPECT_EQ(view[0], (iovec{bufe + LEN(bufe) - 4, 4}));
        EXPECT_EQ(view[1], (iovec{buff, 36}));
        EXPECT_EQ(iov.iovcnt(), LEN(_iovs) - 1);
        EXPECT_EQ(iov.back(), (iovec{bufe, LEN(bufe)-4}));

        double* t = iov.extract_back<double>();
        EXPECT_EQ(iov.iovcnt(), LEN(_iovs) - 1);
        EXPECT_EQ(iov.back(), (iovec{bufe, LEN(bufe)-4-8}));
        EXPECT_TRUE(do_memcmp(t, 8, 'e'));

        auto ptr = iov.extract_back_continuous(90);
        EXPECT_EQ(iov.iovcnt(), LEN(_iovs) - 3);
        EXPECT_EQ(iov.back(), (iovec{bufc, LEN(bufc)-2}));
        EXPECT_TRUE(ptr);
        EXPECT_TRUE(do_memcmp(ptr, 2, 'c', 63, 'd', 25, 'e'));
    }
}

TEST(iovector, push_more)
{

    DEFINE_BUF(a, 23);
    DEFINE_BUF(b, 36);
    DEFINE_BUF(c, 84);
    DEFINE_BUF(d, 63);
    DEFINE_BUF(e, 37);
    DEFINE_BUF(f, 56);
    iovec _iovs[] = {
        {bufa, LEN(bufa)}, {bufb, LEN(bufb)}, {bufc, LEN(bufc)},
        {bufd, LEN(bufd)}, {bufe, LEN(bufe)}, {buff, LEN(buff)}};
    {
        IOVector iov(&_iovs[0], LEN(_iovs));
        int total = 64;
        for (int i = 0; i < total; i++) {
            auto ret = iov.push_back_more(32);
            if (i >= 22) {
                EXPECT_EQ(ret, 0);
            } else {
                EXPECT_EQ(ret, 32);
            }
        }
    }
    {
        IOVector iov(&_iovs[0], LEN(_iovs));
        int total = 16;
        for (int i = 0; i < total; i++) {
            auto ret = iov.push_front_more(32);
            if (i >= 4) {
                EXPECT_EQ(ret, 0);
            } else {
                EXPECT_EQ(ret, 32);
            }
        }
    }

}

TEST(iovector, extract_iovector)
{
    IOVector iov;
    iov.push_back(64);
    iov.push_front(64);
    iov.push_back(256);
    iov.push_back(512);
    auto front = iov.front().iov_base;
    auto back = iov.back().iov_base;
    IOVector iov2;
    iov.extract_front(256, &iov2);
    EXPECT_EQ(640, iov.sum());
    EXPECT_EQ(3, iov2.iovcnt());
    EXPECT_EQ(256, iov2.sum());
    EXPECT_EQ(front, iov2.front().iov_base);
    IOVector iov3;
    iov.extract_back(256, &iov3);
    EXPECT_EQ(384, iov.sum());
    EXPECT_EQ(1, iov3.iovcnt());
    EXPECT_EQ(256, iov3.sum());
    EXPECT_EQ(((char*)back) + 256, iov3.back().iov_base);
}

TEST(iovector, slice)
{
    IOVector iov;
    iov.push_back(128);
    iov.push_back(256);
    iov.push_back(512);
    auto o = iov.view();
    {
    iovector_view view;
    auto ret = iov.slice(512, 100, &view);
    EXPECT_EQ(512, ret);
    EXPECT_EQ(o.iovcnt, view.iovcnt);
    EXPECT_EQ((char*)o.iov[0].iov_base + 100, view.iov[0].iov_base);
    EXPECT_EQ(28, view.iov[0].iov_len);
    EXPECT_EQ(o.iov[1].iov_base, view.iov[1].iov_base);
    EXPECT_EQ(256, view.iov[1].iov_len);
    EXPECT_EQ(o.iov[2].iov_base, view.iov[2].iov_base);
    EXPECT_EQ(228, view.iov[2].iov_len);
    }
    {
    iovector_view view;
    auto ret = iov.slice(28, 100, &view);
    EXPECT_EQ(28, ret);
    EXPECT_EQ(1, view.iovcnt);
    EXPECT_EQ((char*)o.iov[0].iov_base + 100, view.iov[0].iov_base);
    EXPECT_EQ(28, view.iov[0].iov_len);
    }
    {
    iovector_view view;
    auto ret = iov.slice(128, 0, &view);
    EXPECT_EQ(128, ret);
    EXPECT_EQ(1, view.iovcnt);
    EXPECT_EQ(o.iov[0].iov_base, view.iov[0].iov_base);
    EXPECT_EQ(128, view.iov[0].iov_len);
    }
    {
    iovector_view view;
    auto ret = iov.slice(1024, 0, &view);
    EXPECT_EQ(896, ret);
    EXPECT_EQ(3, view.iovcnt);
    EXPECT_EQ(o.iov[0].iov_base, view.iov[0].iov_base);
    EXPECT_EQ(128, view.iov[0].iov_len);
    EXPECT_EQ(o.iov[1].iov_base, view.iov[1].iov_base);
    EXPECT_EQ(256, view.iov[1].iov_len);
    EXPECT_EQ(o.iov[2].iov_base, view.iov[2].iov_base);
    EXPECT_EQ(512, view.iov[2].iov_len);
    }
}

TEST(iovector, memcpy)
{
    IOVector iov1, iov2;
    iov1.push_back(128);
    iov1.push_back(256);
    iov1.push_back(512);
    iov2.push_back(512);
    iov2.push_back(256);
    iov2.push_back(128);
    auto v1 = iov1.view();
    {
    IOVector tmp1(iov1.iovec(), iov1.iovcnt()), tmp2(iov2.iovec(), iov2.iovcnt());
    auto v2 = tmp2.view();
    auto ret = tmp1.memcpy_to(&v2);
    v2 = iov2.view();
    EXPECT_EQ(896, ret);
    EXPECT_EQ(0, memcmp(v1.iov[0].iov_base, v2.iov[0].iov_base, 128));
    EXPECT_EQ(0, memcmp(v1.iov[1].iov_base, (char*)v2.iov[0].iov_base + 128, 128));
    EXPECT_EQ(0, memcmp((char*)v1.iov[1].iov_base + 128, (char*)v2.iov[0].iov_base + 256, 128));
    EXPECT_EQ(0, memcmp(v1.iov[2].iov_base, (char*)v2.iov[0].iov_base + 384, 128));
    EXPECT_EQ(0, memcmp((char*)v1.iov[2].iov_base + 128, v2.iov[1].iov_base, 256));
    EXPECT_EQ(0, memcmp((char*)v1.iov[2].iov_base + 384, v2.iov[2].iov_base, 128));
    }
    {
    IOVector tmp1(iov1.iovec(), iov1.iovcnt()), tmp2(iov2.iovec(), iov2.iovcnt());
    auto v2 = tmp2.view();
    auto ret = tmp1.memcpy_from(&v2);
    v2 = iov2.view();
    EXPECT_EQ(896, ret);
    EXPECT_EQ(0, memcmp(v1.iov[0].iov_base, v2.iov[0].iov_base, 128));
    EXPECT_EQ(0, memcmp(v1.iov[1].iov_base, (char*)v2.iov[0].iov_base + 128, 128));
    EXPECT_EQ(0, memcmp((char*)v1.iov[1].iov_base + 128, (char*)v2.iov[0].iov_base + 256, 128));
    EXPECT_EQ(0, memcmp(v1.iov[2].iov_base, (char*)v2.iov[0].iov_base + 384, 128));
    EXPECT_EQ(0, memcmp((char*)v1.iov[2].iov_base + 128, v2.iov[1].iov_base, 256));
    EXPECT_EQ(0, memcmp((char*)v1.iov[2].iov_base + 384, v2.iov[2].iov_base, 128));
    }
}

// #ifdef GIT_VERSION
#define _STR(x) #x
#define STR(x) _STR(x)

static const string get_version() {
#if defined(GIT_VER)
    return STR(GIT_VER);
#else
    return "unknown version";
#endif
}

TEST(consistent_hash_map, test)
{
    consistent_hash_map<uint64_t, int> map;
    map.push_back(0, 1);
    map.push_back(100, 2);
    map.push_back(500, 3);
    map.sort();
    EXPECT_EQ(map.find(0)->second, 1);
    EXPECT_EQ(map.find(1)->second, 2);
    EXPECT_EQ(map.find(50)->second, 2);
    EXPECT_EQ(map.find(99)->second, 2);
    EXPECT_EQ(map.find(100)->second, 2);
    EXPECT_EQ(map.find(101)->second, 3);
    EXPECT_EQ(map.find(400)->second, 3);
    EXPECT_EQ(map.find(499)->second, 3);
    EXPECT_EQ(map.find(500)->second, 3);
    EXPECT_EQ(map.find(501)->second, 1);
    EXPECT_EQ(map.find(1001)->second, 1);
    EXPECT_EQ(map.find(1010)->second, 1);
    EXPECT_EQ(map.find(1010000)->second, 1);
}

TEST(consistent_hash_map, iter_find_back) {
    consistent_hash_map<uint64_t, int> map;
    map.push_back(100, 1);
    map.push_back(200, 2);
    map.sort();
    auto it = map.find(100);
    auto it2 = map.next(it);
    auto it3 = map.find(it2->first);
    EXPECT_NE(it->second, it3->second);
}

TEST(estring, test)
{
    estring s = "alskdjf,;;,q3r1234;poiu";
    LOG_DEBUG(s);
    charset cs(";,");
    auto sp = s.split(cs);
    auto it = sp.begin();
    auto front = *it;
    auto remainder = it.remainder();
    LOG_DEBUG(VALUE(front), VALUE(remainder));
    EXPECT_EQ(front, "alskdjf");
    EXPECT_EQ(remainder, "q3r1234;poiu");
    std::string sdf(*it);
    estring esdf = *it;
    EXPECT_EQ(sdf, esdf);

    vector<string> a; //(sp.begin(), sp.end());
    for (auto x: sp)
    {
        a.push_back(x);
        LOG_DEBUG(x);
    }

    // a.assign(sp.begin(), sp.end());

    EXPECT_EQ(a.size(), 3);
    EXPECT_EQ(a[0], front);
    EXPECT_EQ(a[1], "q3r1234");
    EXPECT_EQ(a[2], "poiu");

    auto sv = s;//.view();
    EXPECT_TRUE(sv.starts_with("alskdjf"));
    EXPECT_FALSE(sv.starts_with("alsk32"));
    EXPECT_TRUE(sv.ends_with("poiu"));
    EXPECT_FALSE(sv.ends_with("alsk32"));

    auto ps = estring::snprintf("%d%d%d", 2, 3, 4);
    EXPECT_EQ(ps, "234");

    estring as = "   \tasdf  \t\r\n";
    auto trimmed = as.trim();
    EXPECT_EQ(trimmed, "asdf");

    EXPECT_EQ(estring_view("234423").to_uint64(), 234423);
    EXPECT_EQ(estring_view("-234423").to_int64(), -234423);
    EXPECT_EQ(estring_view("asfdsf").to_uint64(32), 32);
    EXPECT_EQ(estring_view("-3.14").to_double(), -3.14);
    EXPECT_EQ(estring_view("1e10").to_double(), 1e10);

    EXPECT_EQ(estring_view("1").hex_to_uint64(), 0x1);
    EXPECT_EQ(estring_view("1a2b3d4e5f").hex_to_uint64(), 0x1a2b3d4e5f);
}

TEST(generator, example)
{
    ___example_of_generator____();
}

template <class T>
void basic_map_test(T &test_map) {

    test_map.clear();

    std::string prefix = "seggwrg90if908234j5rlkmx.c,bnmi7890wer1234rbdfb";
    for (int i = 0; i < 100000; i++) if (i % 2 == 0) {
        //std::string
        test_map.insert({prefix + std::to_string(i), i});
        ASSERT_EQ(test_map.size(), i/2+1);
    }
    for (int i = 100000; i < 200000; i++) if (i % 2 == 0){
        //string_view
        std::string x = prefix + std::to_string(i);
        // sprintf(xx, "%s%d", prefix.c_str(), i);
        // std::string_view k((prefix + std::to_string(i)).c_str());
        // std::string_view k(x);
        test_map.emplace(x, i);
        ASSERT_EQ(test_map.size(), i/2+1);
    }

    char *xname = new char [1000];

    auto p = test_map.begin();
    for (int i = 200000; i < 300000; i++) if (i % 2 == 0) {
        sprintf(xname, "%s%d", prefix.c_str(), i);
        // string_key k(prefix + to_string(i));
        test_map.insert(p, pair<string_view, int>(xname, i));
        ASSERT_EQ(test_map.size(), i/2+1);
    }

    for (int i = 300000; i < 400000; i++) if (i % 2 == 0) {
        sprintf(xname, "%s%d", prefix.c_str(), i);
        test_map[xname] = i;
        EXPECT_EQ(test_map.size(), i/2+1);
    }

    for (int i = 400000; i < 500000; i++) if (i % 2 == 0) {
        sprintf(xname, "%s%d", prefix.c_str(), i);
        // test_map.insert(pair<string_key, int>(string_key(std::move(string_key(xname))), i));
        test_map.insert(pair<string_view, int>(string_view(xname), i));
        EXPECT_EQ(test_map.size(), i/2+1);
    }

    for (int i = 500000; i < 600000; i++) if (i % 2 == 0) {
        // sprintf(xname, "%s%d", prefix.c_str(), i);
        // unordered_map_string_key<int>::value_type x = {string_key(prefix + std::to_string(i)), i};
        std::string s = prefix + std::to_string(i);
        const std::pair<const string_view, int> x = {s, i};
        test_map.insert(test_map.begin(), x);
        EXPECT_EQ(test_map.size(), i/2+1);
    }

    vector<pair<string, int> > vec;

    for (int i = 600000; i < 700000; i++) if (i % 2 == 0) {
        std::string s = prefix + std::to_string(i);
        std::pair<string, int> x = make_pair(s, i);//{s, i};
        vec.emplace_back(x);
    }
    test_map.insert(vec.begin(), vec.end());

    for (int i = 700000; i < 800000; i++) if (i % 2 == 0) {
        std::string x = prefix + std::to_string(i);
        std::string_view k(x);
        // string_key kk(k);
        auto v = pair<string_view, int>(k, i);
        test_map.insert(p, v);
        ASSERT_EQ(test_map.size(), i/2+1);
    }


    test_map.insert({ {"ppp7000002", 7000002}, {"ppp7000004", 7000004}, {"ppp7000006", 7000006} });
    test_map.emplace("ppp7000004", 7000004);
    EXPECT_EQ(test_map.find("ppp7000002")->second, 7000002);

    const T &const_map = test_map;

    memcpy(xname, prefix.c_str(), prefix.size());
    for (int i = 0; i < 800000; i++) {
        if (i % 2 == 0) {
            EXPECT_EQ(test_map.find(prefix + std::to_string(i))->second, i);
            memcpy(xname + prefix.size(), std::to_string(i).c_str(), std::to_string(i).size());
            xname[prefix.size() + std::to_string(i).size()] = 0;
            EXPECT_EQ(test_map.find(xname)->second, i);
            EXPECT_EQ(test_map[xname], i);
            EXPECT_EQ(test_map.at(xname), i);

            EXPECT_EQ(const_map.find(xname)->second, i);
            EXPECT_EQ(const_map.at(xname), i);

            // string_key sk(xname);
            // string_key &sk1 = sk;
            string_view sv(xname);
            // EXPECT_EQ(test_map.at(sk), i);
            EXPECT_EQ(test_map.count(sv), 1);

            auto rg = test_map.equal_range(xname);
            EXPECT_EQ(test_map.at(sv), rg.first->second);

            auto rg1 = const_map.equal_range(xname);
            EXPECT_EQ(const_map.at(sv), rg1.first->second);

        } else {
            EXPECT_EQ(test_map.find(prefix + std::to_string(i)), test_map.end());

            memcpy(xname + prefix.size(), std::to_string(i).c_str(), std::to_string(i).size());
            xname[prefix.size() + std::to_string(i).size()] = 0;

            EXPECT_EQ(test_map.find(xname), test_map.end());
            EXPECT_EQ(test_map.count(xname), 0);

            EXPECT_EQ(const_map.find(xname), const_map.end());
            EXPECT_EQ(const_map.count(xname), 0);

            // auto rg = test_map.equal_range(xname);
            // EXPECT_EQ(rg.first, rg.second);

            // EXPECT_EQ(test_map.at(xname), i);
            // EXPECT_EQ(test_map[xname], i);

        }
    }

    test_map.clear();
    // string_key y("asdf");
    // string_key x = &y;

    // test_map.insert({y, -1});
    test_map["1"] = 1;
    EXPECT_EQ(test_map.count("asdf"), 0);
    EXPECT_EQ(test_map.count("1"), 1);
    // std::map<string, string> xmap;
    // xmap.emplace_hint(xmap.begin(), "a", "b");

    delete []xname;
}


TEST(unordered_map_string_key, test) {
    unordered_map_string_key<int> test_map;
    test_map.reserve(6);
    basic_map_test(test_map);
    LOG_DEBUG("buckets `", test_map.bucket_count());
    test_map.rehash(5000000);
    EXPECT_GE(test_map.bucket_count(), 5000000);
    LOG_DEBUG("buckets `", test_map.bucket_count());
}

TEST(simple_unordered_map_string_key, test) {
    unordered_map_string_key<int> test_map;
    test_map.reserve(6);

    string prefix = "asfegrgr";

    for (int i = 0; i < 10000000; i++) if (i % 2 == 0) {
        //std::string
        std::string s = std::to_string(i);
        // string_view view(s);
        char chars[1000];
        sprintf(chars, "%d", i);
        // std::pair<const std::string_view, int> pr = make_pair(string_view(s), i);
        // unordered_map_string_key<int>::value_type x = make_pair(s, i);
        const std::pair<string, int> x = make_pair(s, i);//{s, i};
        test_map.insert(x);
        ASSERT_EQ(test_map.size(), i/2+1);
        test_map.insert({chars, i});
        ASSERT_EQ(test_map.size(), i/2+1);
    }
    for (int i = 0; i < 10000000; i++) if (i % 2 == 0) {
        //std::string
        std::string s = std::to_string(i);
        char chars[1000];
        sprintf(chars, "%d", i);
        // string_key k(s);
        // string_view view(s);
        string_view sv(chars);
        ASSERT_EQ(test_map.find(chars)->second, i);
        ASSERT_EQ(test_map.find(s)->second, i);
        ASSERT_EQ(test_map.find(sv)->second, i);
        // ASSERT_EQ(test_map.size(), i/2+1);
    }

    // unordered_map<int, int> x;
    // x.insert(1, 2);

    LOG_DEBUG("buckets `", test_map.bucket_count());
    test_map.rehash(10000000);
    EXPECT_GE(test_map.bucket_count(), 10000000);
    LOG_DEBUG("buckets `", test_map.bucket_count());

    // std::vector<string_key> vec;
    // for (int i = 0; i < 1000000; i++) {
    //     std::string s = std::to_string(i);
    //     vec.emplace_back(string_key(s));
    // };

    // string_key key("1000");
}

TEST(map_string_key, test) {
    map_string_key<std::unique_ptr<int> > test_uptr_map;
    std::unique_ptr<int> test_ptr(new int(10));
    test_uptr_map.emplace("sss", std::move(test_ptr));

    map_string_key<int> test_map;
    basic_map_test(test_map);

    std::string prefix = "seggwrg90if908234j5rlkmx.c,bnmi7890wer1234rbdfb";
    EXPECT_EQ(test_map.lower_bound(prefix + "2"), test_map.find(prefix + "2"));
    EXPECT_EQ(test_map.lower_bound(prefix + "1"), test_map.find(prefix + "2"));
    EXPECT_EQ(test_map.upper_bound(prefix + "22"), test_map.find(prefix + "23"));

    EXPECT_EQ(test_map.lower_bound(prefix + "9000000"), test_map.end());
    EXPECT_EQ(test_map.upper_bound(prefix + "9000000"), test_map.end());

    const map_string_key<int> &const_map = test_map;
    EXPECT_EQ(const_map.lower_bound(prefix + "2"), test_map.find(prefix + "2"));
    EXPECT_EQ(const_map.lower_bound(prefix + "1"), test_map.find(prefix + "2"));
    EXPECT_EQ(const_map.upper_bound(prefix + "22"), test_map.find(prefix + "23"));

    EXPECT_EQ(const_map.lower_bound(prefix + "9000000"), test_map.end());
    EXPECT_EQ(const_map.upper_bound(prefix + "9000000"), test_map.end());
}

TEST(RangeLock, Basic) {
  RangeLock m;

  //  test overlay segment
  m.lock(0, 4096);
  bool alreadUnLock = false;
  struct Data {
    RangeLock* m;
    bool* alreadyUnLock;
  };

  Data data{&m, &alreadUnLock};
  auto lambda = [] (void* para) -> uint64_t {
    auto data = static_cast<Data*>(para);
    data->m->unlock(0, 4096);
    *data->alreadyUnLock = true;
    return 0;
  };
  photon::Timer timer(1000, {&data, lambda});

  m.lock(0, 8192);
  EXPECT_TRUE(alreadUnLock);
  m.unlock(0, 8192);

  //  test independent segment
  m.lock(4096, 8192);
  m.lock(12288, 4096);
  m.unlock(4096, 8192);
  m.unlock(12288, 4096);
}

TEST(PooledAllocator, allocFailed) {
    PooledAllocator<> pool;
    auto alloc = pool.get_io_alloc();
    auto p1 = alloc.alloc(1024 * 1024);
    EXPECT_NE(nullptr, p1);
    alloc.dealloc(p1);
    auto p2 = alloc.alloc(1024 * 1024 + 1);
    EXPECT_EQ(nullptr, p2);
}

TEST(update_now, after_idle_sleep) {
    thread_yield();  // update now
    auto before = photon::now;
    LOG_DEBUG(VALUE(before));
    // photon::now will be update before entering sleep
    photon::thread_sleep(1);
    // and should be update after sleep;
    auto after_ = photon::now;
    LOG_DEBUG(VALUE(after_));
    LOG_DEBUG(VALUE(after_ - before));
    EXPECT_GT(after_, before);
}

// #endif
int main(int argc, char **argv)
{
    photon::vcpu_init();
    DEFER(photon::vcpu_fini());
    char a[100]{}, b[100]{};
    memset(a, 1, sizeof(a));
    memcpy(b, a, sizeof(a));
    do_memcmp(b, sizeof(b), 1);
    // #ifdef GIT_VERSION
    auto version = get_version();
    cout<<"git HEAD: "<<version.c_str() << endl;
#ifndef __clang__
    cout<<"gnu_get_libc_version() = "<< gnu_get_libc_version() <<endl;
#endif
    // #endif
    ::testing::InitGoogleTest(&argc, argv);
    int ret = RUN_ALL_TESTS();
    LOG_ERROR_RETURN(0, ret, VALUE(ret));
}
