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

#include <gtest/gtest.h>

#include <photon/net/socket.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>
#include <photon/common/utility.h>
#include <photon/common/alog-stdstring.h>
#include <photon/net/security-context/tls-stream.h>

using namespace photon;

static const char cert_str[] = R"PEMSTR(
-----BEGIN CERTIFICATE-----
MIIDGTCCAgGgAwIBAgIJAJTRQod8eLVYMA0GCSqGSIb3DQEBCwUAMCMxEjAQBgNV
BAoMCXQtc3RvcmFnZTENMAsGA1UECwwEZGFkaTAeFw0yMDA1MjUxMDI1NDZaFw0y
MzA1MjUxMDI1NDZaMCMxEjAQBgNVBAoMCXQtc3RvcmFnZTENMAsGA1UECwwEZGFk
aTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAMjojRFNr6rTWj3AlgQW
oTUKj/A2rXdfvCMVrJtsQLdir1FWqrvia2FrGabvCGqpSOmDVsNXM01putjHlq+R
dFZOG3Ye7WE0advSB3/3POauLt4wMs3LgOV53AkX4YWgrdZPVyfiG216aQQd9cZM
NZ13evqBn7diclgFqVM+Wt0k6AsN2Xns36x6SZPIY9mAF68QTf8aOMlTbC5UVQ3S
T/H9H9nIVMwSJ/DUADrdTejNYRnCzEri0FNHM39kB8LJrgRflyMGQeXgVRwQ16eP
MCR1N2NdSJz5lmXvegBjzS2P714vI0J33gI5UbJfDWVFXsV1R5E0fMe7yrNdOqv2
q10CAwEAAaNQME4wHQYDVR0OBBYEFDfkPK7Nj/WMnZZN9e6+Dt3tbZB6MB8GA1Ud
IwQYMBaAFDfkPK7Nj/WMnZZN9e6+Dt3tbZB6MAwGA1UdEwQFMAMBAf8wDQYJKoZI
hvcNAQELBQADggEBAH9GWMoLrZLjDk/OHOuXy/gbOFnOvJq78x+8F5hSaemDRR1l
gNKMm4GkXRaYj3YoMHn+kSEgpDguVa/zCo0k5ZEj0g9ZriAyQVLf8nR1hRfoP6CZ
YQMTEM6RYyweXzoJFYsHlnRWU2uTSK+ZLN0qdg4ST8REBpaexQ2t6zk/DaZpW8lp
nG6Zo1KkJbYp3vfpc03Kez1do/QkTDRB9pm14qz7MTQ3mzKxhSG+HJLvUQARsqHQ
dhHsVbXV15G/6PsqE0ToTr3wFBqoIgZbvEedqrt7CkQc95qnLcXkURg4tqZ3creU
g9t6WMtBxl9lFqzZZWJSwXhTGuFKiyAiQ6sBit0=
-----END CERTIFICATE-----
)PEMSTR";

static const char key_str[] = R"PEMSTR(
-----BEGIN ENCRYPTED PRIVATE KEY-----
MIIFDjBABgkqhkiG9w0BBQ0wMzAbBgkqhkiG9w0BBQwwDgQI9rxFFVP8ySoCAggA
MBQGCCqGSIb3DQMHBAiKyInYU4ROGwSCBMgcgUP7WYTwieC9jki6aeaRwrbUdqX3
/GGf8A6hM0o74rL8HlBU1IRv9n/9pY3Bf/Ll2qjFpqs8qq5xLkCgHaPIgwcsr26b
NlRJWt9c17qoFkn8brCWzpafeao9T0gxBprERL9N7uFBAbDSHOix7MB5BUJipuXO
uZyC2mvbTN5bzeR+A2z+SgNKLUnxwuEhalLhOhKtV9xKQX47E50oMpBY1EoKDeyJ
3ierZK7XvUyxAjxYeZ+g4qL9zKmL4Bo3SIs0Q3F3Oj8eYDfsLFRD1St65BCymFft
hkGNb7ZS1Sfx04pGKvWvR2ghPSpJ1ac46FTeGAF7Ch7/U67+czLyl+fdSpqe8oWP
tla5MwmKOO7jruCbxqNQ3NAbIu13j5lEPcsK/yimp5QQ1CGfLrONEuzRnFYMjN8Q
3qpkeJmhwGF6W9JJ1KgMMlyvGJRAjisOz8BqNbHq2sjOmaWAkAD+UI7tztPgxTUf
tT57hchxTQRPrcoB6E4T/gQXNRzvBJWbY0YnlLsgPYSChdKdQ8JNtJDoqYWvCyNx
YngKKet+z5VtgVb9oHor+wkjgO71zsvpQbKh4rzpwGsHwXHiYY4Bzf/fnyLj7gFl
eSPzb/M3cxcqbP4VA4iSs5DJ/y0xRxVWFlL8vCwmKX/3wzlGJXLcjwb37kGqMWlh
BZylQI3Y5Auv50hyaL8dZvhnALKoPtQSomq/ajtBILx/UEmXhLiXIgJmJ4fqdn0N
nd0S+nBr1s1Y4UjoihT98QHBIZkSFBHpojWssGd39IWaYVA0sCs+aIhZMfMtnEjf
05KgFZZNfpk8ANMsLlVDQZKg+WTPldRIdD5SVS/uTH82qWx3JDu1plKwpnsFyZMt
49vk8EBkdE0eFdtcRoB7lfl4DcIQut1zLye/3AY3mYZ8yWCF5wUjM7HJL7P9gx9I
u0bcWva1XyMsnED+fhXfRItEsy8iz0wQT9sjZlZ73ZAReE4k0PU1J1b0yPbDDy5W
xwezBdNTw8fF7vCx4HF47DfoxCZ9hI1ZEE/OyDhMvURBHP4DM4+RKknsxAKVmUk1
Smlu+jB6CwiAT0lIyl4y25NTYeta+3YOf+7DPTfGB1cfqLUyLQHngbgLSi4PcNr2
Yx3Y2dQ6s5KrOLdjwkM1MHRxQupsOWerxMxzCGhdBSZyUe/wGib/W29YvsVJB25V
UIueRsoBR/EF5F2lWtChkkgZhHjjx0a9v94HtauQFE/tcPT1hiapuV53W0eQBeYt
6Ofo7flnu7NEp5ZPfBVRplL/S7R3qXOiymUJKDSGK5JF6gCB+R3C83J6dCfpOJvA
v0KDae0QqFPQa+kAUdRpNCkgsSE+Pga4EG+1SFJO51nBQoc0c0GTM2UP9bUB7H6n
o+wc8fMgfrQx3lqS0gFXb0TTANRL0CqpOKRkveGoS5wjxcbXLvzKdQoVSFkPECpy
40s5Xn6tFb4XSv0viD2G0tztOkqhOZWGQObV7CUzhMCrsuAFG2kP4w/fKwhfe/zG
T48wYg2jgps8p6eGvuM94APKdeuDTTxdqF+QGLn7htXBxDWZCnbb0k7LPPs1ZSOp
g3DuxnLgkoimHT8MNAooUo3KpPM6PEYP3GStF8JfiNfRCpe4GwYjIM9yJ+NzBZxJ
Lvg=
-----END ENCRYPTED PRIVATE KEY-----
)PEMSTR";

static const char passphrase_str[] = "Just4Test";

photon::semaphore sem(0);

int handler(void* arg, net::ISocketStream* stream) {
    auto* ctx = (net::TLSContext*)arg;
    char buf[6];
    char buffer[1048576];
    auto ss = net::new_tls_stream(ctx, stream,
                                       net::SecurityRole::Server, false);
    DEFER(delete ss);
    LOG_INFO("BEFORE READ");
    auto ret = ss->read(buf, 6);
    LOG_INFO("AFTER READ");
    EXPECT_EQ(6, ret);
    LOG_INFO(VALUE(buf));
    LOG_INFO("BEFORE WRITE");
    ss->write(buffer, 1048576);
    LOG_INFO("AFTER WRITE");
    sem.signal(1);
    return 0;
}

void client_test(net::ISocketStream* stream, net::TLSContext* ctx) {
    auto ss = net::new_tls_stream(ctx, stream,
                                       net::SecurityRole::Client, false);
    DEFER(delete ss);
    char buf[] = "Hello";
    auto ret = ss->write(buf, 6);
    EXPECT_EQ(6, ret);
    char b[4096];
    size_t rx = 0;
    for (int i = 0; i < 256; i++) {
        rx += ss->recv(b, 4096);
    }
    EXPECT_EQ(1048576UL, rx);
    sem.wait(1);
}

TEST(basic, test) {
    auto ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    DEFER(net::delete_tls_context(ctx));
    DEFER(photon::wait_all());
    auto server = net::new_tcp_socket_server();
    DEFER(delete server);
    auto client = net::new_tcp_socket_client();
    DEFER(delete client);
    ASSERT_EQ(0, server->bind(0, net::IPAddr("127.0.0.1")));
    ASSERT_EQ(0, server->listen());
    auto ep = server->getsockname();
    LOG_INFO(VALUE(ep));
    ASSERT_EQ(0, server->start_loop(false));
    photon::thread_yield();
    server->set_handler({handler, ctx});
    auto stream = client->connect(ep);
    ASSERT_NE(nullptr, stream);
    DEFER(delete stream);

    client_test(stream, ctx);
}

TEST(basic, uds) {
    auto ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    DEFER(net::delete_tls_context(ctx));
    DEFER(photon::wait_all());
    auto server = net::new_uds_server(true);
    DEFER(delete server);
    auto client = net::new_uds_client();
    DEFER(delete client);
    auto fn = "/tmp/uds-tls-test-" + std::to_string(::getpid()) + ".sock";
    ASSERT_EQ(0, server->bind(fn.c_str()));
    ASSERT_EQ(0, server->listen());
    ASSERT_EQ(0, server->start_loop(false));
    photon::thread_yield();
    server->set_handler({handler, ctx});
    auto stream = client->connect(fn.c_str());
    ASSERT_NE(nullptr, stream);
    DEFER(delete stream);

    client_test(stream, ctx);
}

int s_handler(void*, net::ISocketStream* stream) {
    char buf[6];
    char buffer[1048576];
    LOG_INFO("BEFORE READ");
    auto ret = stream->read(buf, 6);
    LOG_INFO("AFTER READ");
    EXPECT_EQ(6, ret);
    LOG_INFO(VALUE(buf));
    LOG_INFO("BEFORE WRITE");
    stream->write(buffer, 1048576);
    LOG_INFO("AFTER WRITE");
    sem.signal(1);
    return 0;
}

void s_client_test(net::ISocketStream* stream) {
    char buf[] = "Hello";
    auto ret = stream->write(buf, 6);
    EXPECT_EQ(6, ret);
    char b[4096];
    size_t rx = 0;
    for (int i = 0; i < 256; i++) {
        rx += stream->recv(b, 4096);
    }
    EXPECT_EQ(1048576UL, rx);
    sem.wait(1);
}

TEST(cs, test) {
    auto ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    DEFER(net::delete_tls_context(ctx));
    DEFER(photon::wait_all());
    auto server =
        net::new_tls_server(ctx, net::new_tcp_socket_server(), true);
    DEFER(delete server);
    auto client =
        net::new_tls_client(ctx, net::new_tcp_socket_client(), true);
    DEFER(delete client);
    ASSERT_EQ(0, server->bind(0, net::IPAddr("127.0.0.1")));
    ASSERT_EQ(0, server->listen());
    auto ep = server->getsockname();
    LOG_INFO(VALUE(ep));
    ASSERT_EQ(0, server->start_loop(false));
    photon::thread_yield();
    server->set_handler({s_handler, ctx});
    auto stream = client->connect(ep);
    ASSERT_NE(nullptr, stream);
    DEFER(delete stream);

    s_client_test(stream);
}

TEST(cs, uds) {
    auto ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    DEFER(net::delete_tls_context(ctx));
    DEFER(photon::wait_all());
    auto server =
        net::new_tls_server(ctx, net::new_uds_server(true), true);
    DEFER(delete server);
    auto client = net::new_tls_client(ctx, net::new_uds_client(), true);
    DEFER(delete client);
    auto fn = "/tmp/uds-tls-test-" + std::to_string(::getpid()) + ".sock";
    ASSERT_EQ(0, server->bind(fn.c_str()));
    ASSERT_EQ(0, server->listen());
    ASSERT_EQ(0, server->start_loop(false));
    photon::thread_yield();
    server->set_handler({s_handler, ctx});
    auto stream = client->connect(fn.c_str());
    ASSERT_NE(nullptr, stream);
    DEFER(delete stream);

    s_client_test(stream);
}

int main(int argc, char** arg) {
    photon::thread_init();
    DEFER(photon::thread_fini());
    photon::fd_events_init();
    DEFER(photon::fd_events_fini());
    ::testing::InitGoogleTest(&argc, arg);
    return RUN_ALL_TESTS();
}
