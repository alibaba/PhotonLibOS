#include <gtest/gtest.h>
#include <photon/common/alog.h>
#include <photon/common/estring.h>
#include <photon/net/http/server.h>
#include <photon/net/socket.h>
#include <photon/photon.h>

#include <photon/thread/thread.h>

#include <map>
#include <string>

#include "../oss.h"

using namespace photon::objstore;
using namespace photon::net;
using namespace photon::net::http;

static std::map<std::string, std::string> g_captured_headers;
static photon::mutex g_mutex;

static int capture_handler(void*, Request& req, Response& resp,
                           std::string_view) {
  {
    SCOPED_LOCK(g_mutex);
    g_captured_headers.clear();
    for (auto kv : req.headers) {
      g_captured_headers[std::string(kv.first)] = std::string(kv.second);
    }
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
  int sign(Headers& headers, const SignParameters& params) override {
    return 0;
  }
  void set_credentials(CredentialParameters&& credentials) override {}
};

class FakeAuthenticator : public Authenticator {
 public:
  int sign(Headers& headers, const SignParameters& params) override {
    headers.insert("Authorization", "OSS fake-ak:fake-sig");
    headers.insert("Date", "Tue, 10 Jun 2026 00:00:00 GMT");
    return 0;
  }
  void set_credentials(CredentialParameters&& credentials) override {}
};

class OssCustomHeaderTest : public ::testing::Test {
 protected:
  ISocketServer* tcp_server = nullptr;
  HTTPServer* http_server = nullptr;

  void SetUp() override {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    tcp_server = new_tcp_socket_server();
    tcp_server->timeout(1000ULL * 1000);
    tcp_server->bind_v4localhost();
    tcp_server->listen();
    http_server = new_http_server();
    http_server->add_handler({nullptr, &capture_handler});
    tcp_server->set_handler(http_server->get_connection_handler());
    tcp_server->start_loop();
  }

  void TearDown() override {
    delete http_server;
    delete tcp_server;
    photon::fini();
  }

  ClientOptions make_opts() {
    ClientOptions opts;
    opts.endpoint = "http://oss-test.example.com";
    opts.bucket = "test-bucket";
    opts.proxy = estring().appends("http://127.0.0.1:",
                                   tcp_server->getsockname().port);
    opts.retry_times = 0;
    return opts;
  }
};

TEST_F(OssCustomHeaderTest, custom_headers_are_sent) {
  auto opts = make_opts();
  opts.custom_headers = {{"x-custom-trace-id", "abc123"}};

  auto client = new_oss_client(opts, new NoopAuthenticator());
  ASSERT_NE(client, nullptr);
  DEFER(delete client);

  ObjectHeaderMeta meta;
  client->head_object("test-obj", meta);

  SCOPED_LOCK(g_mutex);
  ASSERT_NE(g_captured_headers.find("x-custom-trace-id"),
            g_captured_headers.end());
  EXPECT_EQ(g_captured_headers["x-custom-trace-id"], "abc123");
}

TEST_F(OssCustomHeaderTest, keys_are_lowercased) {
  auto opts = make_opts();
  opts.custom_headers = {{"X-CUSTOM-UPPER", "val"}};

  auto client = new_oss_client(opts, new NoopAuthenticator());
  ASSERT_NE(client, nullptr);
  DEFER(delete client);

  ObjectHeaderMeta meta;
  client->head_object("test-obj", meta);

  SCOPED_LOCK(g_mutex);
  EXPECT_EQ(g_captured_headers.count("X-CUSTOM-UPPER"), 0u);
  ASSERT_NE(g_captured_headers.find("x-custom-upper"),
            g_captured_headers.end());
  EXPECT_EQ(g_captured_headers["x-custom-upper"], "val");
}

TEST_F(OssCustomHeaderTest, sdk_headers_override_custom) {
  auto opts = make_opts();
  opts.custom_headers = {{"x-oss-range-behavior", "custom-value"}};

  auto client = new_oss_client(opts, new NoopAuthenticator());
  ASSERT_NE(client, nullptr);
  DEFER(delete client);

  char buf[1];
  iovec iov{buf, 1};
  client->get_object_range("test-obj", &iov, 1, 0);

  SCOPED_LOCK(g_mutex);
  auto it = g_captured_headers.find("x-oss-range-behavior");
  ASSERT_NE(it, g_captured_headers.end());
  EXPECT_EQ(it->second, "standard");
}

TEST_F(OssCustomHeaderTest, case_insensitive_insert_prevents_override) {
  auto opts = make_opts();
  opts.custom_headers = {{"X-OSS-RANGE-BEHAVIOR", "custom-value"},
                         {"authoriZation", "custom-auth"},
                         {"dAte", "custom-date"}};

  auto client = new_oss_client(opts, new FakeAuthenticator());
  ASSERT_NE(client, nullptr);
  DEFER(delete client);

  char buf[1];
  iovec iov{buf, 1};
  client->get_object_range("test-obj", &iov, 1, 0);

  SCOPED_LOCK(g_mutex);
  auto it = g_captured_headers.find("x-oss-range-behavior");
  ASSERT_NE(it, g_captured_headers.end());
  EXPECT_EQ(it->second, "standard");

  auto auth_it = g_captured_headers.find("Authorization");
  ASSERT_NE(auth_it, g_captured_headers.end());
  EXPECT_EQ(auth_it->second, "OSS fake-ak:fake-sig");

  auto date_it = g_captured_headers.find("Date");
  ASSERT_NE(date_it, g_captured_headers.end());
  EXPECT_EQ(date_it->second, "Tue, 10 Jun 2026 00:00:00 GMT");
}
