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

#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>

#include <gflags/gflags.h>

#include <photon/net/http/client.h>
#include <photon/net/curl.h>
#include <photon/net/socket.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread11.h>

using namespace photon;

DEFINE_string(ip, "127.0.0.1", "server ip");
DEFINE_int32(port, 19876, "port");
DEFINE_uint64(count, -1UL, "request count per thread, -1 for endless loop");
DEFINE_int32(threads, 4, "num threads");
DEFINE_int32(body_size, 4096, "http body size");
DEFINE_bool(curl, false, "use curl client rather than http client");

class StringStream {
    std::string s;
    public:
        int ptr = 0;
        StringStream() {
            s.resize(FLAGS_body_size);
        }
        std::string& str() {
            return s;
        }
        void clean() {
            ptr = 0;
        }
        size_t write(void* c, size_t n) {
            LOG_DEBUG("CALL WRITE");
            // s.append((char*)c, n);

            if (ptr + n > s.size()) LOG_ERROR_RETURN(0, 0, "buffer limit excceed");
            memcpy((char*)s.data() + ptr, c, n);
            ptr += n;
            return n;
        }
};

struct result {
    uint64_t t_begin = 0, t_end = 1;
    uint64_t sum_latency = 0, sum_throuput = 0, cnt = 0;
    bool failed = false;
    result operator +=(result &rhs) {
        sum_latency += rhs.sum_latency;
        sum_throuput += rhs.sum_throuput;
        cnt += rhs.cnt;
        failed |= rhs.failed;
        return *this;
    }
};

inline uint64_t GetSteadyTimeUs() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               now.time_since_epoch())
        .count() / 1000;
}
void curl_thread_entry(result* res) {
    net::cURL client;
    StringStream buffer;
    client.set_redirect(0);
    std::string target = "http://" + FLAGS_ip + ":" + std::to_string(FLAGS_port);
    for (uint64_t i = 0; i < FLAGS_count; i++) {
        auto t_begin = GetSteadyTimeUs();
        buffer.clean();
        client.GET(target.c_str(), &buffer);
        auto t_end = GetSteadyTimeUs();
        if (buffer.ptr != FLAGS_body_size) {
            LOG_ERROR(VALUE(buffer.ptr), VALUE(errno), VALUE(i), VALUE(FLAGS_body_size));
            res->failed = true;
        }
        res->sum_throuput += FLAGS_body_size;
        res->sum_latency += t_end - t_begin;
        res->cnt++;
    }
}
void test_curl(result &res) {
    res.t_begin = GetSteadyTimeUs();
    std::vector<photon::join_handle*> jhs;
    for (auto i = 0; i < FLAGS_threads; ++i) {
        jhs.emplace_back(photon::thread_enable_join(
            photon::thread_create11(&curl_thread_entry, &res)));
    }
    for (auto &h : jhs) {
        photon::thread_join(h);
    }
    res.t_end = GetSteadyTimeUs();
}
void client_thread_entry(result *res, net::http::Client *client, int idx) {
    std::string body_buf;
    body_buf.resize(FLAGS_body_size);
    std::string target = "http://" + FLAGS_ip + ":" + std::to_string(FLAGS_port);
    for (uint64_t i = 0; i < FLAGS_count; i++) {
        auto t_begin = GetSteadyTimeUs();
        net::http::Client::OperationOnStack<8 * 1024> operation(client, net::http::Verb::GET, target);
        auto op = &operation;
        client->call(op);
        if (op->resp.headers.content_length() != FLAGS_body_size) {
            LOG_ERROR(VALUE(op->resp.headers.content_length()), VALUE(errno), VALUE(i), VALUE(FLAGS_body_size));
            res->failed = true;
        }
        auto ret = op->resp.read((void*)body_buf.data(), FLAGS_body_size);
        if (ret != FLAGS_body_size) {
            LOG_ERROR(VALUE(ret), VALUE(errno), VALUE(i), VALUE(FLAGS_body_size));
            res->failed = true;
        }
        auto t_end = GetSteadyTimeUs();
        res->sum_throuput += FLAGS_body_size;
        res->sum_latency += t_end - t_begin;
        res->cnt++;
    }
}
void test_client(result &res) {
    res.t_begin = GetSteadyTimeUs();
    auto client = net::http::new_http_client();
    DEFER(delete client);
    std::vector<photon::join_handle*> jhs;
    for (auto i = 0; i < FLAGS_threads; ++i) {
        jhs.emplace_back(photon::thread_enable_join(
            photon::thread_create11(&client_thread_entry, &res, client, i)));
    }
    for (auto h : jhs) {
        photon::thread_join(h);
    }
    res.t_end = GetSteadyTimeUs();
}
int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    set_log_output_level(ALOG_INFO);
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
    int ret;
#ifdef __linux__
    ret = net::et_poller_init();
    if (ret < 0) return -1;
    DEFER(net::et_poller_fini());
#endif
    ret = net::cURL::init(CURL_GLOBAL_ALL, 0, 0);
    if (ret < 0) return -1;
    DEFER({ photon::thread_sleep(1); net::cURL::fini(); });

    result res_curl, res_client;
    if (FLAGS_curl) {
        test_curl(res_curl);
    } else {
        test_client(res_client);
    }
    if (res_curl.cnt != 0) LOG_INFO("libcurl latency = `us , throughput = `MB/s, failed = `, read_size = `, threads = `",
                res_curl.sum_latency / res_curl.cnt,
                res_curl.sum_throuput * 1000 * 1000 / (res_curl.t_end - res_curl.t_begin) / 1024 / 1024,
                res_curl.failed, FLAGS_body_size, FLAGS_threads);
    if (res_client.cnt != 0) LOG_INFO("http_client latency = `us , throughput = `MB/s, failed = `, read_size = `, threads = `",
                res_client.sum_latency / res_client.cnt,
                res_client.sum_throuput * 1000 * 1000 / (res_client.t_end - res_client.t_begin) / 1024 / 1024,
                res_client.failed, FLAGS_body_size, FLAGS_threads);
    return 0;
}
