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

#include "../../test/gtest.h"
#include "harness.h"

#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/fs/localfs.h>
#include <photon/net/socket.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>   // thread_create11 for the stress clients

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace photon {
namespace blk {

// NBD protocol constants (mirrors the ones internal to nbd.cpp)
static constexpr uint64_t NBD_INIT_MAGIC   = 0x4e42444d41474943ull;
static constexpr uint64_t NBD_OPTS_MAGIC   = 0x49484156454f5054ull;
static constexpr uint64_t NBD_REP_MAGIC    = 0x0003e889045565a9ull;
static constexpr uint32_t NBD_REQ_MAGIC    = 0x25609513;
static constexpr uint32_t NBD_SIMPLE_REP_MAGIC = 0x67446698;
static constexpr uint32_t NBD_FLAG_C_FIXED_NEWSTYLE = 1u << 0;
static constexpr uint32_t NBD_FLAG_C_NO_ZEROES      = 1u << 1;
static constexpr uint32_t NBD_OPT_GO          = 7;
static constexpr uint32_t NBD_REP_ACK         = 1;
static constexpr uint32_t NBD_REP_INFO        = 3;
static constexpr uint16_t NBD_INFO_EXPORT     = 0;
static constexpr uint16_t NBD_TRANS_READ_ONLY         = 1u << 1;
static constexpr uint16_t NBD_TRANS_SEND_FLUSH        = 1u << 2;
static constexpr uint16_t NBD_TRANS_SEND_FUA          = 1u << 3;
#ifdef __linux__
static constexpr uint16_t NBD_TRANS_SEND_TRIM         = 1u << 5;
static constexpr uint16_t NBD_TRANS_SEND_WRITE_ZEROES = 1u << 6;
#endif
static constexpr uint16_t NBD_CMD_READ         = 0;
static constexpr uint16_t NBD_CMD_WRITE        = 1;
static constexpr uint16_t NBD_CMD_DISC         = 2;
static constexpr uint16_t NBD_CMD_FLUSH        = 3;
#ifdef __linux__
static constexpr uint16_t NBD_CMD_TRIM         = 4;
static constexpr uint16_t NBD_CMD_WRITE_ZEROES = 6;
#endif
static constexpr uint16_t NBD_REQ_FUA     = 1u << 0;
static constexpr uint32_t NBD_SUCCESS = 0;
static constexpr uint32_t NBD_EPERM   = 1;
static constexpr uint32_t NBD_EINVAL  = 22;
static constexpr uint32_t NBD_ENOTSUP = 95;

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

    int handshake(const char* export_name = "") {
        char hdr[18];
        if (read_exact(hdr, sizeof(hdr)) < 0)
            return -1;
        if (be64rd(hdr) != NBD_INIT_MAGIC || be64rd(hdr + 8) != NBD_OPTS_MAGIC)
            return -1;
        char cf[4];
        be32wr(cf, NBD_FLAG_C_FIXED_NEWSTYLE | NBD_FLAG_C_NO_ZEROES);
        if (write_exact(cf, sizeof(cf)) < 0)
            return -1;

        size_t n = strlen(export_name);
        std::vector<char> data(2 + n + 2);
        be16wr(data.data(), (uint16_t)n);
        memcpy(data.data() + 2, export_name, n);
        be16wr(data.data() + 2 + n, 0);
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

class NbdTest : public ::testing::Test {
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
        GTEST_SKIP() << "loopback test requires root";
    if (::access("/sys/block/nbd0", F_OK) != 0)
        GTEST_SKIP() << "nbd kernel module not loaded";

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
        // an FUA write is issued as pwritev2(RWF_DSYNC): this asserts the probe
        // forwards it instead of letting the base default drop the flag
        EXPECT_EQ(0, cli.xfer(NBD_CMD_WRITE, base, wbuf.data(), wbuf.size(), NBD_REQ_FUA));
#ifdef __linux__
        // TRIM and WRITE_ZEROES both land on the VIRTUAL fallocate, because
        // IFile::trim / IFile::zero_range are plain methods that call it -- a
        // pass-through backend that forgot to forward it would answer ENOSYS
        // here, which nbd reports to the client as NBD_ENOTSUP. Offsets are
        // inside this client's own region, disjoint from the pair above.
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
