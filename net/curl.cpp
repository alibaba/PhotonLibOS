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

#include <curl/curl.h>
#include <openssl/err.h>
#include <shared_mutex>
#include <vector>
#include <memory>

#include <photon/common/alog.h>
#include <photon/common/event-loop.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread11.h>
#include <photon/thread/timer.h>
#include <photon/common/timeout.h>
#include <photon/common/utility.h>
#include <photon/net/security-context/tls-stream.h>
#include "../io/reset_handle.h"

namespace photon {
namespace net {
static constexpr int poll_size = 16;

class cURLLoop;
struct CurlThCtx {
    photon::Timer* g_timer = nullptr;
    CURLM* g_libcurl_multi = nullptr;
    photon::CascadingEventEngine* g_poller = nullptr;
    cURLLoop* g_loop = nullptr;
};

struct CurlCallCtx {
    photon::thread* th = nullptr;
    bool canceled = false;
};

__thread CurlThCtx cctx;
static CURLcode global_initialized;
static int do_action(curl_socket_t fd, int event) {
    int running_handles;
    auto ret = curl_multi_socket_action(cctx.g_libcurl_multi, fd, event,
                                        &running_handles);
    if (ret != CURLM_OK)
        LOG_ERROR_RETURN(EIO, -1, "failed to curl_multi_socket_action(): ",
                         curl_multi_strerror(ret));
    int msgs_left;
    CURLMsg* msg;
    while ((msg = curl_multi_info_read(cctx.g_libcurl_multi, &msgs_left))) {
        if (msg->msg == CURLMSG_DONE) {
            auto fcurl = msg->easy_handle;
            photon::semaphore* sem;
            char* eff_url;
            curl_easy_getinfo(fcurl, CURLINFO_EFFECTIVE_URL, &eff_url);
            auto res = curl_easy_strerror(msg->data.result);
            LOG_DEBUG("DONE: ` => (`) ", eff_url, res);
            CURLcode ret = curl_easy_getinfo(fcurl, CURLINFO_PRIVATE, &sem);
            if (ret == CURLE_OK && sem != nullptr) {
                LOG_DEBUG(VALUE(fcurl), " FINISHED");
                sem->signal(1);
            }
        }
    }
    return 0;
}
int curl_perform(CURL* curl, uint64_t timeout) {
    Timeout tmo(timeout);
    photon::semaphore sem(0);
    CURLcode ret = curl_easy_setopt(curl, CURLOPT_PRIVATE, &sem);
    if (ret != CURLE_OK)
        LOG_ERROR_RETURN(ENXIO, ret, "failed to set libcurl private: ",
                         curl_easy_strerror(ret));
    // this will cause set timeout
    DEFER(curl_multi_remove_handle(cctx.g_libcurl_multi, curl));
    // perform start
    auto mret = curl_multi_add_handle(cctx.g_libcurl_multi, curl);
    if (mret != CURLM_OK) {
        LOG_ERROR_RETURN(EIO, mret, "failed to curl_multi_add_handle(): ",
                         curl_multi_strerror(mret));
    }
    int wait = sem.wait(1, tmo.timeout());
    curl_easy_setopt(curl, CURLOPT_PRIVATE, nullptr);
    if (wait == -1) {
        ERRNO err;
        if (err.no == ETIMEDOUT) {
            LOG_ERROR_RETURN(err.no, CURLE_OPERATION_TIMEOUTED, "curl timeout");
        } else {
            LOG_ERROR_RETURN(err.no, CURLM_INTERNAL_ERROR,
                            "failed to cvar.wait for event");
        }
    }
    LOG_DEBUG("FINISHED");
    return CURLM_OK;
}
static uint64_t on_timer(void* = nullptr) {
    photon::thread_create11(&do_action, CURL_SOCKET_TIMEOUT, 0);
    return 0;
}
/* CURLMOPT_TIMERFUNCTION */
static int timer_cb(CURLM*, long timeout_ms, void*) {
    if (timeout_ms >= 0 && cctx.g_timer) {
        cctx.g_timer->reset(timeout_ms * 1000UL);
    }
    return 0;
}

/* CURLMOPT_SOCKETFUNCTION */
static int sock_cb(CURL* curl, curl_socket_t fd, int event, void*, void*) {
    photon::thread* th;
    CURLcode ret = curl_easy_getinfo(curl, CURLINFO_PRIVATE, &th);
    if (ret != CURLE_OK)
        LOG_ERROR_RETURN(EINVAL, -1,
                         "failed to get CURLINFO_PRIVATE from CURL* ", curl);
    if (event & CURL_POLL_REMOVE) {
        cctx.g_poller->rm_interest(
            {.fd = fd,
             .interests = photon::EVENT_READ | photon::EVENT_WRITE,
             .data = nullptr});
        return 0;
    }
    if (fd != CURL_SOCKET_BAD && (event & (CURL_POLL_IN | CURL_POLL_OUT))) {
        if (event & CURL_POLL_IN) {
            cctx.g_poller->add_interest(
                {.fd = fd,
                 .interests = photon::EVENT_READ,
                 .data = (void*)(((uint64_t)fd << 2) | photon::EVENT_READ)});
        }
        if (event & CURL_POLL_OUT) {
            cctx.g_poller->add_interest(
                {.fd = fd,
                 .interests = photon::EVENT_WRITE,
                 .data = (void*)(((uint64_t)fd << 2) | photon::EVENT_WRITE),
                 });
        }
    }
    return 0;
}

class cURLLoop : public Object {
public:
    cURLLoop()
        : loop(new_event_loop({this, &cURLLoop::wait_fds},
                              {this, &cURLLoop::on_poll})) {}

    ~cURLLoop() override { delete loop; }

    void start() { loop->async_run(); }

    void stop() { loop->stop(); }

    photon::thread* loop_thread() { return loop->loop_thread(); }

protected:
    EventLoop* loop;
    int cnt;
    uint64_t cbs[poll_size];

    int wait_fds(EventLoop*) {
        cnt = cctx.g_poller->wait_for_events((void**)&cbs, poll_size);
        return cnt;
    }

    int on_poll(EventLoop*) {
        for (int i = 0; i < cnt; i++) {
            int fd = cbs[i] >> 2;
            int ev = cbs[i] & 0b11;
            if (fd != CURL_SOCKET_BAD && ev != 0)
                photon::thread_create11(&do_action, fd, ev);
        }
        return 0;
    }
};

// CAUTION: this feature is incomplete in curl
int libcurl_set_pipelining(long val) {
    return curl_multi_setopt(cctx.g_libcurl_multi, CURLMOPT_PIPELINING, val);
}

int libcurl_set_maxconnects(long val) {
#if LIBCURL_VERSION_MAJOR > 7 || LIBCURL_VERSION_MAJOR == 7 && LIBCURL_VERSION_MINOR >= 30
    return curl_multi_setopt(cctx.g_libcurl_multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, val);
#else
    errno = ENOSYS;
    return -1;
#endif
}

__attribute__((constructor)) void global_init() {
    global_initialized = curl_global_init(CURL_GLOBAL_ALL);
}

// Fuction defined in tls-stream.
void __OpenSSLGlobalInit();

// Since global cleanup will cleanup openssl
// it needs mutex_buf, which will be destructed before global fini
// Considering destructor always called just before process exit
// even do not cleanup, memories will be released after process exit

// __attribute__((destructor)) void global_fini() {
//     curl_global_cleanup();
// }

class CurlResetHandle : public ResetHandle {
     int reset() override {
        LOG_INFO("reset libcurl by reset handle");
        // interrupt g_loop by ETIMEDOUT to replace g_poller
        if (cctx.g_loop)
            thread_interrupt(cctx.g_loop->loop_thread(), ETIMEDOUT);
        return 0;
     }
};
static thread_local CurlResetHandle *reset_handler = nullptr;

int libcurl_init(long flags, long pipelining, long maxconn) {
    if (cctx.g_loop == nullptr) {
        __OpenSSLGlobalInit();
        cctx.g_poller = photon::new_default_cascading_engine();
        cctx.g_loop = new cURLLoop();
        cctx.g_loop->start();
        cctx.g_timer =
            new photon::Timer(-1UL, {nullptr, &on_timer}, true, 8UL * 1024 * 1024);
        if (!cctx.g_timer)
            LOG_ERROR_RETURN(EFAULT, -1, "failed to create photon timer");

        if (global_initialized != CURLE_OK)
            LOG_ERROR_RETURN(EIO, -1, "CURL global init error: ",
                            curl_easy_strerror(global_initialized));

        LOG_DEBUG("libcurl version ", curl_version());

        cctx.g_libcurl_multi = curl_multi_init();
        if (cctx.g_libcurl_multi == nullptr)
            LOG_ERROR_RETURN(EIO, -1, "failed to init libcurl-multi");

        curl_multi_setopt(cctx.g_libcurl_multi, CURLMOPT_SOCKETFUNCTION, sock_cb);
        curl_multi_setopt(cctx.g_libcurl_multi, CURLMOPT_TIMERFUNCTION, timer_cb);
        curl_multi_setopt(cctx.g_libcurl_multi, CURLMOPT_MAXCONNECTS, 0);
#if LIBCURL_VERSION_MAJOR > 7 || LIBCURL_VERSION_MAJOR == 7 && LIBCURL_VERSION_MINOR >= 30
        curl_multi_setopt(cctx.g_libcurl_multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, 0);
#endif

        libcurl_set_pipelining(pipelining);
        libcurl_set_maxconnects(maxconn);
        if (reset_handler == nullptr) {
            reset_handler = new CurlResetHandle();
        }
        LOG_INFO("libcurl initialized");
    }

    return 0;
}
void libcurl_fini() {
    delete cctx.g_timer;
    cctx.g_timer = nullptr;
    cctx.g_loop->stop();
    delete cctx.g_loop;
    cctx.g_loop = nullptr;
    delete cctx.g_poller;
    cctx.g_poller = nullptr;
    CURLMcode ret = curl_multi_cleanup(cctx.g_libcurl_multi);
    if (ret != CURLM_OK)
        LOG_ERROR("libcurl-multi cleanup error: ", curl_multi_strerror(ret));
    cctx.g_libcurl_multi = nullptr;
    safe_delete(reset_handler);
    LOG_INFO("libcurl finished");
}

std::string url_escape(const char* str) {
    auto s = curl_escape(str, 0);
    DEFER(curl_free(s));
    return std::string(s);
}

std::string url_unescape(const char* str) {
    auto s = curl_unescape(str, 0);
    DEFER(curl_free(s));
    return std::string(s);
}
}  // namespace net
}
