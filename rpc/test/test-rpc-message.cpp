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

#include <cmath>
#include <gtest/gtest.h>
#include <photon/photon.h>
#include <photon/rpc/rpc.h>
#include <photon/rpc/serialize.h>
#include <photon/common/utility.h>
#include <photon/common/alog.h>
#include <photon/net/socket.h>


struct map_value : photon::rpc::Message {
    map_value() = default;
    map_value(int a_, const char* b_, char c_) : a(a_), b(b_), c(c_) {}

    int a = 0;
    photon::rpc::string b;
    char c = 0;

    PROCESS_FIELDS(a, b, c);
};

static bool check_map_order(photon::rpc::sorted_map<photon::rpc::string, map_value>& map) {
    int max = 0;
    for (auto iter = map.begin(); iter != map.end(); ++iter) {
        auto& each_val = iter->second;
        if (each_val.a > max) {
            max = each_val.a;
            LOG_DEBUG(each_val.b.c_str());
        } else {
            LOG_ERROR("corrupted order");
            return false;
        }
    }
    return true;
}

struct user_defined_type : public photon::rpc::CheckedMessage<> {
    static const int f1;
    int f2;
    photon::rpc::string f3;

    struct nested_type : public photon::rpc::CheckedMessage<> {
        int f4_1;
        photon::rpc::string f4_2;

        PROCESS_FIELDS(f4_1, f4_2);
    };
    nested_type f4;

    photon::rpc::sorted_map<photon::rpc::string, map_value> f5;

    struct nested_type2 : public photon::rpc::Message {
        int f6_1 = 61;
        float f6_2 = 6.2;

        PROCESS_FIELDS(f6_1, f6_2)
    };
    photon::rpc::array<nested_type2> f6;

    PROCESS_FIELDS(f1, f2, f3, f4, f5, f6);
};

const int user_defined_type::f1 = 1;

struct TestOperation {
    const static uint32_t IID = 0x1;
    const static uint32_t FID = 0x2;

    struct Request : public photon::rpc::CheckedMessage<> {
        int32_t code = 999;
        photon::rpc::buffer buf;
        photon::rpc::sorted_map<photon::rpc::string, map_value> map;

        PROCESS_FIELDS(code, buf, map);
    };

    using Response = user_defined_type;
};

class TestRPCServer {
public:
    struct ServiceReturnValue {
        photon::rpc::sorted_map_factory<photon::rpc::string, map_value> factory;
        std::vector<TestOperation::Response::nested_type2> f6_list;
    };

    TestRPCServer() : skeleton(photon::rpc::new_skeleton()),
                      server(photon::net::new_tcp_socket_server()) {
        skeleton->register_service<TestOperation>(this);
    }

    ServiceReturnValue do_rpc_service(TestOperation::Request* req, TestOperation::Response* resp,
                                      IOVector* iov, IStream* stream) {
        assert(req->code == 999);
        for (size_t i = 0; i < req->buf.size(); ++i) {
            char* c = (char*) req->buf.addr() + i;
            assert(*c == 'x');
        }

        auto iter = req->map.find("2");
        assert(iter != req->map.end());
        auto k = iter->first;
        assert(k == "2");
        auto v = iter->second;
        assert(v.a == 2);
        assert(v.b == "val-2");
        assert(v.c == '2');

        iter = req->map.find("4");
        if (iter != req->map.end()) {
            LOG_ERROR("shouldn't exist");
            abort();
        }

        if (!check_map_order(req->map))
            abort();

        resp->f2 = 2;
        resp->f3 = "resp-3";
        resp->f4.f4_1 = 41;
        resp->f4.f4_2 = "resp-4-2";

        ServiceReturnValue ret;

        map_value v51(51, "val-5-1", '5');
        map_value v52(52, "val-5-2", '5');
        map_value v53(53, "val-5-3", '5');

        ret.factory.append("5-1", v51);
        ret.factory.append("5-3", v53);
        ret.factory.append("5-2", v52);
        ret.factory.assign_to(&resp->f5);

        for (int i = 0; i < 3; ++i) {
            ret.f6_list.emplace_back(TestOperation::Response::nested_type2());
        }
        resp->f6.assign(ret.f6_list);

        return ret;
    }

    int serve(photon::net::ISocketStream* stream) {
        return skeleton->serve(stream);
    }

    int run() {
        if (server->bind() < 0) LOG_ERRNO_RETURN(0, -1, "Failed to bind");
        if (server->listen() < 0) LOG_ERRNO_RETURN(0, -1, "Failed to listen");
        server->set_handler({this, &TestRPCServer::serve});
        return server->start_loop(false);
    }

    std::unique_ptr<photon::rpc::Skeleton> skeleton;
    std::unique_ptr<photon::net::ISocketServer> server;
};

TEST(rpc, message) {
    TestRPCServer server;
    ASSERT_EQ(0, server.run());

    auto pool = photon::rpc::new_stub_pool(-1, -1, -1);
    DEFER(delete pool);

    photon::net::EndPoint ep;
    ASSERT_EQ(0, server.server->getsockname(ep));
    ep.addr = photon::net::IPAddr("127.0.0.1");

    auto stub = pool->get_stub(ep, false);
    ASSERT_NE(nullptr, stub);
    DEFER(pool->put_stub(ep, true));

    TestOperation::Request req;
    char buf[4096];
    memset(buf, 'x', sizeof(buf));
    req.buf.assign(buf, sizeof(buf));

    photon::rpc::sorted_map_factory<photon::rpc::string, map_value> factory;
    map_value v2(2, "val-2", '2');
    factory.append("2", v2);
    map_value v1(1, "val-1", '1');
    factory.append("1", v1);
    map_value v3(3, "val-3", '3');
    factory.append("3", v3);
    factory.assign_to(&req.map);

    IOVector resp_iov;
    auto* resp = stub->call<TestOperation>(req, resp_iov);

    ASSERT_NE(nullptr, resp);
    ASSERT_EQ(1, resp->f1);
    ASSERT_EQ(2, resp->f2);
    ASSERT_EQ(0, strcmp(resp->f3.c_str(), "resp-3"));
    ASSERT_EQ(41, resp->f4.f4_1);
    ASSERT_TRUE(resp->f4.f4_2 == "resp-4-2");
    ASSERT_TRUE(check_map_order(resp->f5));

    ASSERT_EQ(61, resp->f6[0].f6_1);
    ASSERT_TRUE(fabs(resp->f6[2].f6_2 - float(6.2)) < std::numeric_limits<float>::epsilon());

    LOG_INFO("Test finished, shutdown server...");
    ASSERT_EQ(0, server.skeleton->shutdown());
}

struct VariableLengthMessage : public photon::rpc::Message {
    int a;
    photon::rpc::string b;
    PROCESS_FIELDS(a, b);
};

TEST(rpc, variable_length_serialization) {
    // A flat buffer to simulate network channel
    char channel[4096] = {};

    // Send.
    const char* send_string = "1";
    VariableLengthMessage m_send;
    m_send.a = 1;
    m_send.b.assign(send_string);

    photon::rpc::SerializerIOV s_send;
    s_send.serialize(m_send);
    LOG_DEBUG("send iov ->");
    s_send.iov.debug_print();
    size_t bytes = s_send.iov.memcpy_to(channel, sizeof(channel));
    ASSERT_EQ(bytes, s_send.iov.sum());

    // Receive. rpc::string has been assigned to a large buffer
    // Serialize first
    VariableLengthMessage m_recv;
    const size_t buf_size = strlen(send_string) + 4096;
    char buf[buf_size];
    m_recv.b = photon::rpc::string(buf, buf_size);
    photon::rpc::SerializerIOV s_recv;
    s_recv.serialize(m_recv);
    LOG_DEBUG("expected receive iov ->");
    s_recv.iov.debug_print();

    // Read from network channel, truncate to the actual sent bytes
    s_recv.iov.truncate(bytes);
    s_recv.iov.memcpy_from(channel, bytes);
    LOG_DEBUG("actual receive iov ->");
    s_recv.iov.debug_print();

    // Deserialize at last
    photon::rpc::DeserializerIOV s_des;
    auto* p = s_des.deserialize<VariableLengthMessage>(&s_recv.iov);
    memcpy(&m_recv, p, sizeof(VariableLengthMessage));

    ASSERT_EQ((void*) buf, m_recv.b.c_str());
    ASSERT_EQ(1, m_recv.a);
    ASSERT_EQ(0, memcmp(send_string, m_recv.b.c_str(), strlen(send_string)));
}

int main(int argc, char** arg) {
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
    ::testing::InitGoogleTest(&argc, arg);
    return RUN_ALL_TESTS();
}
