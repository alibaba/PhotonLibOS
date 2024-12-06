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

#include <photon/io/signal.h>
#include <photon/io/fd-events.h>
#include <photon/common/utility.h>
#include <photon/common/alog.h>
#include <photon/thread/thread.h>

#include <csignal>
#include <gtest/gtest.h>
#include <gflags/gflags.h>

using namespace photon;
using namespace std;

bool flag = false;
int sigact = 0;

void handler(int signal) {
    LOG_DEBUG(VALUE(signal));
    sigact = 1;
    flag = true;
}

void handler2(int signal, siginfo_t *info, void* context) {
    LOG_DEBUG(VALUE(signal));
    LOG_DEBUG(VALUE(info));
    LOG_DEBUG(VALUE(info->si_code));
    LOG_DEBUG(VALUE(info->si_errno));
    LOG_DEBUG(VALUE(info->si_signo));
    LOG_DEBUG(VALUE(context));
    sigact = 2;
    flag = true;
}

void prepare() {
    sigact = 0;
    flag = false;
}

TEST(SignalFD, basic) {
    auto p = fork();
    if (p==0) {
        exit(0);
    }
    LOG_INFO("Set SIGUSR1 handler to handler(1)");
    auto ret = sync_signal(SIGUSR1, &handler);
    EXPECT_EQ(nullptr, ret);
    LOG_INFO("Set again");
    ret = sync_signal(SIGUSR1, &handler);
    EXPECT_EQ(&handler, ret);

    LOG_INFO("Set SIGUSR2 handler to sigaction handler(3)");
    struct sigaction act;
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = &handler2;
    int retv = sync_sigaction(SIGUSR2, &act, nullptr);
    EXPECT_EQ(0, retv);

    prepare();
    LOG_INFO("Send SIGUSR1 to myself");
    kill(getpid(), SIGUSR1);
    while (!flag) {
        thread_usleep(1000);
    }
    EXPECT_EQ(1, sigact);

    prepare();
    LOG_INFO("Send SIGUSR2 to myself");
    kill(getpid(), SIGUSR2);
    while (!flag) {
        thread_usleep(1000);
    }
    EXPECT_EQ(2, sigact);

    // set back to handle(1)
    struct sigaction bact;
    bact.sa_flags = 0;
    bact.sa_handler = &handler;
    retv = sync_sigaction(SIGUSR2, &bact, &act);
    EXPECT_EQ(0, retv);

    prepare();
    LOG_INFO("Send SIGUSR2 to myself");
    kill(getpid(), SIGUSR2);
    while (!flag) {
        thread_usleep(1000);
    }
    EXPECT_EQ(1, sigact);
}

TEST(SignalFD, blockall) {
    block_all_signal();
    kill(getpid(), SIGPIPE);
    // yield out
    thread_usleep(1000UL);
    // once signal have not delievered, test will be terminated
    EXPECT_TRUE(true);
}

int main(int argc, char** arg)
{
    LOG_INFO("Set native signal handler");
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
    ::testing::InitGoogleTest(&argc, arg);
    gflags::ParseCommandLineFlags(&argc, &arg, true);
    LOG_DEBUG("test result:`",RUN_ALL_TESTS());
    return 0;
}
