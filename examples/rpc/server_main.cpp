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

#include <gflags/gflags.h>
#include <photon/common/utility.h>
#include <photon/io/signal.h>
#include <photon/photon.h>

#include "server.h"

DEFINE_int32(port, 0, "Server listen port, 0(default) for random port");
std::unique_ptr<ExampleServer> rpcservice;

void handle_null(int) {}
void handle_term(int) { rpcservice.reset(); }

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    photon::init();
    DEFER(photon::fini());

    photon::sync_signal(SIGPIPE, &handle_null);
    photon::sync_signal(SIGTERM, &handle_term);
    photon::sync_signal(SIGINT, &handle_term);
    // start server

    // construct rpcservice
    rpcservice.reset(new ExampleServer());

    rpcservice->run(FLAGS_port);

    return 0;
}