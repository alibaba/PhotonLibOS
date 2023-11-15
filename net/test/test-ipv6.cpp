#include <vector>
#include <gtest/gtest.h>

#include <photon/photon.h>
#include <photon/thread/thread11.h>
#include <photon/net/socket.h>
#include <photon/net/utils.h>
#include <photon/common/alog.h>

TEST(ipv6, endpoint) {
    auto c = photon::net::EndPoint("127.0.0.1");
    EXPECT_TRUE(c.undefined());
    c = photon::net::EndPoint("127.0.0.1:8888");
    EXPECT_FALSE(c.undefined());
    c = photon::net::EndPoint("[::1]:8888");
    EXPECT_FALSE(c.undefined());
}

TEST(ipv6, addr) {
    auto c = photon::net::IPAddr("::1");
    EXPECT_FALSE(c.undefined());
    c = photon::net::IPAddr("::1");
    EXPECT_FALSE(c.undefined());
    c = photon::net::IPAddr("fdbd:dc01:ff:312:9641:f71:10c4:2378");
    EXPECT_FALSE(c.undefined());
    c = photon::net::IPAddr("fdbd:dc01:ff:312:9641:f71::2378");
    EXPECT_FALSE(c.undefined());
    c = photon::net::IPAddr("fdbd:dc01:ff:312:9641::2378");
    EXPECT_FALSE(c.undefined());

    c = photon::net::IPAddr("zfdbd:dq01:8:165::158");
    EXPECT_TRUE(c.undefined());
    c = photon::net::IPAddr("fdbd::ff:312:9641:f71::2378");
    EXPECT_TRUE(c.undefined());
    c = photon::net::IPAddr("::1z");
    EXPECT_TRUE(c.undefined());
    c = photon::net::IPAddr("::ffffffff");
    EXPECT_TRUE(c.undefined());
    c = photon::net::IPAddr("1.256.3.4");
    EXPECT_TRUE(c.undefined());
    c = photon::net::IPAddr("1.2.3.zzzz");
    EXPECT_TRUE(c.undefined());

    EXPECT_TRUE(photon::net::IPAddr() == photon::net::IPAddr::V4Any());
    EXPECT_TRUE(photon::net::IPAddr("0.0.0.0") == photon::net::IPAddr::V4Any());
    EXPECT_TRUE(photon::net::IPAddr("127.0.0.1") == photon::net::IPAddr::V4Loopback());
    EXPECT_TRUE(photon::net::IPAddr("255.255.255.255") == photon::net::IPAddr::V4Broadcast());

    EXPECT_TRUE(photon::net::IPAddr("::") == photon::net::IPAddr::V6Any());
    EXPECT_TRUE(photon::net::IPAddr("::1") == photon::net::IPAddr::V6Loopback());
    EXPECT_TRUE(photon::net::IPAddr("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff") == photon::net::IPAddr::V6None());

    photon::net::IPAddr b("fe80::216:3eff:fe7c:39e0");
    EXPECT_TRUE(b.is_link_local());
}

TEST(ipv6, get_host_by_peer) {
    auto peer = photon::net::gethostbypeer(photon::net::IPAddr("2001:4860:4860::8888"));
    ASSERT_TRUE(!peer.undefined());
    ASSERT_TRUE(!peer.is_ipv4());
    LOG_INFO(peer);
}

TEST(ipv6, dns_lookup) {
    std::vector<photon::net::IPAddr> ret;
    int num = photon::net::gethostbyname("github.com", ret);
    ASSERT_GT(num, 0);
    ASSERT_EQ(num, ret.size());
    bool has_v6 = false;
    for (auto& each : ret) {
        LOG_INFO("github.com IP addr `", each);
        if (!each.is_ipv4()) {
            has_v6 = true;
            break;
        }
    }
    ASSERT_TRUE(has_v6);
}

class DualStackTest : public ::testing::Test {
public:
    void run() {
        auto server = photon::net::new_tcp_socket_server_ipv6();
        ASSERT_NE(nullptr, server);
        DEFER(delete server);
        int ret = server->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
        ASSERT_EQ(0, ret);

        ret = server->bind(9527, photon::net::IPAddr::V6Any());
        ASSERT_EQ(0, ret);
        ret = server->listen();
        ASSERT_EQ(0, ret);

        photon::thread_create11([&] {
            auto client = get_client();
            if (!client) abort();
            DEFER(delete client);

            photon::net::EndPoint ep(get_server_ip(), 9527);
            auto stream = client->connect(ep);
            if (!stream) abort();
            DEFER(delete stream);
            LOG_INFO("Server endpoint: ", ep);

            photon::net::EndPoint ep2;
            int ret = stream->getsockname(ep2);
            if (ret) abort();
            if(ep2.undefined()) abort();
            LOG_INFO("Client endpoint: ", ep2);
            if (ep2.port < 10000) abort();

            photon::net::EndPoint ep3;
            ret = stream->getpeername(ep3);
            if (ret) abort();
            if (ep3.undefined()) abort();
            if (ep != ep3) abort();
        });

        photon::net::EndPoint ep;
        auto stream = server->accept(&ep);
        ASSERT_NE(nullptr, stream);
        LOG_INFO("Client endpoint: ", ep);

        photon::net::EndPoint ep4;
        ret = stream->getsockname(ep4);
        ASSERT_EQ(0, ret);

        if (is_ipv6_client()) {
            ASSERT_TRUE(!ep4.is_ipv4());
        } else {
            ASSERT_TRUE(ep4.is_ipv4());
        }
        ASSERT_EQ(9527, ep4.port);

        // Wait client close
        photon::thread_sleep(2);
        DEFER(delete stream);
    }
protected:
    virtual photon::net::ISocketClient* get_client() = 0;
    virtual photon::net::IPAddr get_server_ip() = 0;
    virtual bool is_ipv6_client() = 0;
};

class V6ToV6Test : public DualStackTest {
protected:
    photon::net::ISocketClient* get_client() override {
        return photon::net::new_tcp_socket_client_ipv6();
    }
    photon::net::IPAddr get_server_ip() override {
        return photon::net::IPAddr::V6Loopback();
    }
    bool is_ipv6_client() override { return true; }
};

class V4ToV6Test : public DualStackTest {
protected:
    photon::net::ISocketClient* get_client() override {
        return photon::net::new_tcp_socket_client();
    }
    photon::net::IPAddr get_server_ip() override {
        return photon::net::IPAddr::V4Loopback();
    }
    bool is_ipv6_client() override { return false; }
};

TEST_F(V6ToV6Test, run) {
    run();
}

TEST_F(V4ToV6Test, run) {
    run();
}

int main(int argc, char** arg) {
    ::testing::InitGoogleTest(&argc, arg);
    if (photon::init() != 0)
        LOG_ERROR_RETURN(0, -1, "error init");
    DEFER(photon::fini());
    return RUN_ALL_TESTS();
}