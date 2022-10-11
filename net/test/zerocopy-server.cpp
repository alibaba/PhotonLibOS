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

#include <string>
#include <unistd.h>
#include <cerrno>
#include <atomic>
#include <map>
#include <vector>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <gflags/gflags.h>

#include <photon/photon.h>
#include <photon/io/aio-wrapper.h>
#include <photon/io/signal.h>
#include <photon/thread/thread11.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/alog-functionptr.h>
#include <photon/common/utility.h>
#include <photon/common/callback.h>
#include <photon/rpc/rpc.h>
#include <photon/common/checksum/crc32c.h>

#include "zerocopy-common.h"

using namespace std;
using namespace photon;

DEFINE_int32(socket_type, 0, "0: tcp socket, 1: zerocopy socket, 2: iouring socket， 3: et socket");
DEFINE_string(dir_name, "zerocopy", "dir_name");

struct FileDescriptor {
    int fd;
    void* buf;
};

FileDescriptor* g_file_fds = nullptr;
bool g_stop_test = false;

void write_checksum_worker(int index) {
    string file_name = FLAGS_dir_name + to_string(index);
    FileDescriptor& fileDesc = g_file_fds[index];

    // Fill buffer with random bytes
    int fd = open("/dev/urandom", O_RDONLY);
    ssize_t n_read = read(fd, fileDesc.buf, FLAGS_buf_size);
    if (n_read != (ssize_t) FLAGS_buf_size) {
        LOG_FATAL("Unable to read /dev/urandom, `", ERRNO());
        exit(-1);
    }
    DEFER(close(fd));

    // Calculate checksum and save it into padding
    char* checksum_buf = (char*) fileDesc.buf + fLU64::FLAGS_buf_size;
    uint32_t crc32_sum = crc32c_extend(fileDesc.buf, FLAGS_buf_size, 0);
    memcpy(checksum_buf, &crc32_sum, sizeof(crc32_sum));

    // Write file
    ssize_t total_size = FLAGS_buf_size + checksum_padding_size;
    ssize_t n_written = write(fileDesc.fd, fileDesc.buf, total_size);
    if (n_written != total_size) {
        LOG_FATAL("write ` failed: n_written `, error `", file_name.c_str(), n_written, strerror(errno));
        exit(-1);
    }
}

void prepare_read_files() {
    system((std::string("mkdir -p ") + fLS::FLAGS_dir_name).c_str());
    for (size_t i = 0; i < FLAGS_num_threads; i++) {
        // Open file on disk
        string file_name = FLAGS_dir_name + "/" + to_string(i);
        int fd = open(file_name.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) {
            LOG_FATAL("open ` failed", file_name.c_str());
            exit(-1);
        }
        ftruncate(fd, FLAGS_buf_size + checksum_padding_size);

        // posix_memalign allocate memory
        void* buf = nullptr;
        int ret = posix_memalign(&buf, 4096, FLAGS_buf_size + checksum_padding_size);
        if (ret != 0) {
            LOG_FATAL("posix_memalign failed: error `", ERRNO());
            exit(-1);
        }

        // Save
        auto& file_fd = g_file_fds[i];
        file_fd.fd = fd;
        file_fd.buf = buf;
    }
}

class TestRPCServer : public Object {
public:
    explicit TestRPCServer(IOAlloc alloc) {
        m_skeleton = rpc::new_skeleton();
        m_skeleton->set_allocator(alloc);
        m_skeleton->register_service<TestReadProto>(this);
        m_skeleton->register_service<TestWriteProto>(this);
        m_qps = 0;
        m_statis_thread = photon::thread_create11(&TestRPCServer::loop_show_statis, this);
        photon::thread_enable_join(m_statis_thread);
    }

    ~TestRPCServer() override {
        photon::thread_interrupt(m_statis_thread);
        photon::thread_join((photon::join_handle*) m_statis_thread);
        delete m_skeleton;
    }

    int serve(net::ISocketStream* socket) {
        int ret = m_skeleton->serve(socket, false);
        return ret;
    }

    int shutdown() {
        LOG_INFO("shutdown rpc server");
        return m_skeleton->shutdown();
    }

    int do_rpc_service(TestReadProto::Request* request,
                       TestReadProto::Response* response, IOVector* iov, IStream* stream) {
        string file_name = FLAGS_dir_name + to_string(request->file_index);
        FileDescriptor& fileDesc = g_file_fds[request->file_index];
        size_t size = FLAGS_calculate_checksum ? FLAGS_buf_size + checksum_padding_size : FLAGS_buf_size;

        size_t n_pushed = iov->push_back(size);
        if (n_pushed != size) {
            LOG_ERROR("iov push back error");
            exit(1);
        }

        ssize_t n_read = photon::libaio_preadv(fileDesc.fd, iov->iovec(), iov->iovcnt(), 0);
        if (n_read != (ssize_t) size) {
            LOG_FATAL("read ` failed: n_read `, error `", file_name.c_str(), n_read, strerror(errno));
            exit(-1);
        }

        response->buf.assign(iov->iovec(), iov->iovcnt());
        m_qps++;
        return 0;
    }

    int do_rpc_service(TestWriteProto::Request* request,
                       TestWriteProto::Response* response, IOVector* iov, IStream* stream) {
        response->code = 0;
        m_qps++;
        return 0;
    }

private:
    void loop_show_statis() {
        while (true) {
            int ret = photon::thread_sleep(10);
            if (ret != 0) {
                break;
            }
            LOG_INFO("Statis: QPS = `", m_qps / 10);
            m_qps = 0;
        }
    }

    rpc::Skeleton* m_skeleton;
    uint64_t m_qps;
    photon::thread* m_statis_thread;
};

void handle_signal(int) {
    LOG_INFO("try to stop test");
    g_stop_test = true;
}

int main(int argc, char** argv) {
    set_log_output_level(ALOG_INFO);
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    if (photon::init(INIT_EVENT_IOURING | INIT_EVENT_SIGNAL,
                     INIT_IO_LIBAIO | INIT_IO_SOCKET_EDGE_TRIGGER) < 0)
        return -1;
    DEFER(photon::fini());

    auto pooled_allocator = new PooledAllocator<>;
    DEFER(delete pooled_allocator);

    g_file_fds = new FileDescriptor[FLAGS_num_threads];
    DEFER(delete[] g_file_fds);
    prepare_read_files();         // 共 num_threads 个 read 文件

    if (FLAGS_calculate_checksum) {
        LOG_INFO("start to write checksum");
        for (size_t i = 0; i < FLAGS_num_threads; i++) {
            photon::thread_create11(write_checksum_worker, i);
        }
    }

    photon::sync_signal(SIGTERM, &handle_signal);
    photon::sync_signal(SIGINT, &handle_signal);

    net::ISocketServer* socket_srv = nullptr;
    if (SocketType(FLAGS_socket_type) == SocketType::TCP) {
        socket_srv = net::new_tcp_socket_server();
        LOG_INFO("New tcp socket server");
    } else if (SocketType(FLAGS_socket_type) == SocketType::ZEROCOPY) {
        socket_srv = net::new_zerocopy_tcp_server();
        LOG_INFO("New zerocopy socket server");
    } else if (SocketType(FLAGS_socket_type) == SocketType::IOURING) {
        socket_srv = net::new_iouring_tcp_server();
        LOG_INFO("New iouring socket server");
    } else if (SocketType(FLAGS_socket_type) == SocketType::ET) {
        socket_srv = net::new_et_tcp_socket_server();
        LOG_INFO("New et socket server");
    }
    DEFER(delete socket_srv);

    auto rpc_server = new TestRPCServer(pooled_allocator->get_io_alloc());
    DEFER(delete rpc_server);

    socket_srv->set_handler({rpc_server, &TestRPCServer::serve});
    socket_srv->bind((uint16_t) FLAGS_port, net::IPAddr("0.0.0.0"));
    socket_srv->listen(1024);

    auto stop_watcher = [&] {
        while (!g_stop_test) {
            photon::thread_sleep(1);
        }
        rpc_server->shutdown();
        socket_srv->terminate();
    };
    photon::thread_create11(stop_watcher);
    LOG_INFO("Socket server running ...");

    socket_srv->start_loop(true);
    LOG_INFO("Out of sleep");
}
