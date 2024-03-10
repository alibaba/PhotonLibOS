#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/net/http/verb.h>
#include <photon/net/http/client.h>
#include <photon/net/security-context/tls-stream.h>

using namespace photon::net;

int main(int argc, char **argv) {
    set_log_output_level(0);
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    DEFER({photon::fini();});

    auto tls = new_tls_context();
    auto client = http::new_http_client(nullptr, tls);

    auto op = client->new_operation(http::Verb::GET, "https://debug.fly.dev");
    op->retry = 0;
    int res = op->call();

    printf("result: %d\n", res);
    exit(res);
}