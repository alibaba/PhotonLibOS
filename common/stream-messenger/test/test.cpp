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

#include <photon/common/stream-messenger/messenger.h>
#include <memory>
#include <algorithm>
#include <string>
#include <gtest/gtest.h>
#include <photon/thread/thread.h>
#include <photon/common/memory-stream/memory-stream.h>
#include <photon/common/utility.h>
#include <photon/common/alog.h>
using namespace std;
using namespace photon;
using namespace StreamMessenger;

char STR[] = "abcdefghijklmnopqrstuvwxyz";

void* echo_server(void* mc_)
{
    auto mc = (IMessageChannel*)mc_;

    IOVector msg;
    size_t blksz = 2;
    while(true)
    {
        auto ret = mc->recv(msg);
        if (ret == 0)
        {
            LOG_DEBUG("zero-lengthed message recvd, quit");
            break;
        }
        if (ret < 0)
        {
            if (errno == ENOBUFS)
            {
                blksz *= 2;
                msg.push_back(blksz);
                LOG_DEBUG("adding ` bytes to the msg iovector", blksz);
                continue;
            }

            LOG_ERROR_RETURN(0, nullptr, "failed to recv msg");
        }

        auto ret2 = mc->send(msg);
        EXPECT_EQ(ret2, ret);
    }
    LOG_DEBUG("exit");
    return nullptr;
}

TEST(StreamMessenger, DISABLED_normalTest)
{
    auto ds = new_duplex_memory_stream(64);
    DEFER(delete ds);

    auto mca = new_messenger(ds->endpoint_a);
    DEFER(delete mca);
    thread_create(&echo_server, mca);

    auto mcb = new_messenger(ds->endpoint_b);
    DEFER(delete mcb);

    auto size = LEN(STR) - 1;
    auto ret = mcb->send(STR, size);
    EXPECT_EQ(ret, size);
    LOG_DEBUG(ret, " bytes sent");
    char buf[size];
    auto ret2 = mcb->recv(buf, sizeof(buf));
    EXPECT_EQ(ret2, sizeof(buf));
    EXPECT_EQ(memcmp(buf, STR, size), 0);
    LOG_DEBUG(ret, " bytes recvd and verified");

    mcb->send(nullptr, 0);
    thread_yield();
    LOG_DEBUG("exit");
}

int main(int argc, char **argv)
{
    log_output_level = ALOG_DEBUG;
    ::testing::InitGoogleTest(&argc, argv);
    photon::vcpu_init();
    DEFER(photon::vcpu_fini());
    auto ret = RUN_ALL_TESTS();
    return 0;
}
