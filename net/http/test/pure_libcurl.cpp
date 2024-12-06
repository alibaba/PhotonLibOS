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

#include <iostream>
#include <curl/curl.h>
#include <gflags/gflags.h>
#include <string>
#include <thread>
#include <vector>

using namespace std;
DEFINE_int32(threads, 1, "thread num");
DEFINE_int32(loop_cnt, 1, "thread num");
size_t writeFunction(void* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append((char*)ptr, size * nmemb);
    return size * nmemb;
}

inline uint64_t GetSteadyTimeUs() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               now.time_since_epoch())
        .count() / 1000;
}
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

vector<result> results;
void thread_entry(int idx) {
    auto &res = results[idx];
    auto curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 64);
    if (!curl) {
        cout << "curl_easy_init failed" << endl;
        return;
    }
    std::string response_string;
    std::string header_string;
    for (auto i = 0; i < FLAGS_loop_cnt; i++) {
        curl_easy_setopt(curl, CURLOPT_URL, "http://11.158.232.168:19876");
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 100L);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFunction);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_string);
        auto t_begin = GetSteadyTimeUs();
        curl_easy_perform(curl);
        auto t_end = GetSteadyTimeUs();
        // cout << header_string << endl;
        // cout << response_string << endl;
        if ((header_string.size() == 0) || (response_string.size() == 0))
            res.failed = true;
        else {
            res.cnt++;
            res.sum_throuput += response_string.size();
            res.sum_latency += t_end - t_begin;
        }
        response_string.resize(0);
    }
    curl_easy_cleanup(curl);
}


int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    vector<thread> ths;
    results.resize(FLAGS_threads);
    result final_res;
    final_res.t_begin = GetSteadyTimeUs();
    for (auto i = 0 ; i < FLAGS_threads; i++) {
        ths.emplace_back(std::thread(&thread_entry, i));
    }
    for (auto& jh : ths) {
        jh.join();
    }
    final_res.t_end = GetSteadyTimeUs();
    for (auto &res : results) final_res += res;
    auto time_used_us = (final_res.t_end - final_res.t_begin);
    cout << "failed : " << final_res.failed << endl;
    // cout << "total_data : " << final_res.sum_throuput / 1024 << "KB" << endl;
    // cout << "timeUsed : " << time_used_us << "us" << endl;
    cout << "threads : " << FLAGS_threads << endl;
    cout << "latency : " << final_res.sum_latency / final_res.cnt << "us" << endl
         << "throuphPut : " << final_res.sum_throuput * 1000 * 1000 / time_used_us / 1024 / 1024 << "MB/s" << endl;
    curl_global_cleanup();
    return 0;
}
