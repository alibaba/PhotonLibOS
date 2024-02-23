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

#include "../../rpc/rpc.cpp"
#include <memory>
#include <chrono>
#include <gtest/gtest.h>
#include <photon/thread/thread.h>
#include <photon/common/memory-stream/memory-stream.h>
#include <photon/common/utility.h>
#include <photon/common/alog-stdstring.h>
#include <photon/net/socket.h>
#include <photon/photon.h>
#include "../../test/ci-tools.h"

using namespace std;
using namespace photon;
using namespace rpc;

class RpcTest : public testing::Test {
public:
    void SetUp() override {
        GTEST_ASSERT_EQ(0, photon::init(ci_ev_engine, photon::INIT_IO_NONE));
    }
    void TearDown() override {
        photon::fini();
    }
};

std::string S = "1234567890";
struct Args
{
    int a = 0, b = 0, c = 0, d = 0;
    std::string s;
    void init()
    {
        a = b = c = d = 123;
        s = "1234567890";
    }
    void verify()
    {
        EXPECT_EQ(a, 123);
        EXPECT_EQ(b, 123);
        EXPECT_EQ(c, 123);
        EXPECT_EQ(d, 123);
        EXPECT_EQ(s, S);
    }
    uint64_t serialize(iovector& iov)
    {
        iov.clear();
        iov.push_back({&a, offsetof(Args, s)});
        iov.push_back({(void*)s.c_str(), s.length()});
        return 2;
    }
    void deserialize(iovector& iov)
    {
        iov.extract_front(offsetof(Args, s), &a);
        auto slen = iov.sum();
        s.resize(slen);
        iov.extract_front(slen, &s[0]);
    }
};

FunctionID FID(234);

rpc::Header rpc_server_read(IStream* s)
{
    rpc::Header header;
    s->read(&header, sizeof(header));
//    EXPECT_EQ(header.tag, 1);

    IOVector iov;
    iov.push_back(header.size);
    s->readv(iov.iovec(), iov.iovcnt());

    Args args;
    args.deserialize(iov);
    args.verify();

    return header;
}

char STR[] = "!@#$%^&*()_+";
void rpc_server_write(IStream* s, uint64_t tag)
{
    rpc::Header header;
    header.tag = tag;
    header.size = LEN(STR);

    IOVector iov;
    iov.push_back(&header, sizeof(header));
    iov.push_back(STR, LEN(STR));

    s->writev(iov.iovec(), iov.iovcnt());
}

void* rpc_server(void* args_)
{
    LOG_DEBUG("enter");
    auto s = (IStream*)args_;
    while (true)
    {
        auto header = rpc_server_read(s);
        rpc_server_write(s, header.tag);
        if (header.function == (uint64_t)-1) break;
    }
    LOG_DEBUG("exit");
    return nullptr;
}

int server_function(void* instance, iovector* request, rpc::Skeleton::ResponseSender sender, IStream*)
{
    EXPECT_EQ(instance, (void*)123);

    Args args;
    args.deserialize(*request);
    args.verify();

    IOVector iov;
    iov.push_back(STR, LEN(STR));
    sender(&iov);
    LOG_DEBUG("exit");
    return 0;
}

int server_exit_function(void* instance, iovector* request, rpc::Skeleton::ResponseSender sender, IStream*)
{
    IOVector iov;
    iov.push_back(STR, LEN(STR));
    sender(&iov);

    auto sk = (Skeleton*)instance;
    sk->shutdown_no_wait();

    LOG_DEBUG("exit");
    return 0;
}

bool skeleton_exited;
photon::condition_variable skeleton_exit;

void* rpc_skeleton(void* args)
{
    auto s = (IStream*)args;
    auto sk = new_skeleton();
    DEFER(delete sk);

    sk->add_function(FID, rpc::Skeleton::Function((void*)123, &server_function));
    sk->add_function(-1,  rpc::Skeleton::Function(sk, &server_exit_function));
    sk->serve(s);
    LOG_DEBUG("exit");
    skeleton_exit.notify_all();
    skeleton_exited = true;
    return nullptr;
}

void do_call(StubImpl& stub, uint64_t function)
{
    SerializerIOV req_iov, resp_iov;
    Args args;
    args.init();
    args.serialize(req_iov.iov);

    LOG_DEBUG("before call");
    stub.do_call(function, &req_iov.iov, &resp_iov.iov, -1);
    LOG_DEBUG("after call recvd: '`'", (char*)resp_iov.iov.back().iov_base);
    EXPECT_EQ(memcmp(STR, resp_iov.iov.back().iov_base, LEN(STR)), 0);
}

TEST_F(RpcTest, call)
{
    unique_ptr<DuplexMemoryStream> ds( new_duplex_memory_stream(16) );
    thread_create(&rpc_skeleton, ds->endpoint_a);
    StubImpl stub(ds->endpoint_b);
    do_call(stub, 234);
    do_call(stub, -1);
    if (!skeleton_exited)
        skeleton_exit.wait_no_lock();
}

uint64_t ncallers;
void* do_concurrent_call(void* arg)
{
    ncallers++;
    LOG_DEBUG("enter");
    auto stub = (StubImpl*)arg;
    for (int i = 0; i < 10; ++i)
        do_call(*stub, 234);
    LOG_DEBUG("exit");
    ncallers--;
    return nullptr;
}

void* do_concurrent_call_shut(void* arg)
{
    ncallers++;
    LOG_DEBUG("enter");
    auto stub = (StubImpl*)arg;
    for (int i = 0; i < 10; ++i)
        do_call(*stub, 234);
    LOG_DEBUG("exit");
    ncallers--;
    return nullptr;
}

TEST_F(RpcTest, concurrent)
{
//    log_output_level = 1;
    LOG_INFO("Creating 1,000 threads, each doing 1,000 RPC calls");
    // ds will be destruct just after function returned
    // but server will not
    // therefore, it will cause assert when destruction
    skeleton_exited = false;
    unique_ptr<DuplexMemoryStream> ds( new_duplex_memory_stream(16) );
    thread_create(&rpc_skeleton, ds->endpoint_a);

    LOG_DEBUG("asdf1");
    StubImpl stub(ds->endpoint_b);
    for (int i = 0; i < 10; ++i)
        thread_create(&do_concurrent_call, &stub);

    LOG_DEBUG("asdf2");
    do { thread_usleep(1);
    } while(ncallers > 0);
    LOG_DEBUG("asdf3");
    do_call(stub, -1);
    LOG_DEBUG("asdf4");
    LOG_DEBUG("FINISHED");
    ds->close();
    if (!skeleton_exited)
        skeleton_exit.wait_no_lock();
}

void do_call_timeout(StubImpl& stub, uint64_t function)
{
    SerializerIOV req_iov, resp_iov;
    Args args;
    args.init();
    args.serialize(req_iov.iov);

    LOG_DEBUG("before call");
    if (stub.do_call(function, &req_iov.iov, &resp_iov.iov, 1UL*1000*1000) >= 0) {
        LOG_DEBUG("after call recvd: '`'", (char*)resp_iov.iov.back().iov_base);
    }
}

void* do_concurrent_call_timeout(void* arg)
{
    ncallers++;
    LOG_DEBUG("enter");
    auto stub = (StubImpl*)arg;
    for (int i = 0; i < 10; ++i)
        do_call_timeout(*stub, 234);
    LOG_DEBUG("exit");
    ncallers--;
    return nullptr;
}

int server_function_timeout(void* instance, iovector* request, rpc::Skeleton::ResponseSender sender, IStream*)
{
    EXPECT_EQ(instance, (void*)123);
    Args args;
    args.deserialize(*request);
    args.verify();

    photon::thread_usleep(3UL*1000*1000);

    IOVector iov;
    iov.push_back(STR, LEN(STR));
    LOG_INFO("Before Send");
    sender(&iov);
    LOG_INFO("After Send");
    LOG_DEBUG("exit");
    return 0;
}

void* rpc_skeleton_timeout(void* args)
{
    auto s = (IStream*)args;
    auto sk = new_skeleton();
    DEFER(delete sk);
    sk->add_function(FID, rpc::Skeleton::Function((void*)123, &server_function_timeout));
    sk->add_function(-1,  rpc::Skeleton::Function(sk, &server_exit_function));
    sk->serve(s);
    LOG_DEBUG("exit");
    skeleton_exit.notify_all();
    skeleton_exited = true;
    return nullptr;
}

TEST_F(RpcTest, timeout) {
    LOG_INFO("Creating 1,000 threads, each doing 1,000 RPC calls");
    // ds will be destruct just after function returned
    // but server will not
    // therefore, it will cause assert when destruction
    unique_ptr<DuplexMemoryStream> ds( new_duplex_memory_stream(655360) );

    skeleton_exited = false;
    thread_create(&rpc_skeleton_timeout, ds->endpoint_a);

    LOG_DEBUG("asdf1");
    StubImpl stub(ds->endpoint_b);
    for (int i = 0; i < 10; ++i)
        thread_create(&do_concurrent_call_timeout, &stub);

    LOG_DEBUG("asdf2");
    do { thread_usleep(1);
    } while(ncallers > 0);
    LOG_DEBUG("asdf3");
    do_call_timeout(stub, -1);
    LOG_DEBUG("asdf4");
    LOG_DEBUG("FINISHED");
    ds->close();
    if (!skeleton_exited)
        skeleton_exit.wait_no_lock();
    log_output_level = 0;
}

class RpcServer {
public:
    RpcServer(Skeleton* skeleton, net::ISocketServer* socket) : m_socket(socket), m_skeleton(skeleton) {
        m_skeleton->register_service<Operation>(this);
        m_socket->set_handler({this, &RpcServer::serve});
    }
    struct Operation {
        const static uint32_t IID = 0x1;
        const static uint32_t FID = 0x2;
        struct Request : public photon::rpc::Message {
            int code = 0;
            PROCESS_FIELDS(code);
        };
        struct Response : public photon::rpc::Message {
            int code = 0;
            PROCESS_FIELDS(code);
        };
    };
    int do_rpc_service(Operation::Request* req, Operation::Response* resp, IOVector* iov, IStream* stream) {
        resp->code = req->code;
        return 0;
    }
    int serve(photon::net::ISocketStream* stream) {
        return m_skeleton->serve(stream);
    }
    int run() {
        m_socket->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
        if (m_socket->bind(9527, net::IPAddr::V6Any()) != 0)
            LOG_ERRNO_RETURN(0, -1, "bind failed");
        if (m_socket->listen() != 0)
            LOG_ERRNO_RETURN(0, -1, "listen failed");
        return m_socket->start_loop(false);
    }
    net::ISocketServer* m_socket;
    Skeleton* m_skeleton;
};

static int do_call_2(Stub* stub) {
    RpcServer::Operation::Request req;
    RpcServer::Operation::Response resp;
    return stub->call<RpcServer::Operation>(req, resp);
}

TEST_F(RpcTest, shutdown) {
    auto socket_server = photon::net::new_tcp_socket_server_ipv6();
    GTEST_ASSERT_NE(nullptr, socket_server);
    DEFER(delete socket_server);
    auto sk = photon::rpc::new_skeleton();
    GTEST_ASSERT_NE(nullptr, sk);
    DEFER(delete sk);

    RpcServer rpc_server(sk, socket_server);
    GTEST_ASSERT_EQ(0, rpc_server.run());

    auto pool = photon::rpc::new_stub_pool(-1, -1, -1);
    DEFER(delete pool);

    photon::net::EndPoint ep(net::IPAddr::V4Loopback(), 9527);
    auto stub = pool->get_stub(ep, false);
    ASSERT_NE(nullptr, stub);
    DEFER(pool->put_stub(ep, true));

    photon::thread_create11([&]{
        photon::thread_sleep(1);
        sk->shutdown();
        delete sk;
        sk = nullptr;
    });

    auto start = std::chrono::steady_clock::now();
    while (true) {
        int ret = do_call_2(stub);
        if (ret < 0) {
            GTEST_ASSERT_EQ(ECONNRESET, errno);
            break;
        }
    }
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    GTEST_ASSERT_GT(duration, 900);
    GTEST_ASSERT_LT(duration, 1100);
}

TEST_F(RpcTest, passive_shutdown) {
    auto socket_server = photon::net::new_tcp_socket_server_ipv6();
    GTEST_ASSERT_NE(nullptr, socket_server);
    DEFER(delete socket_server);
    auto sk = photon::rpc::new_skeleton();
    GTEST_ASSERT_NE(nullptr, sk);
    DEFER(delete sk);

    RpcServer rpc_server(sk, socket_server);
    GTEST_ASSERT_EQ(0, rpc_server.run());

    photon::net::EndPoint ep(net::IPAddr::V4Loopback(), 9527);

    photon::thread_create11([&]{
        // Should always succeed in 3 seconds
        auto pool = photon::rpc::new_stub_pool(-1, -1, -1);
        DEFER(delete pool);
        auto stub = pool->get_stub(ep, false);
        if (!stub) abort();
        DEFER(pool->put_stub(ep, true));
        for (int i = 0 ; i < 30; ++i) {
            int ret = do_call_2(stub);
            if (ret < 0) {
                LOG_ERROR(VALUE(ret));
                abort();
            }
            photon::thread_usleep(100'000);
        }
    });

    photon::thread_create11([&]{
        photon::thread_sleep(2);
        // Should get connection refused after 2 seconds. Because socket closed listen fd at 1 second.
        auto pool = photon::rpc::new_stub_pool(-1, -1, -1);
        DEFER(delete pool);
        auto stub = pool->get_stub(ep, false);
        if (stub) {
            LOG_ERROR("should not get stub");
            abort();
        }
        if (errno != ECONNREFUSED) {
            LOG_ERROR(ERRNO());
            abort();
        }
    });

    auto start = std::chrono::steady_clock::now();

    photon::thread_sleep(1);
    socket_server->terminate();
    delete socket_server;
    socket_server = nullptr;

    LOG_INFO("begin passive shutdown");
    sk->shutdown(false);
    LOG_INFO("end passive shutdown");
    delete sk;
    sk = nullptr;

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // The passive shutdown took 3 seconds, until client closed the connection
    GTEST_ASSERT_GT(duration, 2900);
    GTEST_ASSERT_LT(duration, 3200);
}

int main(int argc, char** arg)
{
    ci_parse_env();
    ::testing::InitGoogleTest(&argc, arg);
    return RUN_ALL_TESTS();
}
