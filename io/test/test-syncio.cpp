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
#include <fcntl.h>
#include <photon/thread/thread.h>
#include <photon/io/aio-wrapper.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <photon/io/fd-events.h>
#include <photon/common/utility.h>
#include <photon/common/alog.h>
#include <gtest/gtest.h>
using namespace photon;


struct rand_args_t
{
    size_t start, length, n;
    int fd;
};

const size_t alignment = 4096;
photon::condition_variable aConditionVariable;

template<typename W>
void* rand_read(void* _args)
{
    auto args = (rand_args_t*)_args;
    auto length = args->length / alignment - 1;
    args->start &= (alignment-1);

    void* buf = nullptr;
    posix_memalign(&buf, alignment, alignment);

    for (size_t i = 0; i < args->n; ++i)
    {
        off_t offset = rand() % length * alignment + args->start;
        ssize_t ret;
        // LOG_DEBUG("rand_read round:`", i);
        if (i&1)
        {
            ret = W::pread(args->fd, buf, alignment, offset);
        }
        else
        {
            struct iovec iovec {buf, alignment};
            ret = W::preadv(args->fd, &iovec, 1, offset);
        }
        if (ret < 0)
            LOG_ERRNO_RETURN(0, nullptr, "failed to pread()");

        auto p = (uint64_t*)buf;
        for (size_t j = 0; j < alignment / 8; ++j)
            if (p[j])
                LOG_ERROR_RETURN(0, nullptr, "non-zero data block found: offset=`, size=4096", offset);
    }

    aConditionVariable.notify_all();
    return nullptr;
}

template<typename W>
void* rand_write(void* _args)
{
    auto args = (rand_args_t*)_args;
    auto length = args->length / alignment - 1;
    args->start &= (alignment-1);

    void* buf = nullptr;
    posix_memalign(&buf, alignment, alignment);
    memset(buf, 0, alignment);

    for (size_t i = 0; i < args->n; ++i)
    {
        off_t offset = rand() % length * alignment + args->start;
        ssize_t ret;
        if (i&1)
        {
            ret = W::pwrite(args->fd, buf, alignment, offset);
        }
        else
        {
            struct iovec iovec {buf, alignment};
            ret = W::pwritev(args->fd, &iovec, 1, offset);
        }
        if (ret < 0)
            LOG_ERRNO_RETURN(0, nullptr, "failed to pwrite()");

        // std::cout << "rand_write round:" << i << std::endl;
        auto p = (uint64_t*)buf;
        for (size_t j = 0; j < alignment / 8; ++j)
            if (p[j])
                LOG_ERROR_RETURN(0, nullptr, "non-zero data block found: offset=`, size=4096", offset);
    }

    aConditionVariable.notify_all();
    return nullptr;
}

int test_aio(const char* fn, bool is_posix);
int test_libaio(const char* fn)
{
    int ret = libaio_wrapper_init();
    if (ret < 0)
        LOG_ERROR_RETURN(0, -1, "failed to init libaio subsystem");

    DEFER(libaio_wrapper_fini());
    system("rm -f test_file.bin");
    return test_aio(fn, false);
}

int test_posix_libaio(const char* fn)
{
    system("rm -f test_file.bin");
    return test_aio(fn, true);
}

void create_n_thread_do(int n, thread_entry entry, void* arg)
{
    const int N_THREAD = 100;
    join_handle* jh[N_THREAD];
    if (n > N_THREAD) n = N_THREAD;
    for (int i = 0; i < n; ++i)
    {
        auto th = thread_create(entry, arg);
        jh[i] = thread_enable_join(th);
    }
    for (int i = 0; i < n; ++i)
        thread_join(jh[i]);
}

template<typename W>
void do_test_aio(rand_args_t& args)
{
    const int N = 10;
    int status = -4;
    auto engine = abi::__cxa_demangle(typeid(W).name(), NULL, NULL, &status);
    LOG_DEBUG("Testing ` randread with ` threads, ioengine is ", DEC(args.n).comma(true), N, engine);
    create_n_thread_do(N, &rand_read<W>, &args);
    LOG_DEBUG("Testing ` randwrite with ` threads, ioengine is ", DEC(args.n).comma(true), N, engine);
    create_n_thread_do(N, &rand_write<W>, &args);
    W::fsync(args.fd);
    W::fdatasync(args.fd);
    LOG_DEBUG("done!");
    free(engine);
}

int test_aio(const char* fn, bool is_posix)
{
    int fd = open(fn, O_RDWR);

    const int FILE_SIZE = alignment*1024;
    if (fd < 0)
    {
        LOG_ERROR( "failed to open file '`', create it.", fn);
        int total_size = FILE_SIZE;
        int write_bytes = 0, write_size = 0;
        _unused(write_bytes);
        char buffer[alignment];
        memset(buffer, 0x0, sizeof(buffer));
        fn = "test_file.bin";

        FILE* fp = fopen(fn, "wb");
        if (fp)
        {
            while(total_size)
            {
                if((size_t)total_size < alignment)
                {
                    write_size = total_size;
                }
                else
                {
                    write_size = alignment;
                }

                total_size -= fwrite(buffer, sizeof(char), write_size, fp);
            }
        }
        fclose(fp);
        fp = fopen(fn, "rwb+");
        fd = fileno(fp);
    }

    rand_args_t args;
    args.fd = fd;
    args.length = FILE_SIZE;
    if (is_posix)
    {
        args.n = 1000 * 10;
        do_test_aio<posixaio>(args);
    }
    else
    {
        args.n = 1000 * 100;
        do_test_aio<libaio>(args);
    }
    return 0;
}

int main(int argc, char** arg) {
    ::testing::InitGoogleTest(&argc, arg);

    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());

    test_libaio("/tmp/test-syncio");
    test_posix_libaio("/tmp/test-syncio");

    usleep(0);
    LOG_DEBUG("test result:`", RUN_ALL_TESTS());

}