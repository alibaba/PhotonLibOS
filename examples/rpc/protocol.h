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

#include <photon/rpc/serialize.h>

#include <cstdint>

/**
 * @brief interface means class with member deleivers `IID` as
 * interface.
 */
struct ExampleInterface {
    const static uint32_t IID = 0x222;
};

/**
 * @brief `TestProtocol` is a example of self-defined rpc protocol.
 * A protocol should have `IID` and `FID` as identifier, and struct/class
 * `Request` `Response` inherited `RPC::Message`, defined
 * details of RPC request and response.
 * NOTE: RPC will not adjust by endings. DO NOT try to make RPC across different
 * machine byte order archs.
 */
struct Testrun : public ExampleInterface {
    /**
     * @brief `IID` and `FID` will conbined as a coordinate identitfy,
     * these numbers will be set into RPC request and response header.
     * The combination of IID & FID should be identical.
     */
    const static uint32_t FID = 0x333;

    struct SomePODStruct {
        bool foo;
        size_t bar;
    };

    /**
     * @brief `Request` struct defines detail of RPC request,
     *
     */
    struct Request : public photon::rpc::Message {
        // All POD type fields can keep by what it should be in fields.
        uint32_t someuint;
        int64_t someint64;
        char somechar;
        SomePODStruct somestruct;
        // Array type should use RPC::array, do not supports multi-dimensional
        // arrays.
        photon::rpc::array<int> intarray;
        // String should use RPC::string instead.
        // NOTE: Since RPC::string is implemented by array, do not support
        // array-of-string `photon::rpc::array<RPC::string>` will leads to
        // unexpected result.
        photon::rpc::string somestr;
        // `photon::rpc::iovec_array` as a special fields, RPC will deal with
        // inside-buffer contents. it deal with buffer as iovectors
        photon::rpc::iovec_array buf;

        // After all, using macro `PROCESS_FIELDS` to set up compile time
        // reflection for those field want to transport by RPC
        PROCESS_FIELDS(someuint, someint64, somechar, somestruct, intarray,
                       somestr, buf);
    };

    struct Response : public photon::rpc::Message {
        // Since response is also a RPC::Message, keeps rule of `Request`

        size_t len;
        // `photon::rpc::aligned_iovec_array` will keep buffer data in the head
        // of RPC message when serializing/deserializing, may help to keep
        // memory alignment.
        photon::rpc::aligned_iovec_array buf;

        PROCESS_FIELDS(len, buf);
    };
};

/**
 * @brief simple heartbeat records time
 */
struct Heartbeat : public ExampleInterface {
    const static uint32_t FID = 0x01;

    struct Request : public photon::rpc::Message {
        uint64_t now;

        PROCESS_FIELDS(now);
    };

    struct Response : public photon::rpc::Message {
        uint64_t now;

        PROCESS_FIELDS(now);
    };
};

/**
 * @brief sample to send iovector to remote
 */
struct Echo : public ExampleInterface {
    const static uint32_t FID = 0x02;

    struct Request : public photon::rpc::Message {
        photon::rpc::string str;

        PROCESS_FIELDS(str);
    };

    struct Response : public photon::rpc::Message {
        photon::rpc::string str;

        PROCESS_FIELDS(str);
    };
};

/**
 * @brief sample to read iovector from remote
 */
struct ReadBuffer : public ExampleInterface {
    const static uint32_t FID = 0x12;

    struct Request : public photon::rpc::Message {
        photon::rpc::string fn;

        PROCESS_FIELDS(fn);
    };

    struct Response : public photon::rpc::Message {
        ssize_t ret;
        photon::rpc::iovec_array buf;

        PROCESS_FIELDS(ret, buf);
    };
};

/**
 * @brief sample to send iovector to remote
 */
struct WriteBuffer : public ExampleInterface {
    const static uint32_t FID = 0x13;

    struct Request : public photon::rpc::Message {
        photon::rpc::string fn;
        photon::rpc::aligned_iovec_array buf;

        PROCESS_FIELDS(fn, buf);
    };

    struct Response : public photon::rpc::Message {
        ssize_t ret;

        PROCESS_FIELDS(ret);
    };
};
