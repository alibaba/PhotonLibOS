#include <gtest/gtest.h>
#include <photon/common/alog.h>
#include <photon/common/estring.h>
#include <photon/net/http/server.h>
#include <photon/net/socket.h>
#include <photon/net/utils.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>

#include <string>

#include "../oss.h"

using namespace photon::objstore;
using namespace photon::net;
using namespace photon::net::http;

namespace {

// Set when a request actually reaches the proxy server.
bool g_server_reached = false;
photon::mutex g_mutex;

int capture_handler(void*, Request& req, Response& resp, std::string_view) {
  {
    SCOPED_LOCK(g_mutex);
    g_server_reached = true;
  }
  char body[] = "x";
  resp.set_result(200);
  resp.headers.content_length(1);
  resp.headers.insert("Content-Range", "bytes 0-0/1");
  resp.write(body, 1);
  return 0;
}

class NoopAuthenticator : public Authenticator {
 public:
  int sign(Headers&, const SignParameters&) override { return 0; }
  void set_credentials(CredentialParameters&&) override {}
};

class OssIPVersionTest : public ::testing::Test {
 protected:
  ISocketServer* tcp_server = nullptr;
  HTTPServer* http_server = nullptr;
  // Whether 127.0.0.1 resolves to IPv4 here; gates the family tests below (skip
  // via early return, as GTEST_SKIP is absent in some CI gtest versions). IPv6
  // is covered end-to-end by OssIPVersionV6Test.
  bool loopback_is_ipv4 = false;

  void SetUp() override {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    g_server_reached = false;
    tcp_server = new_tcp_socket_server();
    tcp_server->timeout(1000ULL * 1000);
    tcp_server->bind_v4localhost();
    tcp_server->listen();
    http_server = new_http_server();
    http_server->add_handler({nullptr, &capture_handler});
    tcp_server->set_handler(http_server->get_connection_handler());
    tcp_server->start_loop();

    // Own resolver/cache: learn the loopback family.
    auto* resolver = new_default_resolver();
    DEFER(delete resolver);
    loopback_is_ipv4 = resolver->resolve("127.0.0.1").is_ipv4();
  }

  void TearDown() override {
    delete http_server;
    delete tcp_server;
    photon::fini();
  }

  ClientOptions make_opts(IPVersion ver) {
    ClientOptions opts;
    opts.endpoint = "oss-test.example.com";
    opts.bucket = "test-bucket";
    opts.proxy = estring().appends("http://127.0.0.1:",
                                   tcp_server->getsockname().port);
    opts.retry_times = 0;
    opts.ip_version = ver;
    return opts;
  }

  // Returns the head_object result; also reports whether the server was hit.
  int probe(IPVersion ver, bool* reached) {
    auto opts = make_opts(ver);
    auto client = new_oss_client(opts, new NoopAuthenticator());
    EXPECT_NE(client, nullptr);
    if (!client) return -1;
    DEFER(delete client);
    ObjectHeaderMeta meta;
    int ret = client->head_object("test-obj", meta);
    SCOPED_LOCK(g_mutex);
    *reached = g_server_reached;
    return ret;
  }
};

TEST_F(OssIPVersionTest, both_accepts_any_family) {
  bool reached = false;
  int ret = probe(IPVersion::kBoth, &reached);
  // No filter installed: the loopback proxy is reachable.
  EXPECT_EQ(0, ret);
  EXPECT_TRUE(reached);
}

TEST_F(OssIPVersionTest, ipv4_only_accepts_ipv4_loopback) {
  if (!loopback_is_ipv4) {
    LOG_INFO("127.0.0.1 does not resolve to IPv4 here; skip");
    return;
  }
  bool reached = false;
  int ret = probe(IPVersion::kIPv4Only, &reached);
  // IPv4-only keeps the loopback address, so the request reaches the server.
  EXPECT_TRUE(reached);
  EXPECT_EQ(0, ret);
}

TEST_F(OssIPVersionTest, ipv6_only_rejects_ipv4_loopback) {
  if (!loopback_is_ipv4) {
    LOG_INFO("127.0.0.1 does not resolve to IPv4 here; skip");
    return;
  }
  bool reached = false;
  int ret = probe(IPVersion::kIPv6Only, &reached);
  // IPv6-only discards the IPv4 loopback, so the request fails.
  EXPECT_FALSE(reached);
  EXPECT_NE(0, ret);
}

// IPv6 side via the OSS client. The proxy listens on ::1 and is addressed as
// "localhost" (the URL parser rejects a literal [::1]); "localhost" resolves to
// both families, so ip_version picks which one is dialed. Each filter gets its
// own TEST_F (fresh photon session => fresh DNS cache) so the host-keyed cache
// never bypasses the filter.
class OssIPVersionV6Test : public ::testing::Test {
 protected:
  ISocketServer* tcp_server = nullptr;
  HTTPServer* http_server = nullptr;
  // True when the ::1 server is up and "localhost" resolves to an IPv6.
  bool v6_usable = false;

  void SetUp() override {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    g_server_reached = false;
    tcp_server = new_tcp_socket_server();
    tcp_server->timeout(1000ULL * 1000);
    if (tcp_server->bind_v6localhost() < 0 || tcp_server->listen() < 0) {
      LOG_INFO("IPv6 loopback unavailable here; v6 tests will skip");
      return;
    }
    http_server = new_http_server();
    http_server->add_handler({nullptr, &capture_handler});
    tcp_server->set_handler(http_server->get_connection_handler());
    tcp_server->start_loop();

    // Own resolver/cache: does "localhost" yield an IPv6?
    auto* resolver = new_default_resolver();
    DEFER(delete resolver);
    auto is_v6 = [](IPAddr a) { return a.is_ipv6(); };
    v6_usable = resolver->resolve_filter("localhost", is_v6).is_ipv6();
    if (!v6_usable)
      LOG_INFO("'localhost' has no IPv6 here; v6 tests will skip");
  }

  void TearDown() override {
    delete http_server;
    delete tcp_server;
    photon::fini();
  }

  int probe(IPVersion ver, bool* reached) {
    ClientOptions opts;
    opts.endpoint = "oss-test.example.com";
    opts.bucket = "test-bucket";
    opts.proxy = estring().appends("http://localhost:",
                                   tcp_server->getsockname().port);
    opts.retry_times = 0;
    opts.ip_version = ver;
    auto client = new_oss_client(opts, new NoopAuthenticator());
    EXPECT_NE(client, nullptr);
    if (!client) return -1;
    DEFER(delete client);
    ObjectHeaderMeta meta;
    int ret = client->head_object("test-obj", meta);
    SCOPED_LOCK(g_mutex);
    *reached = g_server_reached;
    return ret;
  }
};

TEST_F(OssIPVersionV6Test, ipv6_only_accepts_ipv6_loopback) {
  if (!v6_usable) return;
  bool reached = false;
  int ret = probe(IPVersion::kIPv6Only, &reached);
  // IPv6-only keeps the ::1 address, so the client reaches the v6 proxy.
  EXPECT_TRUE(reached);
  EXPECT_EQ(0, ret);
}

TEST_F(OssIPVersionV6Test, ipv4_only_rejects_ipv6_loopback) {
  if (!v6_usable) return;
  bool reached = false;
  int ret = probe(IPVersion::kIPv4Only, &reached);
  // IPv4-only drops ::1; the only listener is on ::1, so the request fails.
  EXPECT_FALSE(reached);
  EXPECT_NE(0, ret);
}

}  // namespace

int main(int argc, char** argv) {
  set_log_output_level(ALOG_INFO);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
