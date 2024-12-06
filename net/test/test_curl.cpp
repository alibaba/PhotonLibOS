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
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <photon/net/curl.h>
#include <photon/common/alog.h>
#include <photon/thread/thread11.h>
#include <photon/io/fd-events.h>

using namespace photon;

class StringStream {
    std::string s;

    public:
        std::string& str() {
            return s;
        }

        size_t write(void* c, size_t n) {
            LOG_DEBUG("CALL WRITE");
            s.append((char*)c, n);
            return n;
        }
};

TEST(cURL, feature) {
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_LIBCURL))
        FAIL();
    DEFER(photon::fini());

    std::unique_ptr<net::cURL> client(new net::cURL());
    std::unique_ptr<StringStream> buffer(new StringStream());
    client->set_redirect(10).set_verbose(true);
    // for (int i=0;i<2;i++) {
        client->GET("http://github.com", buffer.get());
    // }
    LOG_INFO(buffer->str().c_str());
    buffer.reset();
    client.reset();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
