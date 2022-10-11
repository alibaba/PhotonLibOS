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

#include <photon/common/alog-stdstring.h>
#include <photon/net/socket.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>
#include <photon/common/utility.h>
#include <photon/net/security-context/sasl-stream.h>
#include <photon/common/string_view.h>

using namespace photon;

#define TOKEN_SIZE (8 * 1024)

photon::semaphore sem(0);
size_t testId = 0;

struct sasltv {
    std::string_view mech;
    bool client_first;
    const char *password;
    const char *authzid;
    const char *authid;
    const char *service;
    const char *hostname;
    const char *qop;
    const char *anonymous;
    const char *passcode;
    const char *suggestpin;
    const char *pin;
    int securidrc;
};

static struct sasltv sasltvs[] = {
    {"DIGEST-MD5", true, "password", "authzid", "authid", "service", "0", "qop-int"},
    {"CRAM-MD5", false, "password", nullptr, "authid"},
    {"SCRAM-SHA-1", true, "password", nullptr, "authid"},
    {"SECURID", true, nullptr, "authzid", "authid", nullptr, nullptr, nullptr, nullptr, "4711",
     "23", "42", GSASL_SECURID_SERVER_NEED_NEW_PIN},
    {"EXTERNAL", true, nullptr, "authzid"},
    {"PLAIN", true, "password", "authzid", "authid"},
    {"LOGIN", false, "password", nullptr, "authid"},
    {"ANONYMOUS", true, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "test@test.com"},
    {"SAML20", true, nullptr, "authzid", "https://saml.example.org/"},
    {"OPENID20", true, nullptr, "authzid", "https://user.example.org/"},
    // GS-KRB5 need kinit in the environment, so we disable this test by default.
    // {"GS2-KRB5", true, nullptr, "test@EXAMPLE.COM", nullptr, "server", "j63a04458.sqa.eu95"}
};

static int client_callback(void *, Gsasl *ctx, Gsasl_session *sctx, Gsasl_property prop) {
    int rc = GSASL_NO_CALLBACK;

    switch (prop) {
    case GSASL_PASSWORD:
        gsasl_property_set(sctx, prop, sasltvs[testId].password);
        rc = GSASL_OK;
        break;

    case GSASL_AUTHID:
        gsasl_property_set(sctx, prop, sasltvs[testId].authid);
        rc = GSASL_OK;
        break;

    case GSASL_AUTHZID:
        gsasl_property_set(sctx, prop, sasltvs[testId].authzid);
        rc = GSASL_OK;
        break;

    case GSASL_SERVICE:
        gsasl_property_set(sctx, prop, sasltvs[testId].service);
        rc = GSASL_OK;
        break;

    case GSASL_REALM:
        rc = GSASL_OK;
        break;

    case GSASL_HOSTNAME:
        gsasl_property_set(sctx, prop, sasltvs[testId].hostname);
        rc = GSASL_OK;
        break;

    case GSASL_QOP:
        gsasl_property_set(sctx, prop, sasltvs[testId].qop);
        rc = GSASL_OK;
        break;

    case GSASL_ANONYMOUS_TOKEN:
        gsasl_property_set(sctx, prop, sasltvs[testId].anonymous);
        rc = GSASL_OK;
        break;

    case GSASL_CB_TLS_UNIQUE:
        break;

    case GSASL_SCRAM_SALTED_PASSWORD:
        break;

    case GSASL_PASSCODE:
        gsasl_property_set(sctx, prop, sasltvs[testId].passcode);
        rc = GSASL_OK;
        break;

    case GSASL_PIN: {
        gsasl_property_set(sctx, prop, sasltvs[testId].pin);
        rc = GSASL_OK;
    } break;

    case GSASL_SAML20_IDP_IDENTIFIER:
        gsasl_property_set(sctx, prop, sasltvs[testId].authid);
        rc = GSASL_OK;
        break;

    case GSASL_SAML20_AUTHENTICATE_IN_BROWSER:
        rc = GSASL_OK;
        break;

    case GSASL_OPENID20_AUTHENTICATE_IN_BROWSER:
        rc = GSASL_OK;
        break;

    default:
        LOG_DEBUG("Client: Unknown callback property `", prop);
        break;
    }

    return rc;
}

static int server_callback(void *, Gsasl *ctx, Gsasl_session *sctx, Gsasl_property prop) {
    int rc = GSASL_NO_CALLBACK;

    switch (prop) {
    case GSASL_PASSWORD:
        gsasl_property_set(sctx, prop, sasltvs[testId].password);
        rc = GSASL_OK;
        break;

    case GSASL_AUTHID:
        gsasl_property_set(sctx, prop, sasltvs[testId].authid);
        rc = GSASL_OK;
        break;

    case GSASL_AUTHZID:
        gsasl_property_set(sctx, prop, sasltvs[testId].authzid);
        rc = GSASL_OK;
        break;

    case GSASL_SERVICE:
        gsasl_property_set(sctx, prop, sasltvs[testId].service);
        rc = GSASL_OK;
        break;

    case GSASL_REALM:
        rc = GSASL_OK;
        break;

    case GSASL_HOSTNAME:
        gsasl_property_set(sctx, prop, sasltvs[testId].hostname);
        rc = GSASL_OK;
        break;

    case GSASL_QOPS:
        gsasl_property_set(sctx, prop, sasltvs[testId].qop);
        rc = GSASL_OK;
        break;

    case GSASL_CB_TLS_UNIQUE:
        break;

    case GSASL_SCRAM_ITER:
        break;

    case GSASL_SCRAM_SALT:
        break;

    case GSASL_VALIDATE_EXTERNAL: {
        LOG_INFO("Server validation in EXTERNAL mechanism");
        const char *authid = gsasl_property_fast(sctx, GSASL_AUTHZID);
        if (strcmp(authid, sasltvs[testId].authzid) == 0)
            rc = GSASL_OK;
        else
            rc = GSASL_AUTHENTICATION_ERROR;
    } break;

    case GSASL_VALIDATE_SIMPLE: {
        LOG_INFO("Server validation in SIMPLE mechanism");
        const char *authid = gsasl_property_fast(sctx, GSASL_AUTHID);
        if (strcmp(authid, sasltvs[testId].authid) == 0)
            rc = GSASL_OK;
        else
            rc = GSASL_AUTHENTICATION_ERROR;
        const char *passwd = gsasl_property_fast(sctx, GSASL_PASSWORD);
        if (strcmp(passwd, sasltvs[testId].password) == 0)
            rc = GSASL_OK;
        else
            rc = GSASL_AUTHENTICATION_ERROR;
    } break;

    case GSASL_VALIDATE_ANONYMOUS:
        LOG_INFO("Server validation in ANONYMOUS mechanism");
        if (strcmp(sasltvs[testId].anonymous, gsasl_property_fast(sctx, GSASL_ANONYMOUS_TOKEN)) ==
            0)
            rc = GSASL_OK;
        else
            rc = GSASL_AUTHENTICATION_ERROR;
        break;

    case GSASL_VALIDATE_GSSAPI: {
        LOG_INFO("Server validation in GSSAPI mechanism");
        const char *client_name = gsasl_property_fast(sctx, GSASL_GSSAPI_DISPLAY_NAME);
        const char *authzid = gsasl_property_fast(sctx, GSASL_AUTHZID);
        if (strcmp(client_name, sasltvs[testId].authzid) == 0 &&
            strcmp(authzid, sasltvs[testId].authzid) == 0)
            rc = GSASL_OK;
        else
            rc = GSASL_AUTHENTICATION_ERROR;
    } break;

    case GSASL_VALIDATE_SECURID: {
        LOG_INFO("Server validation in SECURID mechanism");
        const char *passcode = gsasl_property_fast(sctx, GSASL_PASSCODE);
        const char *pin = gsasl_property_fast(sctx, GSASL_PIN);

        if (strcmp(passcode, sasltvs[testId].passcode) != 0)
            return GSASL_AUTHENTICATION_ERROR;

        if (sasltvs[testId].securidrc == GSASL_SECURID_SERVER_NEED_NEW_PIN) {
            rc = sasltvs[testId].securidrc;
            sasltvs[testId].securidrc = GSASL_OK;

            if (sasltvs[testId].suggestpin) {
                gsasl_property_set(sctx, GSASL_SUGGESTED_PIN, sasltvs[testId].suggestpin);
            }
        } else if (sasltvs[testId].securidrc == GSASL_SECURID_SERVER_NEED_ADDITIONAL_PASSCODE) {
            rc = sasltvs[testId].securidrc;
            sasltvs[testId].securidrc = GSASL_OK;
        } else {
            rc = sasltvs[testId].securidrc;
            sasltvs[testId].securidrc = GSASL_SECURID_SERVER_NEED_NEW_PIN; // restore it for test

            if (pin && sasltvs[testId].pin && strcmp(pin, sasltvs[testId].pin) != 0)
                return GSASL_AUTHENTICATION_ERROR;

            if ((pin == NULL && sasltvs[testId].pin != NULL) ||
                (pin != NULL && sasltvs[testId].pin == NULL))
                return GSASL_AUTHENTICATION_ERROR;
        }

    } break;

    case GSASL_SAML20_REDIRECT_URL:
        LOG_INFO("server got identity: `", gsasl_property_get(sctx, GSASL_SAML20_IDP_IDENTIFIER));
        gsasl_property_set(sctx, prop,
                           "https://saml.example.org/SAML/Browser?SAMLRequest=PHNhbWxwOkF1d");
        rc = GSASL_OK;
        break;

    case GSASL_VALIDATE_SAML20:
        LOG_INFO("Server validation in SAML20 mechanism");
        rc = GSASL_OK;
        break;

    case GSASL_OPENID20_REDIRECT_URL:
        gsasl_property_set(sctx, prop, "http://user.example/NONCE/?openid.foo=bar");
        rc = GSASL_OK;
        break;

    case GSASL_VALIDATE_OPENID20:
        LOG_INFO("Server validation in OPENID20 mechanism");
        rc = GSASL_OK;
        break;

    case GSASL_OPENID20_OUTCOME_DATA:
        rc = GSASL_OK;
        break;

    default:
        LOG_DEBUG("Server: Unknown callback property `", prop);
        break;
    }
    return rc;
}

static int auth_cb_send(void *data, Gsasl_session *session, net::ISocketStream *stream) {
    int rc = 0;
    ssize_t ret = 0;
    char *output = nullptr;
    char input[TOKEN_SIZE];
    size_t inputlen = 0, outputlen = 0;

    rc = gsasl_step(session, NULL, 0, &output, &outputlen);
    if (outputlen == 0 && sasltvs[testId].mech == "DIGEST-MD5") {
        output = (char *)malloc(10);
        memcpy(output, "DUMMY MSG", 10);
        outputlen = 10;
    }
    do {
        ret = stream->send(output, outputlen);
        LOG_DEBUG("` send ` `", sasltvs[testId].client_first ? "client" : "server", ret, output);
        gsasl_free(output);
        inputlen = stream->recv(input, TOKEN_SIZE);
        LOG_DEBUG("` recv ` `", sasltvs[testId].client_first ? "client" : "server", inputlen,
                  input);
        rc = gsasl_step(session, input, inputlen, &output, &outputlen);
        if (rc != GSASL_NEEDS_MORE && rc != GSASL_OK)
            LOG_ERROR_RETURN(0, -1, "gsasl_step error: `", gsasl_strerror(rc));
    } while (rc == GSASL_NEEDS_MORE);

    if (sasltvs[testId].mech == "SAML20" || sasltvs[testId].mech == "OPENID20" ||
        sasltvs[testId].mech == "SECURID") {
        ret = stream->send(output, outputlen);
        LOG_DEBUG("` send ` ` rc: `", sasltvs[testId].client_first ? "client" : "server", ret,
                  output, rc);
        inputlen = stream->recv(input, TOKEN_SIZE);
        LOG_DEBUG("` recv ` `", sasltvs[testId].client_first ? "client" : "server", inputlen,
                  input);
    }
    if (outputlen)
        gsasl_free(output);
    return rc;
}

static int auth_cb_recv(void *data, Gsasl_session *session, net::ISocketStream *stream) {
    int rc = 0;
    ssize_t ret = 0;
    char *output = nullptr;
    char input[TOKEN_SIZE];
    size_t inputlen = 0, outputlen = 0;

    do {
        inputlen = stream->recv(input, TOKEN_SIZE);
        LOG_DEBUG("` recv ` `", sasltvs[testId].client_first ? "server" : "client", inputlen,
                  input);
        rc = gsasl_step(session, input, inputlen, &output, &outputlen);
        if (rc != GSASL_NEEDS_MORE && rc != GSASL_OK)
            LOG_ERROR_RETURN(0, -1, "gsasl_step error: `", gsasl_strerror(rc));
        if (outputlen == 0) { // soket cannot send empty data, so we construct some dummy data.
            output = (char *)malloc(10);
            memcpy(output, "DUMMY MSG", 10);
            outputlen = 10;
        }
        ret = stream->send(output, outputlen);
        LOG_DEBUG("` send ` ` rc: `", sasltvs[testId].client_first ? "server" : "client", ret,
                  output, rc);
        gsasl_free(output);
    } while (rc == GSASL_NEEDS_MORE);
    photon::thread_usleep(1000);
    return rc;
}

int handler(void *, net::ISocketStream *stream) {
    auto auth_cb = sasltvs[testId].client_first ? net::Gsasl_auth_cb(nullptr, &auth_cb_recv)
                                                : net::Gsasl_auth_cb(nullptr, &auth_cb_send);
    auto prep_cb = net::Gsasl_prep_cb(nullptr, &server_callback);
    auto session = net::new_sasl_server_session(sasltvs[testId].mech.data(), auth_cb, prep_cb);
    DEFER(delete session);
    char buf[6];
    char buffer[1048576];
    auto ss = net::new_sasl_stream(session, stream, false);
    DEFER(delete ss);
    LOG_INFO("BEFORE READ");
    auto ret = ss->read(buf, 6);
    LOG_INFO("AFTER READ");
    EXPECT_EQ(6, ret);
    LOG_INFO(VALUE(buf));
    LOG_INFO("BEFORE WRITE");
    ret = ss->write(buffer, 1048576);
    LOG_INFO("write ret: ", ret);
    LOG_INFO("AFTER WRITE");
    sem.signal(1);
    return 0;
}

void client_test(net::ISocketStream *stream) {
    auto auth_cb = sasltvs[testId].client_first ? net::Gsasl_auth_cb(nullptr, &auth_cb_send)
                                                : net::Gsasl_auth_cb(nullptr, &auth_cb_recv);
    auto prep_cb = net::Gsasl_prep_cb(nullptr, &client_callback);
    auto session = net::new_sasl_client_session(sasltvs[testId].mech.data(), auth_cb, prep_cb);
    DEFER(delete session);
    auto ss = net::new_sasl_stream(session, stream, false);
    DEFER(delete ss);
    char buf[] = "Hello";
    auto ret = ss->write(buf, 6);
    EXPECT_EQ(6, ret);
    char b[4096];
    size_t rx = 0;
    for (int i = 0; i < 256; i++) {
        rx += ss->read(b, 4096);
    }
    EXPECT_EQ(1048576UL, rx);
    sem.wait(1);
}

TEST(basic, test) {
    for (testId = 0; testId < sizeof(sasltvs) / sizeof(sasltvs[0]); testId++) {
        LOG_INFO("------------mechnism: `-------------", sasltvs[testId].mech);
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
        server->set_handler({handler, nullptr});
        auto stream = client->connect(ep);
        ASSERT_NE(nullptr, stream);
        DEFER(delete stream);

        client_test(stream);
    }
}

TEST(basic, uds) {
    for (testId = 0; testId < sizeof(sasltvs) / sizeof(sasltvs[0]); testId++) {
        LOG_INFO("------------mechnism: `-------------", sasltvs[testId].mech);
        DEFER(photon::wait_all());
        auto server = net::new_uds_server(true);
        DEFER(delete server);
        auto client = net::new_uds_client();
        DEFER(delete client);
        auto fn = "/tmp/uds-sasl-test-" + std::to_string(::getpid()) + ".sock";
        ASSERT_EQ(0, server->bind(fn.c_str()));
        ASSERT_EQ(0, server->listen());
        ASSERT_EQ(0, server->start_loop(false));
        photon::thread_yield();
        server->set_handler({handler, nullptr});
        auto stream = client->connect(fn.c_str());
        ASSERT_NE(nullptr, stream);
        DEFER(delete stream);

        client_test(stream);
    }
}

int s_handler(void *, net::ISocketStream *stream) {
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

void s_client_test(net::ISocketStream *stream) {
    char buf[] = "Hello";
    auto ret = stream->write(buf, 6);
    EXPECT_EQ(6, ret);
    char b[4096];
    size_t rx = 0;
    for (int i = 0; i < 256; i++) {
        rx += stream->read(b, 4096);
    }
    EXPECT_EQ(1048576UL, rx);
    sem.wait(1);
}

TEST(cs, test) {
    for (testId = 0; testId < sizeof(sasltvs) / sizeof(sasltvs[0]); testId++) {
        LOG_INFO("------------mechnism: `-------------", sasltvs[testId].mech);
        auto c_auth_cb = sasltvs[testId].client_first
                             ? net::Gsasl_auth_cb(nullptr, &auth_cb_send)
                             : net::Gsasl_auth_cb(nullptr, &auth_cb_recv);
        auto c_prep_cb = net::Gsasl_prep_cb(nullptr, &client_callback);
        auto c_session =
            net::new_sasl_client_session(sasltvs[testId].mech.data(), c_auth_cb, c_prep_cb);
        DEFER(delete c_session);
        auto s_auth_cb = sasltvs[testId].client_first
                             ? net::Gsasl_auth_cb(nullptr, &auth_cb_recv)
                             : net::Gsasl_auth_cb(nullptr, &auth_cb_send);
        auto s_prep_cb = net::Gsasl_prep_cb(nullptr, &server_callback);
        auto s_session =
            net::new_sasl_server_session(sasltvs[testId].mech.data(), s_auth_cb, s_prep_cb);
        DEFER(delete s_session);
        DEFER(photon::wait_all());
        auto server = net::new_sasl_server(s_session, net::new_tcp_socket_server(), true);
        DEFER(delete server);
        auto client = net::new_sasl_client(c_session, net::new_tcp_socket_client(), true);
        DEFER(delete client);
        ASSERT_EQ(0, server->bind(0, net::IPAddr("127.0.0.1")));
        ASSERT_EQ(0, server->listen());
        auto ep = server->getsockname();
        LOG_INFO(VALUE(ep));
        ASSERT_EQ(0, server->start_loop(false));
        photon::thread_yield();
        server->set_handler({s_handler, nullptr});
        auto stream = client->connect(ep);
        ASSERT_NE(nullptr, stream);
        DEFER(delete stream);

        s_client_test(stream);
    }
}

TEST(cs, uds) {
    for (testId = 0; testId < sizeof(sasltvs) / sizeof(sasltvs[0]); testId++) {
        LOG_INFO("------------mechnism: `-------------", sasltvs[testId].mech);
        auto c_auth_cb = sasltvs[testId].client_first
                             ? net::Gsasl_auth_cb(nullptr, &auth_cb_send)
                             : net::Gsasl_auth_cb(nullptr, &auth_cb_recv);
        auto c_prep_cb = net::Gsasl_prep_cb(nullptr, &client_callback);
        auto c_session =
            net::new_sasl_client_session(sasltvs[testId].mech.data(), c_auth_cb, c_prep_cb);
        DEFER(delete c_session);
        auto s_auth_cb = sasltvs[testId].client_first
                             ? net::Gsasl_auth_cb(nullptr, &auth_cb_recv)
                             : net::Gsasl_auth_cb(nullptr, &auth_cb_send);
        auto s_prep_cb = net::Gsasl_prep_cb(nullptr, &server_callback);
        auto s_session =
            net::new_sasl_server_session(sasltvs[testId].mech.data(), s_auth_cb, s_prep_cb);
        DEFER(delete s_session);
        DEFER(photon::wait_all());
        auto server = net::new_sasl_server(s_session, net::new_uds_server(true), true);
        DEFER(delete server);
        auto client = net::new_sasl_client(c_session, net::new_uds_client(), true);
        DEFER(delete client);
        auto fn = "/tmp/uds-sasl-test-" + std::to_string(::getpid()) + ".sock";
        ASSERT_EQ(0, server->bind(fn.c_str()));
        ASSERT_EQ(0, server->listen());
        ASSERT_EQ(0, server->start_loop(false));
        photon::thread_yield();
        server->set_handler({s_handler, nullptr});
        auto stream = client->connect(fn.c_str());
        ASSERT_NE(nullptr, stream);
        DEFER(delete stream);

        s_client_test(stream);
    }
}

int main(int argc, char **arg) {
    photon::vcpu_init();
    DEFER(photon::vcpu_fini());
    photon::fd_events_init();
    DEFER(photon::fd_events_fini());
    ::testing::InitGoogleTest(&argc, arg);
    return RUN_ALL_TESTS();
}
