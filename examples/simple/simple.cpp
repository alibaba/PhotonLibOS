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

#include <fcntl.h>
#include <vector>

#include <photon/photon.h>
#include <photon/thread/thread11.h>
#include <photon/common/alog.h>
#include <photon/common/iovector.h>
#include <photon/fs/localfs.h>
#include <photon/net/socket.h>

// In this example, we will demonstrate a simple example using various functional modules,
// namely `common`, `thread`, `fs`, `io` and `net`. The program basically sets up two Photon threads,
// creates a Photon file and fs for IO, and sent buffer through Photon socket.
//
// Because every module has its own document, this example will not focus on the API details.
// Please refer to the README under the module directories.

static void run_socket_server(photon::net::ISocketServer* server, photon::fs::IFile* file,
                              photon::condition_variable* cond);

int main() {
    // Initialize Photon environment. Choose the iouring event engine.
    // Note that Photon downloads and compiles liburing by default. Even though compiling it doesn't require
    // the latest kernel, running an io_uring program does need the kernel version be greater than 5.8.
    //
    // If you have trouble upgrading the kernel, please switch the event_engine argument
    // from `photon::INIT_EVENT_IOURING` to `photon::INIT_EVENT_EPOLL`.
    int ret = photon::init(photon::INIT_EVENT_IOURING, 0);
    if (ret < 0) {
        LOG_ERROR_RETURN(0, -1, "failed to init photon environment");
    }

    // DEFER is a helper macro from `common` module. Like Go's defer, it ensures the statement be executed
    // before the function returns. Its implementation is based on the concept of RAII.
    DEFER(photon::fini());

    // Create a local IFileSystem under current working dir.
    // If not enabling io_uring, please switch the io_engine_type from `photon::fs::ioengine_iouring` to
    // `photon::fs::ioengine_psync`.
    auto fs = photon::fs::new_localfs_adaptor(".", photon::fs::ioengine_iouring);
    if (!fs) {
        LOG_ERRNO_RETURN(0, -1, "failed to create fs");
    }
    DEFER(delete fs);

    // Open a IFile from IFileSystem. The IFile object will close itself at destruction.
    auto file = fs->open("simple-test-file", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!file) {
        LOG_ERRNO_RETURN(0, -1, "failed to open file");
    }
    DEFER(delete file);

    auto server = photon::net::new_tcp_socket_server();
    photon::condition_variable cond;

    // In the photon world, we just call coroutine thread. Photon threads run on top of native OS threads.
    // We create a Photon thread to run socket server. Pass some local variables to the new thread as arguments.
    auto server_thread = photon::thread_create11(run_socket_server, server, file, &cond);
    photon::thread_enable_join(server_thread);

    // Wait for server ready
    cond.wait_no_lock();

    // Create socket client and connect
    auto client = photon::net::new_tcp_socket_client();
    photon::net::EndPoint ep{photon::net::IPAddr("127.0.0.1"), 9527};
    auto conn = client->connect(ep);
    if (!conn) {
        LOG_ERRNO_RETURN(0, -1, "failed to connect server");
    }

    // Write socket
    char buf[1024];
    if (conn->send(buf, 1024) != 1024) {
        LOG_ERRNO_RETURN(0, -1, "failed to write socket");
    }

    // Close connection
    delete conn;

    // Sleep one second and shutdown server
    photon::thread_usleep(1000 * 1000);
    server->terminate();

    // Interrupt the sleeping server thread, and join it
    photon::thread_interrupt(server_thread);
    photon::thread_join((photon::join_handle*) server_thread);
}

void run_socket_server(photon::net::ISocketServer* server, photon::fs::IFile* file, photon::condition_variable* cond) {
    auto handler = [&](photon::net::ISocketStream* arg) -> int {
        char buf[1024];
        auto sock = (photon::net::ISocketStream*) arg;

        // read is a wrapper for fully recv
        ssize_t ret = sock->read(buf, 1024);
        if (ret <= 0) {
            LOG_ERRNO_RETURN(0, -1, "failed to read socket");
        }

        // IOVector is a helper class for manipulate io-vectors.
        IOVector iov;
        iov.push_back(buf, 512);
        iov.push_back(buf + 512, 512);

        // This is a demo about how to use the io-vector interface. Even though some io engines
        // may not have the writev method, Photon's IFile encapsulation would make it compatible.
        ssize_t written = file->writev(iov.iovec(), iov.iovcnt());
        if (written != (ssize_t) iov.sum()) {
            LOG_ERRNO_RETURN(0, -1, "failed to write file");
        }
        return 0;
    };

    server->set_handler(handler);
    server->bind(9527, photon::net::IPAddr());
    server->listen(1024);
    // Photon's logging system formats the output string at compile time, and has better performance
    // than other systems using snprintf. The ` is a generic placeholder.
    LOG_INFO("Server is listening for port ` ...", 9527);
    server->start_loop(false);

    // Notify outside main function to continue.
    cond->notify_one();

    photon::thread_sleep(-1);
    // This Photon thread will here sleep forever, until been interrupted by `photon::thread_interrupt`.
    // Note that the underlying OS thread won't be blocked. It's basically a context switch.
    LOG_INFO("Server stopped");
}