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

#pragma once

#include <photon/common/object.h>
#include <photon/common/estring.h>
#include <photon/common/callback.h>
#include <photon/net/socket.h>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace photon {
namespace net {

enum class SecurityRole {
    Client = 1,
    Server = 2,
};

enum class VerifyMode : int {
    NONE = 0x00,                  // SSL_VERIFY_NONE
    PEER = 0x01,                  // SSL_VERIFY_PEER
    FAIL_IF_NO_PEER_CERT = 0x02,  // SSL_VERIFY_IF_NO_PEER_CERT
    CLIENT_ONCE = 0x04,           // SSL_VERIFY_CLIENT_ONCE
};

/**
 * @brief TLSContext managers TLS key and cert
 * These parameters is able to set after created
 */
class TLSContext : public Object {
public:
    virtual int set_pass_phrase(const char* pass) = 0;
    virtual int set_cert(const char* cert_str) = 0;
    virtual int set_pkey(const char* key_str, const char* passphrase) = 0;
    virtual int set_verify_mode(VerifyMode mode = VerifyMode::NONE) = 0;
    // set client-side alpn protos in proto-list format
    virtual int set_alpn_protos(const std::vector<estring_view>& protos) = 0;
    // set server-side callback to choose proto
    // return value must be one string_view of the vector
    virtual int set_alpn_select_cb(
        Delegate<estring_view, const std::vector<estring_view>&>) = 0;
    virtual int set_ca_cert(const char* ca_cert_str) = 0;
    virtual int set_ca_file(const char* ca_file, const char* ca_path = nullptr) = 0;
    // Enable or disable binding the peer certificate to the hostname passed to
    // tls_stream_set_hostname(). Enabled by default; the analogue of curl's
    // CURLOPT_SSL_VERIFYHOST. Disabling it downgrades tls_stream_set_hostname()
    // to SNI only, which accepts any certificate a trusted CA has signed,
    // whoever it was issued to.
    virtual int set_verify_hostname(bool enable = true) = 0;
    // Reports the verify mode actually in effect, which is not necessarily the
    // last value passed to set_verify_mode(): set_ca_cert() and set_ca_file()
    // turn VerifyMode::PEER on as a side effect.
    virtual VerifyMode get_verify_mode() = 0;
};

enum class TLSVersion{
    SSL23,
    TLS11,
    TLS12,
};

/**
 * @brief Create a tls context, contains cert and private key infomation.
 *
 * @param cert_str certificate in string format
 * @param key_str private key in string format
 * @param passphrase passphrase for private key
 * @return TLSContext* context object pointer
 */
TLSContext* new_tls_context(const char* cert_str = nullptr,
                            const char* key_str = nullptr,
                            const char* passphrase = nullptr,
                            TLSVersion version = TLSVersion::TLS12);

/**
 * @brief Create socket stream on TLS.
 *
 * @param ctx TLS context. Context lifetime is inrelevant to Stream, user should
 *            keep it accessable during whole life time
 * @param base base socket, as underlay socket using for data transport
 * @param role should act as client or server during TLS handshake
 * @param ownership if new socket stream owns base socket.
 * @return ISocketStream*
 */
ISocketStream* new_tls_stream(TLSContext* ctx, ISocketStream* base,
                              SecurityRole role, bool ownership = false);
/**
 * @brief Create socket server on TLS. as a client socket factory.
 *
 * @param ctx TLS context. Context lifetime is inrelevant to Stream, user should
 *            keep it accessable during whole life time.
 * @param base base socket, as underlay socket using for data transport.
 * @param ownership if new socket stream owns base socket.
 * @return ISocketServer* server factory
 */
ISocketServer* new_tls_server(TLSContext* ctx, ISocketServer* base,
                              bool ownership = false);

/**
 * @brief Create socket client on TLS. as a client socket factory.
 *
 * @param ctx TLS context. Context lifetime is inrelevant to Stream, user should
 *            keep it accessable during whole life time.
 * @param base base socket, as underlay socket using for data transport.
 * @param ownership if new socket stream owns base socket.
 * @return ISocketClient* client factory
 */
ISocketClient* new_tls_client(TLSContext* ctx, ISocketClient* base,
                              bool ownership = false);

/**
 * @brief Set the hostname a client expects to be talking to.
 *
 * Sends `hostname` as SNI, and binds the peer certificate to it, so that a
 * certificate signed by a trusted CA but issued to some *other* name is
 * rejected. Both a chain check and this name check are needed; a chain check
 * alone only proves the certificate is genuine, not that it belongs to the peer
 * you asked for.
 *
 * Must be called before the first read/write on the stream, as the client
 * handshake is deferred until then.
 *
 * The name check is performed by OpenSSL during chain verification, so it
 * requires VerifyMode::PEER. Requesting a hostname on a context left at
 * VerifyMode::NONE is refused with EINVAL rather than silently ignored. IP
 * literals are matched against iPAddress SANs, hostnames against dNSName SANs
 * (falling back to the subject CN when the certificate carries no SAN).
 * A wildcard matches a single label, in the leftmost position only, and may not
 * cover part of a label (www*.example.com is rejected).
 *
 * @param stream a client-side TLS stream
 * @param hostname the expected hostname, or an IPv4/IPv6 literal
 * @return 0 on success, -1 on error (errno set). A failure leaves the stream
 *         unverified; callers should close it rather than continue.
 */
int tls_stream_set_hostname(ISocketStream* stream, const char* hostname);

/**
 * @brief Send `hostname` as SNI, without checking the certificate against it.
 *
 * The opt-out from tls_stream_set_hostname()'s name check, for callers that
 * verify the peer identity by other means. SNI only tells the server which
 * certificate to send; it places no constraint on the one that comes back.
 *
 * @return 0 on success, -1 on error (errno set)
 */
int tls_stream_set_sni(ISocketStream* stream, const char* hostname);

estring_view tls_stream_get_alpn_selected(ISocketStream* stream);

}  // namespace net
}  // namespace photon
