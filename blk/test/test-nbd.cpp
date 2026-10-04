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

#include "../blk.h"
#include "../nbd-proto.h"

#include "../../test/gtest.h"
#include "harness.h"

#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/fs/localfs.h>
#include <photon/net/socket.h>
#include <photon/thread/stack-allocator.h>   // the default stack allocator, wrapped to count
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>   // thread_create11 for the stress clients

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace photon {
namespace blk {

static inline uint16_t be16rd(const void* p) { return __builtin_bswap16(*(const uint16_t*)p); }
static inline uint32_t be32rd(const void* p) { return __builtin_bswap32(*(const uint32_t*)p); }
static inline uint64_t be64rd(const void* p) { return __builtin_bswap64(*(const uint64_t*)p); }
static inline void be16wr(void* p, uint16_t v) { *(uint16_t*)p = __builtin_bswap16(v); }
static inline void be32wr(void* p, uint32_t v) { *(uint32_t*)p = __builtin_bswap32(v); }
static inline void be64wr(void* p, uint64_t v) { *(uint64_t*)p = __builtin_bswap64(v); }

// A minimal newstyle-fixed nbd client for protocol validation
struct NbdTestClient {
    net::ISocketStream* s = nullptr;
    uint16_t trans_flags = 0;
    uint64_t export_size = 0;
    uint64_t next_handle = 1;

    ~NbdTestClient() {
        if (s) {
            disc();
            delete s;
        }
    }

    void disconnect() {
        if (s) {
            disc();
            delete s;
            s = nullptr;
        }
    }

    int connect_tcp(const char* ip, uint16_t port) {
        auto c = net::new_tcp_socket_client();
        if (!c)
            return -1;
        DEFER(delete c);
        s = c->connect(net::EndPoint(ip, port));
        return s ? 0 : -1;
    }

    int connect_unix(const char* path) {
        auto c = net::new_uds_client();
        if (!c)
            return -1;
        DEFER(delete c);
        s = c->connect(path);
        return s ? 0 : -1;
    }

    // the greeting and the client flags, stopping short of the OPT_GO that
    // handshake() sends -- the cases that need a payload of their own start here
    int handshake_front() {
        char hdr[18];
        if (read_exact(hdr, sizeof(hdr)) < 0)
            return -1;
        if (be64rd(hdr) != NBD_INIT_MAGIC || be64rd(hdr + 8) != NBD_OPTS_MAGIC)
            return -1;
        char cf[4];
        be32wr(cf, NBD_FLAG_C_FIXED_NEWSTYLE | NBD_FLAG_C_NO_ZEROES);
        return write_exact(cf, sizeof(cf));
    }

    // A well-formed OPT_GO / OPT_INFO payload: a 32-bit export-name length, the
    // name, then a 16-bit count of requested information items. `items` only sets
    // the count -- a count with no items behind it is itself one of the malformed
    // payloads a case needs to be able to send.
    static std::vector<char> go_payload(const char* export_name, uint16_t items = 0) {
        size_t n = strlen(export_name);
        std::vector<char> d(4 + n + 2);
        be32wr(d.data(), (uint32_t)n);
        memcpy(d.data() + 4, export_name, n);
        be16wr(d.data() + 4 + n, items);
        return d;
    }

    // OPT_EXPORT_NAME's payload is the name and nothing else: no item count, and
    // its answer is the export meta rather than an option reply.
    static std::vector<char> name_payload(const char* export_name) {
        size_t n = strlen(export_name);
        std::vector<char> d(4 + n);
        be32wr(d.data(), (uint32_t)n);
        memcpy(d.data() + 4, export_name, n);
        return d;
    }

    // A payload whose name MATCHES and whose item count parses, with bytes left over
    // behind it. Every individual read in the walk succeeds and the name is the right
    // one, so the only thing that can refuse it is the requirement that the walk land
    // exactly on the option's own declared length. That is what makes this the payload
    // which witnesses that check: with a name that did not match, the name policy
    // would refuse it on its own and the length check could be deleted unnoticed.
    static std::vector<char> go_payload_with_trailer(const char* export_name,
                                                     size_t trailer) {
        std::vector<char> d = go_payload(export_name);
        d.insert(d.end(), trailer, '\0');
        return d;
    }

    int read_raw(void* buf, size_t n) { return read_exact(buf, n); }

    int send_raw_option(uint32_t opt, const void* data, uint32_t len) {
        return send_option(opt, data, len);
    }

    // One option reply: its type in *type_out and its payload in *data_out.
    // Returns -1 on a transport error, which is what a server that closed the
    // connection without replying looks like from here.
    int read_opt_reply(uint32_t* type_out, std::vector<char>* data_out) {
        char rh[20];
        if (read_exact(rh, sizeof(rh)) < 0)
            return -1;
        if (be64rd(rh) != NBD_REP_MAGIC)
            return -1;
        *type_out = be32rd(rh + 12);
        uint32_t len = be32rd(rh + 16);
        data_out->resize(len);
        if (len && read_exact(data_out->data(), len) < 0)
            return -1;
        return 0;
    }

    // A request header with no payload behind it: exactly what a client that
    // stalls mid-WRITE has sent, and what makes the server wait for bytes while
    // holding both of its device-wide gates.
    int send_header_only(uint16_t type, uint64_t offset, uint32_t len, uint16_t flags = 0) {
        char req[28];
        encode_req(req, type, flags, next_handle++, offset, len);
        return write_exact(req, sizeof(req));
    }

    // bound this client's own reads and writes, so that a server which never
    // answers fails the case instead of hanging it
    void set_timeout(uint64_t us) { s->timeout(us); }

    int handshake(const char* export_name = "") {
        if (handshake_front() < 0)
            return -1;

        // the name length is 32 bits on the wire. Encoding it as 16 was the same
        // wrong assumption the server made when it skipped the payload instead of
        // parsing it, so a 4-byte OPT_GO looked well formed to both sides.
        std::vector<char> data = go_payload(export_name);
        if (send_option(NBD_OPT_GO, data.data(), (uint32_t)data.size()) < 0)
            return -1;

        while (true) {
            char rh[20];
            if (read_exact(rh, sizeof(rh)) < 0)
                return -1;
            if (be64rd(rh) != NBD_REP_MAGIC)
                return -1;
            uint32_t type = be32rd(rh + 12);
            uint32_t len = be32rd(rh + 16);
            std::vector<char> d(len);
            if (len && read_exact(d.data(), len) < 0)
                return -1;
            if (type & (1u << 31))
                return -1;
            if (type == NBD_REP_INFO && len >= 12 && be16rd(d.data()) == NBD_INFO_EXPORT) {
                export_size = be64rd(d.data() + 2);
                trans_flags = be16rd(d.data() + 10);
            }
            if (type == NBD_REP_ACK)
                return 0;
        }
    }

    static void encode_req(char* req, uint16_t type, uint16_t flags, uint64_t handle,
                           uint64_t offset, uint32_t len) {
        be32wr(req, NBD_REQ_MAGIC);
        be16wr(req + 4, flags);
        be16wr(req + 6, type);
        be64wr(req + 8, handle);
        be64wr(req + 16, offset);
        be32wr(req + 24, len);
    }

    // returns the nbd status code of the reply (0 = success), -1 on transport error
    int xfer(uint16_t type, uint64_t offset, void* buf, uint32_t len, uint16_t flags = 0) {
        uint64_t handle = next_handle++;
        char req[28];
        encode_req(req, type, flags, handle, offset, len);
        if (write_exact(req, sizeof(req)) < 0)
            return -1;
        if (type == NBD_CMD_WRITE && len && write_exact(buf, len) < 0)
            return -1;
        char rep[16];
        if (read_exact(rep, sizeof(rep)) < 0)
            return -1;
        if (be32rd(rep) != NBD_SIMPLE_REP_MAGIC || be64rd(rep + 8) != handle)
            return -1;
        uint32_t err = be32rd(rep + 4);
        if (type == NBD_CMD_READ && err == NBD_SUCCESS && read_exact(buf, len) < 0)
            return -1;
        return (int)err;
    }

    // ---- pipelining: many requests in flight on ONE connection ----
    // The server dispatches every request to its own coroutine (gated by the
    // device's queue_depth) and each writes its own reply, so replies arrive in
    // COMPLETION order, not request order: a pipelined client must match them
    // by handle.
    struct Pending { uint16_t type; uint32_t len; void* buf; };
    std::unordered_map<uint64_t, Pending> pending;

    // queue a request without waiting; returns its handle, or 0 on error
    uint64_t submit(uint16_t type, uint64_t offset, void* buf, uint32_t len,
                    uint16_t flags = 0) {
        uint64_t handle = next_handle++;
        char req[28];
        encode_req(req, type, flags, handle, offset, len);
        if (write_exact(req, sizeof(req)) < 0)
            return 0;
        if (type == NBD_CMD_WRITE && len && write_exact(buf, len) < 0)
            return 0;
        pending[handle] = Pending{type, len, buf};
        return handle;
    }

    // collect one outstanding reply; returns its nbd status, or -1 on a
    // transport error or a reply for a request we never sent
    int collect(uint64_t* handle_out = nullptr) {
        char rep[16];
        if (read_exact(rep, sizeof(rep)) < 0)
            return -1;
        if (be32rd(rep) != NBD_SIMPLE_REP_MAGIC)
            return -1;
        uint64_t handle = be64rd(rep + 8);
        auto it = pending.find(handle);
        if (it == pending.end())
            return -1;
        uint32_t err = be32rd(rep + 4);
        if (it->second.type == NBD_CMD_READ && err == NBD_SUCCESS &&
            read_exact(it->second.buf, it->second.len) < 0)
            return -1;
        if (handle_out) *handle_out = handle;
        pending.erase(it);
        return (int)err;
    }

    void disc() {
        char req[28] = {};
        be32wr(req, NBD_REQ_MAGIC);
        be16wr(req + 6, NBD_CMD_DISC);
        write_exact(req, sizeof(req));
    }

private:
    int read_exact(void* buf, size_t n) {
        return s->read(buf, n) == (ssize_t)n ? 0 : -1;
    }
    int write_exact(const void* buf, size_t n) {
        return s->write(buf, n) == (ssize_t)n ? 0 : -1;
    }
    int send_option(uint32_t opt, const void* data, uint32_t len) {
        char h[16];
        be64wr(h, NBD_OPTS_MAGIC);
        be32wr(h + 8, opt);
        be32wr(h + 12, len);
        if (write_exact(h, sizeof(h)) < 0)
            return -1;
        if (len && write_exact(data, len) < 0)
            return -1;
        return 0;
    }
};

static const char IMG_PATH[] = "/tmp/photon-blk-nbd-test.img";
static constexpr uint64_t IMG_SIZE = 4u << 20;

class NbdTest : public test::SkippableTest {
public:
    test::TestImage img;
    fs::IFile* file = nullptr;

    void SetUp() override {
        ASSERT_EQ(0, img.create(IMG_PATH, IMG_SIZE));
        file = img.file;
    }

    void TearDown() override {
        img.release();
    }

    BlkDevInfo make_info() {
        BlkDevInfo i;
        i.identity = "photon-nbd-test";
        i.size = IMG_SIZE;
        i.features = FEATURE_FLUSH;
#ifdef __linux__
        // localfs fallocate (trim / zero_range) exists on Linux only
        i.features |= FEATURE_DISCARD | FEATURE_WRITE_ZEROES;
#endif
        return i;
    }
};

// The capability half of BlkDevInfo: what lets a caller branch on behaviour instead of
// inferring it from which factory built the object. The axes are properties of the
// transport, so they are already correct on a constructed device; `negotiated` is the
// one member that start() has to fill in.
TEST_F(NbdTest, capabilities_descriptor) {
    NbdConfig cfg(make_info());
    cfg.enable_tcp = true;
    cfg.tcp_endpoint = net::EndPoint("127.0.0.1", 0);
    cfg.loopback_device = false;
    auto dev = new_nbd_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    const BlkDevInfo& i = dev->get_info();

    // nbd's connections ARE its queue and detach() closes them on both paths, so there
    // is no backlog for a later start() to harvest and no registration to adopt or
    // drift against. shutdown() closes the connections rather than refusing a client.
    // The export's size is fixed at handshake, so resize() is not implemented at all.
    EXPECT_EQ(BlkBacklog::None, i.backlog);
    EXPECT_EQ(BlkShutdownRefusal::Disconnects, i.shutdown_refusal);
    EXPECT_EQ(BlkResizeEffect::Unsupported, i.resize_effect);
    EXPECT_EQ(BlkAdoption::NoRegistration, i.adoption);
    // detach(false) skips the in_flight poll but still runs cleanup_runtime(), which
    // joins workers that are waiting on backend I/O -- the measurement behind blk.h no
    // longer promising an immediate return
    EXPECT_EQ(false, i.detach_no_wait);
    // `offered` describes this transport's command set, not what any given backend can
    // do: it serves TRIM and WRITE_ZEROES by translating them, and a backend without
    // them answers per-request.
    EXPECT_EQ(FEATURE_FLUSH | FEATURE_DISCARD | FEATURE_WRITE_ZEROES, i.offered);
    EXPECT_EQ(0ull, i.negotiated);   // nothing advertised yet

    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    // A client cannot decline an individual transmission flag -- it accepts the set or
    // does not connect -- so what start() advertised is what is in force. Spelled out
    // per platform rather than derived from `features & offered`, which is how the
    // implementation computes it and would therefore agree with itself if it were wrong.
    uint64_t want = FEATURE_FLUSH;
#ifdef __linux__
    want |= FEATURE_DISCARD | FEATURE_WRITE_ZEROES;   // make_info() requests them here
#endif
    EXPECT_EQ(want, dev->get_info().negotiated);
}

TEST_F(NbdTest, config_validation) {
    // the pure config checks are construction-time now: no object at all
    NbdConfig cfg(make_info());
    cfg.loopback_device = false;
    errno = 0;
    EXPECT_EQ(nullptr, new_nbd_device(cfg));
    EXPECT_EQ(EINVAL, errno);

    cfg.enable_tcp = true;
    cfg.tcp_endpoint = net::EndPoint("127.0.0.1", 0);
    cfg.info.size = 0;
    errno = 0;
    EXPECT_EQ(nullptr, new_nbd_device(cfg));
    EXPECT_EQ(EINVAL, errno);

    cfg.info.size = IMG_SIZE + 500;  // not a multiple of the sector size
    errno = 0;
    EXPECT_EQ(nullptr, new_nbd_device(cfg));
    EXPECT_EQ(EINVAL, errno);

    cfg.info.size = IMG_SIZE;
    auto dev = new_nbd_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    errno = 0;
    EXPECT_EQ(-1, dev->start(file));
    EXPECT_EQ(EALREADY, errno);
    EXPECT_EQ(0, dev->shutdown());

    // a second object must not steal the unix path of a LIVE server: EBUSY,
    // and the socket file survives
    const char* path = "/tmp/photon-blk-nbd-test.sock";
    ::unlink(path);
    NbdConfig ucfg(make_info());
    ucfg.loopback_device = false;
    ucfg.unix_path = path;
    auto udev = new_nbd_device(ucfg);
    ASSERT_NE(nullptr, udev);
    DEFER(delete udev);
    ASSERT_EQ(0, udev->start(file));
    auto dev2 = new_nbd_device(ucfg);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    errno = 0;
    EXPECT_EQ(-1, dev2->start(file));
    EXPECT_EQ(EBUSY, errno);
    EXPECT_EQ(0, ::access(path, F_OK));
    EXPECT_EQ(0, udev->shutdown());

#ifndef __linux__
    NbdConfig lcfg(make_info());  // loopback_device on by default
    auto ldev = new_nbd_device(lcfg);
    ASSERT_NE(nullptr, ldev);
    DEFER(delete ldev);
    errno = 0;
    EXPECT_EQ(-1, ldev->start(file));
    EXPECT_EQ(ENOSYS, errno);
#endif
}

TEST_F(NbdTest, tcp_roundtrip) {
    NbdConfig cfg(make_info());
    cfg.loopback_device = false;
    cfg.enable_tcp = true;
    cfg.tcp_endpoint = net::EndPoint("127.0.0.1", 0);
    auto dev = new_nbd_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    EXPECT_EQ(nullptr, dev->get_device_node());

    auto srv = dev->get_server_sockets().tcp;
    ASSERT_NE(nullptr, srv);
    net::EndPoint ep;
    ASSERT_EQ(0, srv->getsockname(ep));
    ASSERT_NE(0, ep.port);

    NbdTestClient cli;
    ASSERT_EQ(0, cli.connect_tcp("127.0.0.1", ep.port));
    ASSERT_EQ(0, cli.handshake());
    EXPECT_EQ(IMG_SIZE, cli.export_size);
    EXPECT_TRUE(cli.trans_flags & NBD_TRANS_SEND_FLUSH);
    EXPECT_TRUE(cli.trans_flags & NBD_TRANS_SEND_FUA);
#ifdef __linux__
    EXPECT_TRUE(cli.trans_flags & NBD_TRANS_SEND_TRIM);
    EXPECT_TRUE(cli.trans_flags & NBD_TRANS_SEND_WRITE_ZEROES);
#endif

    std::vector<char> wbuf(8192), rbuf(8192);
    for (size_t i = 0; i < wbuf.size(); i++)
        wbuf[i] = (char)(i * 7 + 3);
    EXPECT_EQ(0, cli.xfer(NBD_CMD_WRITE, 4096, wbuf.data(), wbuf.size()));
    memset(rbuf.data(), 0, rbuf.size());
    EXPECT_EQ(0, cli.xfer(NBD_CMD_READ, 4096, rbuf.data(), rbuf.size()));
    EXPECT_EQ(0, memcmp(wbuf.data(), rbuf.data(), wbuf.size()));

    // write reached the backend file
    memset(rbuf.data(), 0, rbuf.size());
    struct iovec iov{rbuf.data(), 4096};
    ASSERT_EQ((ssize_t)4096, file->preadv(&iov, 1, 4096));
    EXPECT_EQ(0, memcmp(wbuf.data(), rbuf.data(), 4096));

    // FUA write
    EXPECT_EQ(0, cli.xfer(NBD_CMD_WRITE, 0, wbuf.data(), 4096, NBD_REQ_FUA));
    EXPECT_EQ(0, cli.xfer(NBD_CMD_FLUSH, 0, nullptr, 0));

    // out-of-bounds and unknown command
    EXPECT_EQ((int)NBD_EINVAL, cli.xfer(NBD_CMD_READ, IMG_SIZE - 10, rbuf.data(), 4096));
    EXPECT_EQ((int)NBD_ENOTSUP, cli.xfer(7, 0, nullptr, 0));

#ifdef __linux__
    EXPECT_EQ(0, cli.xfer(NBD_CMD_TRIM, 4096, nullptr, 8192));
    EXPECT_EQ(0, cli.xfer(NBD_CMD_WRITE_ZEROES, 4096, nullptr, 8192));
    memset(rbuf.data(), 1, rbuf.size());
    EXPECT_EQ(0, cli.xfer(NBD_CMD_READ, 4096, rbuf.data(), 4096));
    for (int i = 0; i < 4096; i++)
        ASSERT_EQ(0, rbuf[i]);
#endif

    EXPECT_EQ(1u, dev->get_client_connections().size());
}

TEST_F(NbdTest, unix_path_mode) {
    const char* path = "/tmp/photon-blk-nbd-test.sock";
    ::unlink(path);
    NbdConfig cfg(make_info());
    cfg.loopback_device = false;
    cfg.unix_path = path;
    auto dev = new_nbd_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    EXPECT_NE(nullptr, dev->get_server_sockets().uds);

    NbdTestClient cli;
    ASSERT_EQ(0, cli.connect_unix(path));
    ASSERT_EQ(0, cli.handshake("photon-nbd-test"));
    EXPECT_EQ(IMG_SIZE, cli.export_size);

    std::vector<char> wbuf(4096, 0x6b), rbuf(4096);
    EXPECT_EQ(0, cli.xfer(NBD_CMD_WRITE, 0, wbuf.data(), wbuf.size()));
    EXPECT_EQ(0, cli.xfer(NBD_CMD_READ, 0, rbuf.data(), rbuf.size()));
    EXPECT_EQ(0, memcmp(wbuf.data(), rbuf.data(), wbuf.size()));
}

// The same refusal on the nbd side, where the removal used to be an explicit
// unlink of our own rather than something bind() did. A caller's ordinary file
// at unix_path was deleted and a socket bound in its place, and start() then
// reported success -- so the assertion is not only that the file survived but
// that the start was refused.
TEST_F(NbdTest, unix_path_holding_a_regular_file_is_refused_and_preserved) {
    static const char WANT[] = "not a socket, and not ours to remove";
    const char* path = "/tmp/photon-blk-nbd-regular-file";
    ::unlink(path);
    DEFER(::unlink(path));
    int fd = ::open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    ASSERT_GE(fd, 0);
    ASSERT_EQ((ssize_t) sizeof(WANT), ::write(fd, WANT, sizeof(WANT)));
    ::close(fd);

    NbdConfig cfg(make_info());
    cfg.loopback_device = false;
    cfg.unix_path = path;
    auto dev = new_nbd_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    errno = 0;
    EXPECT_EQ(-1, dev->start(file));
    EXPECT_EQ(EINVAL, errno);

    struct stat st;
    ASSERT_EQ(0, ::stat(path, &st));
    EXPECT_TRUE(S_ISREG(st.st_mode));
    EXPECT_EQ(sizeof(WANT), (size_t) st.st_size);
    fd = ::open(path, O_RDONLY);
    ASSERT_GE(fd, 0);
    DEFER(::close(fd));
    char back[sizeof(WANT)] = {};
    EXPECT_EQ((ssize_t) sizeof(WANT), ::read(fd, back, sizeof(back)));
    EXPECT_EQ(0, memcmp(WANT, back, sizeof(WANT)));
}

TEST_F(NbdTest, both_endpoints) {
    const char* path = "/tmp/photon-blk-nbd-test.sock";
    ::unlink(path);
    NbdConfig cfg(make_info());
    cfg.loopback_device = false;
    cfg.unix_path = path;
    cfg.enable_tcp = true;
    cfg.tcp_endpoint = net::EndPoint("127.0.0.1", 0);
    auto dev = new_nbd_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    net::EndPoint ep;
    ASSERT_EQ(0, dev->get_server_sockets().tcp->getsockname(ep));

    NbdTestClient a, b;
    ASSERT_EQ(0, a.connect_unix(path));
    ASSERT_EQ(0, a.handshake());
    ASSERT_EQ(0, b.connect_tcp("127.0.0.1", ep.port));
    ASSERT_EQ(0, b.handshake());

    std::vector<char> wbuf(4096, 0x39), rbuf(4096);
    EXPECT_EQ(0, a.xfer(NBD_CMD_WRITE, 0, wbuf.data(), wbuf.size()));
    EXPECT_EQ(0, b.xfer(NBD_CMD_READ, 0, rbuf.data(), rbuf.size()));
    EXPECT_EQ(0, memcmp(wbuf.data(), rbuf.data(), wbuf.size()));

    EXPECT_EQ(2u, dev->get_client_connections().size());
}

// High-concurrency stress at the PROTOCOL level: in this mode nbd has no
// kernel node, so the concurrency is (1) many simultaneous client connections
// and (2) one connection with many requests in flight. The server gives every
// request its own coroutine behind a device-wide queue_depth gate, and each
// writes its own reply -- so replies arrive in completion order and a
// pipelined client must match them by handle. Blocks are self-describing
// (harness.h), which makes a misrouted or torn reply attributable rather than
// just "wrong bytes".
TEST_F(NbdTest, concurrent_stress) {
    static const char* const BIG_PATH = "/tmp/photon-blk-nbd-stress.img";
    constexpr uint64_t BIG = 64ull << 20;   // the fixture image is only 4 MiB
    test::TestImage big_img;
    ASSERT_EQ(0, big_img.create(BIG_PATH, BIG));
    auto big = big_img.file;

    BlkDevInfo info;
    info.identity = "photon-nbd-stress";
    info.size = BIG;
    info.sector_size_shift = 9;
    info.features = FEATURE_FLUSH;
    NbdConfig cfg(info);
    cfg.loopback_device = false;
    cfg.enable_tcp = true;
    cfg.tcp_endpoint = net::EndPoint("127.0.0.1", 0);
    cfg.queue_depth = 256;
    auto dev = new_nbd_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(big));
    DEFER(dev->shutdown());
    net::EndPoint ep;
    ASSERT_EQ(0, dev->get_server_sockets().tcp->getsockname(ep));

    constexpr uint64_t BASE = 1ull << 20;

    // (1) K connections, private regions, mixed request sizes
    constexpr int K = 16, ITERS = 24;
    constexpr uint64_t REGION = (BIG - BASE) / 2 / K;   // the pipelined phase
                                                        // owns the upper half
    std::atomic<int> bad{0};
    photon::semaphore done;
    for (int t = 0; t < K; t++)
        photon::thread_create11([&, t] {
            DEFER(done.signal(1));
            NbdTestClient cli;
            if (cli.connect_tcp("127.0.0.1", ep.port) < 0 || cli.handshake() < 0) {
                LOG_ERROR("stress client ` failed to connect or handshake", t);
                bad++;
                return;
            }
            test::StressRnd rnd(0x5eed1234u ^ ((uint32_t)t * 7919u));
            static const uint32_t SIZES[] = {4096, 16384, 65536};
            std::vector<char> wbuf(65536), rbuf(65536);
            for (int i = 0; i < ITERS; i++) {
                uint32_t bs = SIZES[rnd.below(3)];
                uint64_t off = BASE + (uint64_t)t * REGION +
                               rnd.below((REGION - bs) / 4096 + 1) * 4096;
                test::stress_format(wbuf.data(), bs, off, (uint32_t)t, (uint32_t)i + 1);
                int w = cli.xfer(NBD_CMD_WRITE, off, wbuf.data(), bs);
                int r = cli.xfer(NBD_CMD_READ, off, rbuf.data(), bs);
                uint32_t tid = 0, seq = 0;
                int v = test::stress_validate(rbuf.data(), bs, off, &tid, &seq);
                if (v == 0 && (tid != (uint32_t)t || seq != (uint32_t)i + 1))
                    v = EILSEQ;   // another client's block inside OUR region
                if (w || r || v) {
                    LOG_ERROR("stress client ` iter ` off ` len `: write ` read ` validate ` (owner tid=` seq=`)",
                              t, i, off, bs, w, r, v, tid, seq);
                    bad++;
                    return;
                }
            }
        });
    done.wait(K);
    EXPECT_EQ(0, bad.load());

    // (2) one connection, PIPE requests in flight per round: a fresh grid of
    // slots each round, so a read-back has exactly one legitimate writer
    constexpr int PIPE = 32, ROUNDS = 8;
    constexpr uint32_t PB = 16384;
    constexpr uint32_t PTID = 99;
    constexpr uint64_t PBASE = BASE + (BIG - BASE) / 2;
    NbdTestClient cli;
    ASSERT_EQ(0, cli.connect_tcp("127.0.0.1", ep.port));
    ASSERT_EQ(0, cli.handshake());
    std::vector<char> wbuf((size_t)PIPE * PB), rbuf((size_t)PIPE * PB);
    int pipe_bad = 0;
    for (int r = 0; r < ROUNDS && !pipe_bad; r++) {
        uint64_t base = PBASE + (uint64_t)r * PIPE * PB;
        auto seq_of = [&](int i) { return (uint32_t)(r * PIPE + i + 1); };
        for (int i = 0; i < PIPE && !pipe_bad; i++) {
            uint64_t off = base + (uint64_t)i * PB;
            test::stress_format(&wbuf[(size_t)i * PB], PB, off, PTID, seq_of(i));
            if (!cli.submit(NBD_CMD_WRITE, off, &wbuf[(size_t)i * PB], PB))
                pipe_bad = EIO;
        }
        if (!pipe_bad && !cli.submit(NBD_CMD_FLUSH, 0, nullptr, 0))
            pipe_bad = EIO;
        for (int i = 0; i < PIPE + 1 && !pipe_bad; i++) {
            int st = cli.collect();
            if (st != 0) {
                LOG_ERROR("pipelined write/flush reply `, round `", st, r);
                pipe_bad = st < 0 ? EIO : st;
            }
        }
        for (int i = 0; i < PIPE && !pipe_bad; i++) {
            uint64_t off = base + (uint64_t)i * PB;
            if (!cli.submit(NBD_CMD_READ, off, &rbuf[(size_t)i * PB], PB))
                pipe_bad = EIO;
        }
        for (int i = 0; i < PIPE && !pipe_bad; i++) {
            int st = cli.collect();
            if (st != 0) {
                LOG_ERROR("pipelined read reply `, round `", st, r);
                pipe_bad = st < 0 ? EIO : st;
            }
        }
        for (int i = 0; i < PIPE && !pipe_bad; i++) {
            uint64_t off = base + (uint64_t)i * PB;
            uint32_t tid = 0, seq = 0;
            int v = test::stress_validate(&rbuf[(size_t)i * PB], PB, off, &tid, &seq);
            if (v == 0 && (tid != PTID || seq != seq_of(i)))
                v = EILSEQ;
            if (v) {
                LOG_ERROR("pipelined slot ` off ` round `: ` (owner tid=` seq=`)",
                          i, off, r, v, tid, seq);
                pipe_bad = v;
            }
        }
    }
    EXPECT_EQ(0, pipe_bad);
    EXPECT_TRUE(cli.pending.empty());
}

TEST_F(NbdTest, read_only) {
    NbdConfig cfg(make_info());
    cfg.loopback_device = false;
    cfg.enable_tcp = true;
    cfg.tcp_endpoint = net::EndPoint("127.0.0.1", 0);
    cfg.read_only = true;
    auto dev = new_nbd_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    net::EndPoint ep;
    ASSERT_EQ(0, dev->get_server_sockets().tcp->getsockname(ep));
    NbdTestClient cli;
    ASSERT_EQ(0, cli.connect_tcp("127.0.0.1", ep.port));
    ASSERT_EQ(0, cli.handshake());
    EXPECT_TRUE(cli.trans_flags & NBD_TRANS_READ_ONLY);

    std::vector<char> buf(4096, 0x11);
    EXPECT_EQ((int)NBD_EPERM, cli.xfer(NBD_CMD_WRITE, 0, buf.data(), buf.size()));
#ifdef __linux__
    EXPECT_EQ((int)NBD_EPERM, cli.xfer(NBD_CMD_TRIM, 0, nullptr, 4096));
#endif
    EXPECT_EQ(0, cli.xfer(NBD_CMD_READ, 0, buf.data(), buf.size()));
}

#ifdef __linux__
TEST_F(NbdTest, loopback_device) {
    if (geteuid() != 0)
        return report_skip("loopback test requires root");
    if (::access("/sys/block/nbd0", F_OK) != 0)
        return report_skip("nbd kernel module not loaded");

    NbdConfig cfg(make_info());  // anonymous UDS + loopback device
    auto dev = new_nbd_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    const char* node = dev->get_device_node();
    ASSERT_NE(nullptr, node);
    auto srvs = dev->get_server_sockets();
    EXPECT_EQ(nullptr, srvs.uds);
    EXPECT_EQ(nullptr, srvs.tcp);
    for (int i = 0; i < 1000 && dev->get_client_connections().empty(); i++)
        photon::thread_usleep(1000);
    EXPECT_EQ(1u, dev->get_client_connections().size());

    // device IO must run off the photon vcpu: the requests loop back through
    // the serve coroutines on this very vcpu
    int result = -1;
    std::string devnode(node);
    test::run_off_vcpu([&] {
        std::vector<char> wbuf(4096, 0x5a), rbuf(4096);
        int fd = ::open(devnode.c_str(), O_RDWR);
        if (fd < 0) {
            result = errno;
            return;
        }
        DEFER(::close(fd));
        if (::pwrite(fd, wbuf.data(), wbuf.size(), 8192) != (ssize_t)wbuf.size()) {
            result = errno;
            return;
        }
        // buffered writes sit in the kernel page cache until writeback; fsync
        // forces them through the loopback into the backend file
        if (::fsync(fd) < 0) {
            result = errno;
            return;
        }
        if (::pread(fd, rbuf.data(), rbuf.size(), 8192) != (ssize_t)rbuf.size()) {
            result = errno;
            return;
        }
        result = memcmp(wbuf.data(), rbuf.data(), wbuf.size()) ? EILSEQ : 0;
    });
    EXPECT_EQ(0, result);

    // the written pattern shows up in the backend file
    std::vector<char> rbuf(4096);
    struct iovec iov{rbuf.data(), rbuf.size()};
    ASSERT_EQ((ssize_t)rbuf.size(), file->preadv(&iov, 1, 8192));
    for (int i = 0; i < 4096; i++)
        ASSERT_EQ(0x5a, (uint8_t)rbuf[i]);
}
#endif

// nbd's parallelism is its client connection count, so the fan-out unit is the
// connection, not a queue. Two clients on a two-vcpu pool must land on two
// different vcpus; if the migration in spawn_serve_conn were missing, both
// serve_conn coroutines would sit on this vcpu and the count would be 1.
TEST_F(NbdTest, connections_spread_over_the_pool) {
    test::TestPool pool(2);
    ASSERT_EQ(2, pool->get_vcpu_num());
    test::RecordingFile rec(file);
    auto* caller = photon::get_vcpu();

    NbdConfig cfg(make_info());
    cfg.loopback_device = false;
    cfg.enable_tcp = true;
    cfg.tcp_endpoint = net::EndPoint("127.0.0.1", 0);
    cfg.pool = pool;
    auto dev = new_nbd_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rec));
    DEFER(dev->shutdown());

    net::EndPoint ep;
    ASSERT_EQ(0, dev->get_server_sockets().tcp->getsockname(ep));

    // two clients, each with its own connection, both doing real IO so the
    // serving coroutines actually touch the backend
    std::vector<char> wbuf(8192), rbuf(8192);
    for (size_t i = 0; i < wbuf.size(); i++)
        wbuf[i] = (char)(i * 7 + 3);
    for (int c = 0; c < 2; c++) {
        NbdTestClient cli;
        ASSERT_EQ(0, cli.connect_tcp("127.0.0.1", ep.port));
        ASSERT_EQ(0, cli.handshake());
        uint64_t base = 4096u + (uint64_t)c * 65536;   // this test's own region
        EXPECT_EQ(0, cli.xfer(NBD_CMD_WRITE, base, wbuf.data(), wbuf.size()));
        EXPECT_EQ(0, cli.xfer(NBD_CMD_READ, base, rbuf.data(), rbuf.size()));
        EXPECT_EQ(0, memcmp(wbuf.data(), rbuf.data(), wbuf.size()));
        // An FUA write, so this connection drives the durability branch too. What it
        // asserts here is only that a write asking for durability still succeeds on a
        // pool vcpu; the case that counts the sync itself, and so can tell "persisted"
        // from "acked", is an_fua_write_is_durable_before_its_reply.
        EXPECT_EQ(0, cli.xfer(NBD_CMD_WRITE, base, wbuf.data(), wbuf.size(), NBD_REQ_FUA));
#ifdef __linux__
        // TRIM lands on the VIRTUAL fallocate, because IFile::trim is a plain
        // method that calls it: a pass-through backend that forgot to forward
        // fallocate would answer ENOSYS, and nbd reports that to the client as
        // NBD_ENOTSUP. WRITE_ZEROES proves nothing on its own any more -- ENOSYS
        // is one of the two "no hole-punch" answers nbd falls back on, so it
        // would succeed by writing zeroes instead. Offsets are inside this
        // client's own region, disjoint from the pair above.
        EXPECT_EQ(0, cli.xfer(NBD_CMD_TRIM, base + 16384, nullptr, 8192));
        EXPECT_EQ(0, cli.xfer(NBD_CMD_WRITE_ZEROES, base + 16384, nullptr, 8192));
#endif
    }

    // accept_th deliberately stays on this vcpu (the control plane does not
    // move), so the caller's vcpu appearing in the set would mean a serve_conn
    // did NOT move. No backend verification here on purpose: it would run on
    // this vcpu through `rec` and record the caller as a false placement.
    EXPECT_EQ(2u, rec.vcpu_count());
    EXPECT_FALSE(rec.ran_on(caller));
}

// Eight OS threads churn connections while the caller's vcpu traverses the
// connection list and, mid-churn, drains and restarts the device. Since the
// migration the list is written from the pool vcpus (spawn_serve_conn's
// push_back, serve_conn's DEFER erase) and read from this one
// (get_client_connections, detach's drain), so the traversals race the
// reallocations if the list loses its lock. The assertions are structural:
// returned pointers are never null and the list never grows past the live
// clients. The streams are NOT dereferenced -- a returned pointer may belong
// to a connection that closed right after the copy, which is legitimate;
// dereferencing it would be this test's own use-after-free.
TEST_F(NbdTest, concurrent_connect_disconnect_under_pool_serving) {
    test::TestPool pool(4);
    NbdConfig cfg(make_info());
    cfg.loopback_device = false;
    cfg.enable_tcp = true;
    cfg.tcp_endpoint = net::EndPoint("127.0.0.1", 0);
    cfg.pool = pool;
    auto dev = new_nbd_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    net::EndPoint ep;
    ASSERT_EQ(0, dev->get_server_sockets().tcp->getsockname(ep));
    ASSERT_NE(0, ep.port);

    // The restart rebinds an ephemeral port, so the workers re-read `port` per
    // attempt, and `gen` brackets the restart window (bumped once before the
    // detach and once after the new port is published, with `down` covering the
    // stretch in between): a failed attempt that overlapped the window is
    // expected, one that did not is a real error. A data mismatch is never
    // excused -- a transfer torn by the restart returns -1, not wrong bytes.
    std::atomic<uint16_t> port{ep.port};
    std::atomic<uint32_t> gen{0};
    std::atomic<bool> down{false};
    std::atomic<int> bad{0};
    std::atomic<int> ok{0};        // fully verified connect/write/read cycles
    std::atomic<int> finished{0};

    constexpr int THREADS = 8, ITERS = 20;
    std::vector<std::thread> ths;
    for (int t = 0; t < THREADS; t++)
        ths.emplace_back([&, t] {
            // NbdTestClient speaks photon sockets, whose fd waits go through
            // the CURRENT vcpu's event engine, so each OS thread brings its own
            if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE) != 0) {
                bad++;
                finished++;
                return;
            }
            DEFER({ photon::fini(); finished++; });
            // per-thread buffers: a shared read-back buffer would have the
            // threads scribble over each other's validation
            std::vector<char> wbuf(4096), rbuf(4096);
            for (size_t i = 0; i < wbuf.size(); i++)
                wbuf[i] = (char)(i * 31 + 7);
            uint64_t off = 4096 + (uint64_t)t * (IMG_SIZE / THREADS);
            for (int i = 0; i < ITERS; i++) {
                // Do not spend iterations inside the restart window: every
                // connect fails there and is legitimately excused, so without
                // this wait a worker can burn its whole budget doing no
                // verifiable work at all -- and `bad == 0` would still hold.
                while (down.load(std::memory_order_acquire))
                    photon::thread_usleep(200);
                uint32_t g0 = gen.load(std::memory_order_acquire);
                int st = -1;
                {
                    NbdTestClient cli;
                    if (cli.connect_tcp("127.0.0.1", port.load(std::memory_order_acquire)) == 0 &&
                        cli.handshake() == 0) {
                        int w = cli.xfer(NBD_CMD_WRITE, off, wbuf.data(), wbuf.size());
                        int r = w ? -1 : cli.xfer(NBD_CMD_READ, off, rbuf.data(), rbuf.size());
                        st = w ? w : r;
                        if (!st && memcmp(wbuf.data(), rbuf.data(), wbuf.size())) {
                            bad++;   // corruption is never a restart-window artifact
                            continue;
                        }
                    }
                }
                if (st && gen.load(std::memory_order_acquire) == g0 &&
                    !down.load(std::memory_order_acquire)) {
                    LOG_ERROR("client ` iter ` failed with ` outside the restart window", t, i, st);
                    bad++;
                }
                if (!st)
                    ok++;   // connected, wrote, read back, and the bytes matched
            }
        });

    // Poll on the caller's vcpu -- accept_loop lives here, so this loop must
    // keep yielding or no worker can even connect -- and restart once while
    // connections are live. detach(true) drains the in-flight requests and
    // drops every connection; start() rebinds and publishes the new port.
    // No semaphore here: blocking would stop the yields accept_loop needs.
    bool restarted = false;
    uint64_t deadline = photon::now + 300ull * 1000 * 1000;
    bool timeout_hit = false;
    while (finished.load() < THREADS) {
        auto conns = dev->get_client_connections();
        // a closing client's Conn can still be listed while the same client's
        // next attempt is already accepted, so the bound is twice the thread
        // count; unbounded growth or a null stream is what a lost lock causes
        EXPECT_LE(conns.size(), (size_t)THREADS * 2);
        for (auto s : conns)
            EXPECT_NE(nullptr, s);
        if (!restarted && !conns.empty()) {
            restarted = true;
            down = true;
            gen++;
            EXPECT_EQ(0, dev->detach(/*wait_pending=*/true));
            EXPECT_EQ(0, dev->start(file));
            net::EndPoint ep2;
            EXPECT_EQ(0, dev->get_server_sockets().tcp->getsockname(ep2));
            port = ep2.port;
            gen++;
            down = false;
        }
        if (photon::now >= deadline) {
            timeout_hit = true;
            break;
        }
        photon::thread_usleep(200);
    }
    for (auto& th : ths)
        th.join();
    EXPECT_FALSE(timeout_hit);
    EXPECT_TRUE(restarted);
    EXPECT_EQ(0, bad.load());
    // `bad == 0` on its own would also hold if the workers never completed a
    // cycle, so count the cycles too. The workers wait out the restart window
    // instead of spending iterations in it, which leaves at most one attempt
    // per thread that the window can still catch (the one already in flight
    // when `down` went up); every other one must have been a fully verified
    // connect / write / read-back / compare cycle.
    EXPECT_GE(ok.load(), THREADS * ITERS - THREADS);
}

// accept_th never moves -- nbd's control plane stays on the caller's vcpu -- and
// with no pool neither does serve_conn, so the only vcpu that touches the backend
// is this one. No backend verification here on purpose: it would run on this vcpu
// through `rec` and record the caller as a placement twice over.
TEST_F(NbdTest, pool_null_serves_on_the_caller_vcpu) {
    test::RecordingFile rec(file);
    auto* caller = photon::get_vcpu();

    NbdConfig cfg(make_info());
    cfg.loopback_device = false;
    cfg.enable_tcp = true;
    cfg.tcp_endpoint = net::EndPoint("127.0.0.1", 0);
    cfg.pool = nullptr;
    auto dev = new_nbd_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rec));
    DEFER(dev->shutdown());
    net::EndPoint ep;
    ASSERT_EQ(0, dev->get_server_sockets().tcp->getsockname(ep));

    NbdTestClient cli;
    ASSERT_EQ(0, cli.connect_tcp("127.0.0.1", ep.port));
    ASSERT_EQ(0, cli.handshake());
    std::vector<char> wbuf(8192), rbuf(8192);
    for (size_t i = 0; i < wbuf.size(); i++)
        wbuf[i] = (char)(i * 7 + 3);
    EXPECT_EQ(0, cli.xfer(NBD_CMD_WRITE, 4096, wbuf.data(), wbuf.size()));
    EXPECT_EQ(0, cli.xfer(NBD_CMD_READ, 4096, rbuf.data(), rbuf.size()));
    EXPECT_EQ(0, memcmp(wbuf.data(), rbuf.data(), wbuf.size()));

    EXPECT_EQ(1u, rec.vcpu_count());
    EXPECT_TRUE(rec.ran_on(caller));
}

// check_pool_engines' integration half: the helper is unit-tested on its own, this
// proves the transport actually asks. A pool whose vcpus cannot host the serving
// coroutines is a configuration error, so start() refuses it up front, before any
// endpoint is bound.
TEST_F(NbdTest, pool_without_an_event_engine_is_refused) {
    photon::WorkPool bad(2);      // ev_engine defaults to 0: no engine at all
    test::RecordingFile rec(file);

    NbdConfig cfg(make_info());
    cfg.loopback_device = false;
    cfg.enable_tcp = true;
    cfg.tcp_endpoint = net::EndPoint("127.0.0.1", 0);
    cfg.pool = &bad;
    auto dev = new_nbd_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    errno = 0;
    EXPECT_EQ(-1, dev->start(&rec));
    EXPECT_EQ(EINVAL, errno);
    EXPECT_EQ(0u, rec.vcpu_count());
}

// ---------------------------------------------------------------------------
// backends and instruments the cases below need and the harness does not have
// ---------------------------------------------------------------------------

// No hole-punch, and zero writes that come back one byte short with errno clean --
// the shape that used to be acked as NBD_SUCCESS, because a short count does not set
// errno and errno_to_nbd(0) is NBD_SUCCESS.
class ShortZeroFile : public test::BackendProbe {
public:
    explicit ShortZeroFile(fs::IFile* f) : test::BackendProbe(f) {}
    int fallocate(int mode, off_t offset, off_t len) override {
        errno = EOPNOTSUPP;   // no ZERO_RANGE: the fallback is the path under test
        return -1;
    }
    ssize_t pwritev(const struct iovec* iov, int iovcnt, off_t offset) override {
        ssize_t r = test::BackendProbe::pwritev(iov, iovcnt, offset);
        if (r > 1) {
            r -= 1;
            errno = 0;   // nothing failed, which is exactly what a short count means
        }
        return r;
    }
};

// Counts coroutine stacks rather than measuring the process: whether a freed stack's
// address space goes back to the OS is the allocator's business, while a stack that
// was allocated and never freed IS the leak. Forwards to the default allocator, so
// nothing else about allocation changes while it is installed.
struct StackCounter {
    std::atomic<long> allocs{0}, deallocs{0};
    long live() const { return allocs.load() - deallocs.load(); }
    void* alloc(size_t size) {
        allocs.fetch_add(1);
        return photon::default_photon_thread_stack_alloc(nullptr, size);
    }
    void dealloc(void* ptr, size_t size) {
        deallocs.fetch_add(1);
        photon::default_photon_thread_stack_dealloc(nullptr, ptr, size);
    }
    // stack-allocator.h calls trim() and stats() optional, but the
    // set_photon_thread_stack_allocator(T&) convenience binds an object by taking
    // the address of all four members, so an object without these two does not
    // compile. The default allocator this forwards to pools nothing, so a zeroed
    // answer from both is the honest one rather than a stub.
    size_t trim(size_t) { return 0; }
    photon::StackPoolStats stats() { return {}; }
};

// The device object outlives a shutdown(), and start() overwrites the backend
// pointer, so a backend released only by the destructor is leaked by the next
// start() -- which is what blk.h's contract means by deleting it on shutdown.
TEST_F(NbdTest, shutdown_releases_a_backend_it_owns) {
    std::atomic<int> destroyed{0};
    NbdConfig cfg(make_info());
    cfg.loopback_device = false;
    cfg.enable_tcp = true;
    cfg.tcp_endpoint = net::EndPoint("127.0.0.1", 0);
    auto dev = new_nbd_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);

    ASSERT_EQ(0, dev->start(new test::CountingFile(file, &destroyed), /*ownership=*/true));
    EXPECT_EQ(0, destroyed.load());
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_EQ(1, destroyed.load());

    // Usable again, which is the transition the leak hid behind: a second start()
    // serves a second backend instead of overwriting a pointer to a live one.
    ASSERT_EQ(0, dev->start(new test::CountingFile(file, &destroyed), true));
    EXPECT_EQ(1, destroyed.load());
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_EQ(2, destroyed.load());

    // An UNOWNED backend stays the caller's to delete, through shutdown and through
    // the destructor alike.
    fs::IFile* mine = new test::CountingFile(file, &destroyed);
    ASSERT_EQ(0, dev->start(mine, false));
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_EQ(2, destroyed.load());
    delete mine;
    EXPECT_EQ(3, destroyed.load());
}

// FUA means the bytes are durable before the reply, and the reply is the device's
// word that they are. pwritev2(RWF_DSYNC) cannot carry that word: IFile::pwritev2 is
// not pure virtual and its base body discards `flags` and forwards to pwritev, so a
// backend that does not override it acks a cached write. The probe counts syncs, and
// xfer() returns only once the reply has arrived, so a count read here is a count
// read after the ack.
TEST_F(NbdTest, an_fua_write_is_durable_before_its_reply) {
    test::BackendProbe probe(file);
    NbdConfig cfg(make_info());
    cfg.loopback_device = false;
    cfg.enable_tcp = true;
    cfg.tcp_endpoint = net::EndPoint("127.0.0.1", 0);
    auto dev = new_nbd_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&probe));
    DEFER(dev->shutdown());
    net::EndPoint ep;
    ASSERT_EQ(0, dev->get_server_sockets().tcp->getsockname(ep));

    NbdTestClient cli;
    ASSERT_EQ(0, cli.connect_tcp("127.0.0.1", ep.port));
    ASSERT_EQ(0, cli.handshake());
    std::vector<char> wbuf(8192, 0x5a);

    // a write without FUA persists nothing on its own
    EXPECT_EQ(0, cli.xfer(NBD_CMD_WRITE, 0, wbuf.data(), wbuf.size()));
    EXPECT_EQ(1, probe.writes.load());
    EXPECT_EQ(0, probe.datasyncs.load());

    // with FUA it is one more write and exactly one more fdatasync -- fdatasync and
    // not fsync, because the request is for data, and the WRITE_ZEROES branch has
    // always synced this way, so the two branches have to agree
    EXPECT_EQ(0, cli.xfer(NBD_CMD_WRITE, 0, wbuf.data(), wbuf.size(), NBD_REQ_FUA));
    EXPECT_EQ(2, probe.writes.load());
    EXPECT_EQ(1, probe.datasyncs.load());
    EXPECT_EQ(0, probe.syncs.load());

#ifdef __linux__
    // TRIM and WRITE_ZEROES carry FUA too, and neither syncs without it
    EXPECT_EQ(0, cli.xfer(NBD_CMD_TRIM, 16384, nullptr, 8192, NBD_REQ_FUA));
    EXPECT_EQ(2, probe.datasyncs.load());
    EXPECT_EQ(0, cli.xfer(NBD_CMD_WRITE_ZEROES, 32768, nullptr, 8192, NBD_REQ_FUA));
    EXPECT_EQ(3, probe.datasyncs.load());
    EXPECT_EQ(0, cli.xfer(NBD_CMD_TRIM, 16384, nullptr, 8192));
    EXPECT_EQ(0, cli.xfer(NBD_CMD_WRITE_ZEROES, 32768, nullptr, 8192));
    EXPECT_EQ(3, probe.datasyncs.load());
#endif
}

// A short count is a legal pwritev result and leaves errno alone. Translating it with
// errno therefore reported whatever the last unrelated failure happened to leave
// behind -- and errno_to_nbd(0) is NBD_SUCCESS, so a zero-fill that wrote 8191 of
// 8192 bytes was acked as complete and the last byte was never anybody's problem.
TEST_F(NbdTest, a_short_zero_fill_is_not_success) {
#ifndef __linux__
    // A skip has to be a preprocessor branch here, not an early return: the body below
    // names NBD_CMD_WRITE_ZEROES, which this file defines only where the server has a
    // WRITE_ZEROES handler at all, so on another platform it would not compile.
    report_skip("IFile::zero_range is fallocate-based, so WRITE_ZEROES is Linux-only");
#else
    ShortZeroFile probe(file);
    NbdConfig cfg(make_info());
    cfg.loopback_device = false;
    cfg.enable_tcp = true;
    cfg.tcp_endpoint = net::EndPoint("127.0.0.1", 0);
    auto dev = new_nbd_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&probe));
    DEFER(dev->shutdown());
    net::EndPoint ep;
    ASSERT_EQ(0, dev->get_server_sockets().tcp->getsockname(ep));

    NbdTestClient cli;
    ASSERT_EQ(0, cli.connect_tcp("127.0.0.1", ep.port));
    ASSERT_EQ(0, cli.handshake());
    EXPECT_EQ((int)NBD_EIO, cli.xfer(NBD_CMD_WRITE_ZEROES, 0, nullptr, 8192));
    // the fallback really did run and really did write: this is a short write, not a
    // refused one, and the distinction is the whole finding
    EXPECT_GT(probe.writes.load(), 0);
#endif
}

// An option payload used to be skip_read and discarded, which let a 4-byte OPT_GO
// through although even an empty name with no items needs 6, and left the export name
// unread -- so any name at all was served this export. It is parsed now: the framing
// has to add up exactly, and the name has to be this export's, or empty, which asks
// for the default export.
TEST_F(NbdTest, an_option_payload_is_parsed_not_skipped) {
    NbdConfig cfg(make_info());
    cfg.loopback_device = false;
    cfg.enable_tcp = true;
    cfg.tcp_endpoint = net::EndPoint("127.0.0.1", 0);
    auto dev = new_nbd_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    net::EndPoint ep;
    ASSERT_EQ(0, dev->get_server_sockets().tcp->getsockname(ep));

    const std::string& id = cfg.info.identity;
    struct Row {
        uint32_t opt;
        std::vector<char> payload;
        bool reply_comes;   // the protocol allows none for EXPORT_NAME
        const char* why;
    };
    const Row rows[] = {
        {NBD_OPT_GO, std::vector<char>(4, 0), true,
         "four bytes: a name length, and no room for the item count"},
        {NBD_OPT_GO, NbdTestClient::go_payload("", 1), true,
         "one information item requested and no bytes behind the count"},
        {NBD_OPT_GO, NbdTestClient::go_payload("some-other-export"), true,
         "an export this device does not serve"},
        {NBD_OPT_INFO, NbdTestClient::go_payload("some-other-export"), true,
         "the same refusal on OPT_INFO"},
        {NBD_OPT_GO, NbdTestClient::go_payload_with_trailer(id.c_str(), 2), true,
         "the name matches and every read succeeds, but two bytes are left over"},
        {NBD_OPT_EXPORT_NAME, NbdTestClient::name_payload("some-other-export"), false,
         "EXPORT_NAME is answered with the export meta or with a close, never a reply"},
    };
    for (const auto& r : rows) {
        NbdTestClient cli;
        ASSERT_EQ(0, cli.connect_tcp("127.0.0.1", ep.port));
        ASSERT_EQ(0, cli.handshake_front());
        ASSERT_EQ(0, cli.send_raw_option(r.opt, r.payload.data(), (uint32_t)r.payload.size()));
        uint32_t type = 0;
        std::vector<char> data;
        int got = cli.read_opt_reply(&type, &data);
        if (r.reply_comes) {
            ASSERT_EQ(0, got) << r.why;
            EXPECT_EQ(NBD_REP_ERR_INVALID, type) << r.why;
        } else {
            EXPECT_EQ(-1, got) << r.why;
        }
    }

    // Both ways in still work, which is what keeps the rows above from passing on a
    // server that refuses everything. A matching name on OPT_GO reaches the
    // transmission phase through INFO + ACK ...
    {
        NbdTestClient cli;
        ASSERT_EQ(0, cli.connect_tcp("127.0.0.1", ep.port));
        ASSERT_EQ(0, cli.handshake(id.c_str()));
        EXPECT_EQ(IMG_SIZE, cli.export_size);
        std::vector<char> rbuf(4096);
        EXPECT_EQ(0, cli.xfer(NBD_CMD_READ, 0, rbuf.data(), rbuf.size()));
    }
    // ... and on OPT_EXPORT_NAME through the export meta, with no option reply at all.
    {
        NbdTestClient cli;
        ASSERT_EQ(0, cli.connect_tcp("127.0.0.1", ep.port));
        ASSERT_EQ(0, cli.handshake_front());
        std::vector<char> name = NbdTestClient::name_payload(id.c_str());
        ASSERT_EQ(0, cli.send_raw_option(NBD_OPT_EXPORT_NAME, name.data(), (uint32_t)name.size()));
        char meta[10];
        ASSERT_EQ(0, cli.read_raw(meta, sizeof(meta)));
        EXPECT_EQ(IMG_SIZE, be64rd(meta));
        EXPECT_TRUE(be16rd(meta + 8) & NBD_TRANS_SEND_FLUSH);
        std::vector<char> rbuf(4096);
        EXPECT_EQ(0, cli.xfer(NBD_CMD_READ, 0, rbuf.data(), rbuf.size()));
    }
}

// Every connection coroutine is joinable, because cleanup_runtime has to be able to
// interrupt one that is parked in a gate wait -- and a joinable photon thread keeps
// its stack until somebody joins it. Reaping them only at cleanup therefore made the
// device hold one stack per connection ever MADE rather than per connection live:
// 8 MiB each at the default stack_size, which is how 40 connect/disconnect cycles
// came to 320 MiB of address space. Each worker now hands its own handle to a
// throwaway coroutine that joins it, so the count comes back.
TEST_F(NbdTest, connection_churn_does_not_accumulate_stacks) {
    StackCounter counter;
    auto saved = photon::get_photon_thread_stack_allocator();
    ASSERT_EQ(0, photon::set_photon_thread_stack_allocator(counter));
    // restored LAST: a stack has to be freed by the allocator that created it, and
    // the DEFERs below destroy the device -- and with it its workers -- first
    DEFER(photon::set_photon_thread_stack_allocator(saved));

    NbdConfig cfg(make_info());
    cfg.loopback_device = false;
    cfg.enable_tcp = true;
    cfg.tcp_endpoint = net::EndPoint("127.0.0.1", 0);
    auto dev = new_nbd_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    net::EndPoint ep;
    ASSERT_EQ(0, dev->get_server_sockets().tcp->getsockname(ep));

    const int CYCLES = 24;
    long base = counter.live();
    std::vector<char> rbuf(4096);
    for (int i = 0; i < CYCLES; i++) {
        NbdTestClient cli;
        ASSERT_EQ(0, cli.connect_tcp("127.0.0.1", ep.port));
        ASSERT_EQ(0, cli.handshake());
        ASSERT_EQ(0, cli.xfer(NBD_CMD_READ, 0, rbuf.data(), rbuf.size()));
    }   // each client's destructor sends DISC and closes, so its worker finishes

    // Retiring a worker is asynchronous against this vcpu -- a reaper coroutine joins
    // it -- so wait for the stacks to come back, bounded, instead of sampling once.
    long live = counter.live();
    for (int i = 0; i < 250 && live > base; i++) {
        photon::thread_usleep(20 * 1000);
        live = counter.live();
    }
    EXPECT_LE(live, base) << CYCLES << " connections came and went and are still "
                             "holding stacks; each one holds stack_size bytes";
    // the control: a counter that never saw an allocation would report live == base
    // and pass the assertion above without measuring anything
    EXPECT_GT(counter.allocs.load(), (long)CYCLES);
}

// A WRITE's payload is read while the request holds BOTH device-wide gates, so a
// client that sends the header and then stops holds a queue-depth slot and its share
// of the byte budget for as long as it likes -- with queue_depth 1 that is every slot
// there is, and the honest client behind it waits. cfg.timeout releases nothing here:
// it is the kernel's request timeout for the loopback device.
TEST_F(NbdTest, a_stalled_write_payload_cannot_starve_another_client) {
    NbdConfig cfg(make_info());
    cfg.loopback_device = false;
    cfg.enable_tcp = true;
    cfg.tcp_endpoint = net::EndPoint("127.0.0.1", 0);
    cfg.queue_depth = 1;      // one slot, so the stall is total rather than partial
    cfg.stall_timeout = 2;    // seconds; enough margin for slow CI runners
    auto dev = new_nbd_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    net::EndPoint ep;
    ASSERT_EQ(0, dev->get_server_sockets().tcp->getsockname(ep));

    NbdTestClient stalled;
    ASSERT_EQ(0, stalled.connect_tcp("127.0.0.1", ep.port));
    ASSERT_EQ(0, stalled.handshake());
    // the header of a 64 KiB write, and then nothing: the server has taken both gates
    // and is waiting for bytes that are not coming
    ASSERT_EQ(0, stalled.send_header_only(NBD_CMD_WRITE, 0, 65536));
    LOG_INFO("diag: A");

    NbdTestClient honest;
    ASSERT_EQ(0, honest.connect_tcp("127.0.0.1", ep.port));
    ASSERT_EQ(0, honest.handshake());
    honest.set_timeout(10 * 1000 * 1000);
    std::vector<char> wbuf(4096, 0x33);
    LOG_INFO("diag: B");
    EXPECT_EQ(0, honest.xfer(NBD_CMD_WRITE, 4096, wbuf.data(), wbuf.size()));
    LOG_INFO("diag: C");
    std::vector<char> rbuf(4096);
    EXPECT_EQ(0, honest.xfer(NBD_CMD_READ, 4096, rbuf.data(), rbuf.size()));
    LOG_INFO("diag: D");
    EXPECT_EQ(0, memcmp(wbuf.data(), rbuf.data(), wbuf.size()));
    LOG_INFO("diag: E");
    stalled.disconnect();
    LOG_INFO("diag: F");
    honest.disconnect();
    LOG_INFO("diag: G");
    photon::thread_usleep(500 * 1000);
    LOG_INFO("diag: H");
    EXPECT_EQ(0u, dev->get_client_connections().size());
    LOG_INFO("diag: I");
}

}  // namespace blk
}  // namespace photon

int main(int argc, char** arg) {
    // A consumer child is this binary re-executed with a sentinel in argv[1]:
    // dispatch it before photon::init() and before gtest sees that argument.
    int cons = photon::blk::test::consumer_child_main(argc, arg);
    if (cons != photon::blk::test::CONS_NOT_A_CHILD)
        return cons;
    if (photon::init(photon::blk::test::TEST_EVENT_ENGINE,
                     photon::blk::test::TEST_IO_ENGINE))
        return -1;
    DEFER(photon::fini());
    ::testing::InitGoogleTest(&argc, arg);
    return RUN_ALL_TESTS();
}
