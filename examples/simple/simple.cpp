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
#include <photon/common/alog.h>
#include <photon/common/iovector.h>
#include <photon/thread/std-compat.h>
#include <photon/fs/localfs.h>
#include <photon/net/socket.h>

// In this example, we will demonstrate a simple example using various functional modules,
// i.e., `common`, `thread`, `fs`, `io` and `net`. The program basically sets up some Photon threads
// in the background, creates a Photon file and fs for IO, and sent buffer through Photon socket.
//
// Because every module has its own document, this example will not focus on the API details.
// Please refer to the README under the module directories.

static void run_socket_server(photon::net::ISocketServer* server, photon::fs::IFile* file,
                              photon_std::condition_variable& cv, photon_std::mutex& mu, bool& got_msg);

int main() {
    // Initialize Photon environment in current vCPU.
    //
    // Please note that in Photon world, vCPU == OS thread, thread == coroutine/fiber.
    // If not specified, the documents will keep using this naming convention.
    //
    // The default event engine will first try io_uring, then choose epoll if io_uring failed.
    // Running an io_uring program would need the kernel version to be greater than 5.8.
    // We encourage you to upgrade to the latest kernel so that you could enjoy the extraordinary performance.
    int ret = photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    if (ret < 0) {
        LOG_ERROR_RETURN(0, -1, "failed to init photon environment");
    }

    // DEFER is a helper macro from `common` module. Like Go's defer, it ensures the statement be executed
    // before the function returns. Its implementation is based on the concept of RAII.
    DEFER(photon::fini());

    // Create a local IFileSystem under current working dir.
    // When enabling io_uring, please switch the io_engine_type from `photon::fs::ioengine_psync` to
    // `photon::fs::ioengine_iouring`.
    auto fs = photon::fs::new_localfs_adaptor(".", photon::fs::ioengine_psync);
    if (!fs) {
        LOG_ERRNO_RETURN(0, -1, "failed to create fs");
    }
    DEFER(delete fs);

    // Open a IFile from IFileSystem. The IFile object will close itself at destruction. RAII.
    auto file = fs->open("simple-test-file", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!file) {
        LOG_ERRNO_RETURN(0, -1, "failed to open file");
    }
    DEFER(delete file);

    auto server = photon::net::new_tcp_socket_server();
    if (server == nullptr) {
        LOG_ERRNO_RETURN(0, -1, "failed to create tcp server");
    }
    DEFER(delete server);

    // Photon's std is equivalent to the standard std, but specially working for coroutines
    photon_std::mutex mu;
    photon_std::condition_variable cv;
    bool got_msg = false;

    // Create a thread to run socket server in the background.
    // Pass some local variables to the new thread as arguments.
    auto server_thread = new photon_std::thread(run_socket_server, server, file, cv, mu, got_msg);
    DEFER(delete server_thread);

    // Create a watcher thread. Inside, it is a typical C++ condition_variable usage.
    auto watcher_thread = new photon_std::thread([&] {
        LOG_INFO("Start to watch message");
        photon_std::unique_lock<photon_std::mutex> lock(mu);
        while (!got_msg) {
            cv.wait(lock);
        }
        LOG_INFO("Got message!");
    });
    DEFER(delete watcher_thread);

    // Wait server to be ready to accept
    photon_std::this_thread::sleep_for(std::chrono::seconds(1));

    // Create socket client and connect
    auto client = photon::net::new_tcp_socket_client();
    if (client == nullptr) {
        LOG_ERRNO_RETURN(0, -1, "failed to create tcp client");
    }
    DEFER(delete client);

    photon::net::EndPoint ep{photon::net::IPAddr("127.0.0.1"), 9527};
    auto stream = client->connect(ep);
    if (!stream) {
        LOG_ERRNO_RETURN(0, -1, "failed to connect server");
    }

    // Send data to socket
    char buf[1024];
    if (stream->send(buf, 1024) != 1024) {
        LOG_ERRNO_RETURN(0, -1, "failed to write socket");
    }

    // Close connection
    delete stream;

    // Wait for a while and shutdown the server
    photon_std::this_thread::sleep_for(std::chrono::seconds(1));
    server->terminate();

    // Join other threads
    watcher_thread->join();
    server_thread->join();
}

void run_socket_server(photon::net::ISocketServer* server, photon::fs::IFile* file,
                       photon_std::condition_variable& cv, photon_std::mutex& mu, bool& got_msg) {

    auto handler = [&](photon::net::ISocketStream* sock) -> int {
        // Receive data from socket.
        // read is a wrapper for fully recv.
        char buf[1024];
        ssize_t ret = sock->read(buf, 1024);
        if (ret <= 0) {
            LOG_ERRNO_RETURN(0, -1, "failed to read socket");
        }

        // IOVector is a helper class to manipulate io-vectors, taking the buf as underlying buffer
        IOVector iov;
        iov.push_back(buf, 512);
        iov.push_back((char*) buf + 512, 512);

        // This is a demo about how to use the io-vector interface. Even though some io engines
        // may not have the writev method, Photon's IFile encapsulation would make it compatible.
        // Note all the IOs in Photon are supposed to be non-blocking.
        ssize_t written = file->writev(iov.iovec(), iov.iovcnt());
        if (written != (ssize_t) iov.sum()) {
            LOG_ERRNO_RETURN(0, -1, "failed to write file");
        }

        // Got message. Notify the watcher
        // Again, typical C++ condition_variable usage
        {
            photon_std::lock_guard<photon_std::mutex> lock(mu);
            got_msg = true;
            cv.notify_one();
        }
        return 0;
    };

    server->set_handler(handler);
    server->bind(9527, photon::net::IPAddr());
    server->listen();

    // Photon's logging system formats the output string at COMPILE time, and has MUCH BETTER performance
    // than other systems using snprintf. The ` is a generic placeholder for any type.
    LOG_INFO("Server is listening for port ` ...", 9527);
    server->start_loop(true);
}
