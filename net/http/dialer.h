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

#include <cstdint>
#include <memory>
#include <vector>

#include <photon/common/callback.h>
#include <photon/common/estring.h>
#include <photon/common/object.h>
#include <photon/common/string_view.h>
#include <photon/net/http/headers.h>
#include <photon/net/socket.h>

namespace photon {
namespace net {

class Resolver;
class TLSContext;

namespace http {

// The complete connection intent for one HTTP request. The resolver lease is
// optional: an empty one selects the process-wide default resolver.
struct DialTarget {
    std::string_view host;
    uint16_t port = 0;
    bool secure = false;
    std::string_view uds_path;
    std::string_view proxy_host;
    uint16_t proxy_port = 0;
    bool proxy_secure = false;
    std::string_view proxy_auth;
    const HeadersBase* proxy_headers = nullptr;
    std::string_view proxy_pool_key;
    std::shared_ptr<Resolver> resolver;

    bool via_proxy() const { return !proxy_host.empty(); }
    bool need_tunnel() const { return via_proxy() && secure; }
};

struct ProxyAuth {
    CommonHeaders<4 * 1024 - 1> headers;
    estring pool_key;
};

using ProxyAuthenticator = Delegate<int, const DialTarget&, ProxyAuth&>;

class IDialer : public Object {
public:
    virtual ISocketStream* dial(const DialTarget& target,
                                uint64_t timeout = -1ULL) = 0;
};

enum class TLSLayer : uint8_t {
    PROXY,
    ORIGIN,
};

// The components below are bound to the vCPU on which they are created and
// must be used and destroyed there. `ownership` transfers ownership of the
// underlay to the returned decorator. A TLS context must outlive its dialer
// unless `context_ownership` is true; when layers share one context, only the
// outermost owning layer may take that ownership.
IDialer* new_transport_dialer(const std::vector<IPAddr>& bind_ips = {});
IDialer* new_tls_dialer(TLSContext* context, IDialer* underlay, TLSLayer layer,
                        bool ownership = false,
                        bool context_ownership = false);
IDialer* new_connect_tunnel_dialer(IDialer* underlay,
                                   bool ownership = false);
IDialer* new_pool_dialer(IDialer* underlay, bool ownership = false,
                         uint64_t expiration = -1ULL);

// Build the standard single-vCPU stack. If context is null, a default context
// is created and owned by the returned stack. Otherwise the caller owns the
// context and must keep it alive until the returned stack is destroyed.
IDialer* new_http_dialer(TLSContext* context = nullptr,
                         const std::vector<IPAddr>& bind_ips = {});

// Make a cross-vCPU dispatcher. Its factory is invoked lazily on each vCPU and
// may therefore run concurrently; its captured state must remain alive and be
// concurrency-safe. It must return a dialer owned by that vCPU. The dispatcher
// owns all results and destroys each one on the vCPU that created it. Returning
// nullptr fails that dial but is not cached, so a later dial retries the factory.
IDialer* new_vcpu_local_dialer(Delegate<IDialer*> factory);

} // namespace http
} // namespace net
} // namespace photon
