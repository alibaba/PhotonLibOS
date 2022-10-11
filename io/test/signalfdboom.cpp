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

#define protected public
#include <photon/io/signal.h>
#include <photon/io/fd-events.h>
#include <photon/common/alog.h>
#undef protected

#include <csignal>
#include <gtest/gtest.h>
#include <gflags/gflags.h>

using namespace photon;
using namespace std;

static int g_count = 0;

void handler(int signal) {
    LOG_DEBUG(VALUE(signal));
    g_count++;
}

TEST(Boom, boom) {
    sync_signal(SIGCHLD, handler);
    sync_signal(SIGPROF, handler);

    for (int i=0;i < 100;i++) {
        kill(getpid(), SIGCHLD);
        kill(getpid(), SIGPROF);
    }

    thread_usleep(1000*1000);
    EXPECT_EQ(2, g_count);
}

int main(int argc, char** arg)
{
    LOG_INFO("Set native signal handler");
    vcpu_init();
    DEFER({vcpu_fini();});
    auto ret = fd_events_init();
    if (ret != 0)
        LOG_ERROR_RETURN(0, -1, "failed to init fdevents");
    DEFER({fd_events_fini();});
    ret = sync_signal_init();
    if (ret != 0)
        LOG_ERROR_RETURN(0, -1, "failed to init signalfd");
    DEFER({sync_signal_fini();});
    ::testing::InitGoogleTest(&argc, arg);
    google::ParseCommandLineFlags(&argc, &arg, true);
    LOG_DEBUG("test result:`",RUN_ALL_TESTS());
    return 0;
}
