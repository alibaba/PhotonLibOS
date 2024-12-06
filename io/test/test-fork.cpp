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

#include <unistd.h>
#include <sys/wait.h>
#include <gtest/gtest.h>
#include <thread>
#include <fcntl.h>

#include <photon/common/alog.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/io/fd-events.h>
#include <photon/io/signal.h>
#include <photon/net/curl.h>
#include <photon/fs/localfs.h>

bool exit_flag = false;
bool exit_normal = false;

void sigint_handler(int signal = SIGINT) {
    LOG_INFO("signal ` received, pid `", signal, getpid());
    exit_flag = true;
}

inline int check_process_exit_stat(int &statVal, int &pid) {
    if (WIFEXITED(statVal)) { // child exit normally
        LOG_INFO("process with pid ` finished with code `.", pid, WEXITSTATUS(statVal));
        return WEXITSTATUS(statVal);
    } else {
        if (WIFSIGNALED(statVal)) { // child terminated due to uncaptured signal
            LOG_INFO("process with pid ` terminated due to uncaptured signal `.", pid,
                     WTERMSIG(statVal));
        } else if (WIFSTOPPED(statVal)) { // child terminated unexpectedly
            LOG_INFO("process with pid ` terminated unexpectedly with signal `.", pid,
                     WSTOPSIG(statVal));
        } else {
            LOG_INFO("process with pid ` terminated abnormally.", pid);
        }
        return -1;
    }
}

void wait_process_end(pid_t pid) {
    if (pid > 0) {
        int statVal;
        if (waitpid(pid, &statVal, 0) > 0) {
            check_process_exit_stat(statVal, pid);
        } else {
            /// EINTR
            if (EINTR == errno) {
                LOG_INFO("process with pid ` waitpid is interrupted.", pid);
            } else {
                LOG_INFO("process with pid ` waitpid exception, strerror: `.", pid,
                         strerror(errno));
            }
        }
    }
}

void wait_process_end_no_hang(pid_t pid) {
    if (pid > 0) {
        int statVal;
        int retry = 100;
    again:
        if (waitpid(pid, &statVal, WNOHANG) <= 0) {
            if (retry--) {
                photon::thread_usleep(50 * 1000);
                goto again;
            } else {
                if (kill(pid, SIGKILL) == 0) {
                    LOG_WARN("force kill child process with pid `", pid);
                } else {
                    LOG_ERROR("force kill child process with pid ` error, errno:`:`", pid, errno,
                              strerror(errno));
                }
                wait_process_end(pid);
            }
        } else {
            if (check_process_exit_stat(statVal, pid) == 0) {
                exit_normal = true;
            }
        }
    }
}

int fork_child_process() {
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERRNO_RETURN(0, -1, "fork error");
        return -1;
    }

    if (pid == 0) {
        photon::block_all_signal();
        photon::sync_signal(SIGTERM, &sigint_handler);

        LOG_INFO("child hello, pid `", getpid());
        while (!exit_flag) {
            photon::thread_usleep(200 * 1000);
        }
        photon::fini();
        LOG_INFO("child exited, pid `", getpid());
        exit(0);
    } else {
        LOG_INFO("parent hello, pid `", getpid());
        return pid;
    }
}

int fork_parent_process(uint64_t event_engine) {
    pid_t m_pid = fork();
    if (m_pid < 0) {
        LOG_ERRNO_RETURN(0, -1, "fork error");
        return -1;
    }

    if (m_pid > 0) {
        LOG_INFO("main hello, pid `", getpid());
        return m_pid;
    }
    photon::fini();
    photon::init(event_engine, photon::INIT_IO_LIBCURL);

    photon::block_all_signal();
    photon::sync_signal(SIGINT, &sigint_handler);

    LOG_INFO("parent hello, pid `", getpid());
    photon::thread_sleep(1);
    auto pid = fork_child_process();
    photon::thread_sleep(1);

    int statVal;
    if (waitpid(pid, &statVal, WNOHANG) == 0) {
        if (kill(pid, SIGTERM) == 0) {
            LOG_INFO("kill child process with pid `", pid);
        } else {
            ERRNO eno;
            LOG_ERROR("kill child process with pid ` error, `", pid, eno);
        }
        wait_process_end_no_hang(pid);
    } else {
        check_process_exit_stat(statVal, pid);
        LOG_ERROR("child process exit unexpected");
    }

    LOG_INFO("child process exit status `", exit_normal);
    EXPECT_EQ(true, exit_normal);

    while (!exit_flag) {
        photon::thread_usleep(200 * 1000);
    }
    LOG_INFO("parent exited, pid `", getpid());
    photon::fini();
    exit(exit_normal ? 0 : -1);
}

TEST(ForkTest, Fork) {
    photon::init(photon::INIT_EVENT_NONE, photon::INIT_IO_NONE);
    DEFER(photon::fini());
    exit_flag = false;
    exit_normal = false;
#if defined(__linux__)
    auto pid = fork_parent_process(photon::INIT_EVENT_EPOLL | photon::INIT_EVENT_SIGNAL);
#else  // macOS, FreeBSD ...
    auto pid = fork_parent_process(photon::INIT_EVENT_DEFAULT);
#endif
    photon::thread_sleep(5);

    int statVal;
    if (waitpid(pid, &statVal, WNOHANG) == 0) {
        if (kill(pid, SIGINT) == 0) {
            LOG_INFO("kill parent process with pid `", pid);
        } else {
            ERRNO eno;
            LOG_ERROR("kill parent process with pid ` error, `", pid, eno);
        }
        wait_process_end_no_hang(pid);
    } else {
        check_process_exit_stat(statVal, pid);
        LOG_ERROR("parent process exit unexpected");
    }

    LOG_INFO("parent process exit status `", exit_normal);
    EXPECT_EQ(true, exit_normal);
}

TEST(ForkTest, ForkInThread) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_LIBCURL);
    DEFER(photon::fini());

    int ret = -1;
    std::thread th([&]() {
        pid_t pid = fork();
        ASSERT_GE(pid, 0);

        if (pid == 0) {
            LOG_INFO("child hello, pid `", getpid());
            exit(0);
        } else {
            LOG_INFO("parent hello, pid `", getpid());
            int statVal;
            waitpid(pid, &statVal, 0);
            ret = check_process_exit_stat(statVal, pid);
        }
    });
    th.join();
    EXPECT_EQ(0, ret);
}

TEST(ForkTest, PopenInThread) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_LIBCURL);
    DEFER(photon::fini());

    photon::semaphore sem(0);
    auto cmd = "du -s \"/tmp\"";
    ssize_t size = -1;
    std::thread([&] {
        auto f = popen(cmd, "r");
        EXPECT_NE(nullptr, f);
        DEFER(fclose(f));
        fscanf(f, "%lu", &size);
        sem.signal(1);
        LOG_INFO("popen done");
    }).detach();
    sem.wait(1);
    EXPECT_NE(-1, size);
    LOG_INFO(VALUE(size));
}

#if defined(__linux__) && defined(PHOTON_URING)
TEST(ForkTest, Iouring) {
    photon::init(photon::INIT_EVENT_NONE, photon::INIT_IO_NONE);
    DEFER(photon::fini());
    exit_flag = false;
    exit_normal = false;
    auto pid = fork_parent_process(photon::INIT_EVENT_IOURING | photon::INIT_EVENT_SIGNAL);

    photon::thread_sleep(5);

    int statVal;
    if (waitpid(pid, &statVal, WNOHANG) == 0) {
        if (kill(pid, SIGINT) == 0) {
            LOG_INFO("kill parent process with pid `", pid);
        } else {
            ERRNO eno;
            LOG_ERROR("kill parent process with pid ` error, `", pid, eno);
        }
        wait_process_end_no_hang(pid);
    } else {
        check_process_exit_stat(statVal, pid);
        LOG_ERROR("parent process exit unexpected");
    }

    LOG_INFO("parent process exit status `", exit_normal);
    EXPECT_EQ(true, exit_normal);
}
#endif

#if defined(__linux__)
TEST(ForkTest, LIBAIO) {
    photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_LIBAIO);
    DEFER(photon::fini());

    std::unique_ptr<photon::fs::IFileSystem> fs(
        photon::fs::new_localfs_adaptor("/tmp/", photon::fs::ioengine_libaio));
    std::unique_ptr<photon::fs::IFile> lf(
        fs->open("test_local_fs_fork_parent", O_RDWR | O_CREAT, 0755));
    void* buf = nullptr;
    ::posix_memalign(&buf, 4096, 4096);
    DEFER(free(buf));
    int ret = lf->pwrite(buf, 4096, 0);
    EXPECT_EQ(ret, 4096);

    ret = -1;
    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        std::unique_ptr<photon::fs::IFileSystem> fs(
            photon::fs::new_localfs_adaptor("/tmp/", photon::fs::ioengine_libaio));
        std::unique_ptr<photon::fs::IFile> lf(
            fs->open("test_local_fs_fork", O_RDWR | O_CREAT, 0755));
        void* buf = nullptr;
        ::posix_memalign(&buf, 4096, 4096);
        DEFER(free(buf));
        auto ret = lf->pwrite(buf, 4096, 0);
        EXPECT_EQ(ret, 4096);
        ret = lf->close();
        photon::fini();
        exit(ret);
    } else {
        int statVal;
        waitpid(pid, &statVal, 0);
        ret = check_process_exit_stat(statVal, pid);
    }
    EXPECT_EQ(0, ret);

    ret = lf->pwrite(buf, 4096, 0);
    EXPECT_EQ(ret, 4096);
    ret = lf->close();
    EXPECT_EQ(0, ret);
}
#endif

int main(int argc, char **argv) {
    set_log_output_level(0);

    ::testing::InitGoogleTest(&argc, argv);
    auto ret = RUN_ALL_TESTS();
    if (ret) LOG_ERROR_RETURN(0, ret, VALUE(ret));
}
