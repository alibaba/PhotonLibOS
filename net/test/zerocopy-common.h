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

#include <photon/rpc/rpc.h>
#include <unistd.h>

using namespace photon;

static const int checksum_padding_size = 4096;

DEFINE_string(ip, "127.0.0.1", "ip");
DEFINE_uint64(port, 9527, "port");
DEFINE_uint64(buf_size, 4096, "RPC buffer size");
DEFINE_uint64(num_threads, 32, "num of threads");
DEFINE_bool(calculate_checksum, false, "calculate checksum for read test");

enum class SocketType {
    TCP, ZEROCOPY, IOURING, ET,
};

enum class IOType {
    READ, WRITE,
};

struct TestReadProto {
    const static uint32_t IID = 9527;
    const static uint32_t FID = 1;

    struct Request : public rpc::Message {
        int file_index;

        PROCESS_FIELDS(file_index);
    };

    struct Response : public rpc::Message {
        rpc::aligned_iovec_array buf;

        PROCESS_FIELDS(buf);
    };
};

struct TestWriteProto {
    const static uint32_t IID = 9527;
    const static uint32_t FID = 2;

    struct Request : public rpc::Message {
        rpc::aligned_buffer buf;

        PROCESS_FIELDS(buf);
    };

    struct Response : public rpc::Message {
        int code;

        PROCESS_FIELDS(code);
    };
};
