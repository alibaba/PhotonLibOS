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
#include <cinttypes>
#include <string>
#include <photon/common/stream.h>
#include <photon/net/socket.h>
#include <photon/common/string_view.h>


class DuplexMemoryStream;

extern "C" IStream* new_simplex_memory_stream(uint32_t capacity);

extern "C" DuplexMemoryStream* new_duplex_memory_stream(uint32_t capacity);

// flag is a bitwise switch, showing if read(01) or write(10) are able to fail
// 00 ----- nothing goes fail
// 01 ----- read may fail (1%)
// 10 ----- write may fail (1%)
// 11 ----- both read and write may fail
// default is 11(3 in 10-based integer), both read and write operation may fail
extern "C" IStream* new_fault_stream(IStream* stream, int flag=3, bool ownership=false);

class StringSocketStream;
StringSocketStream* new_string_socket_stream();

class DuplexMemoryStream
{
public:
    virtual ~DuplexMemoryStream() { }
    IStream* endpoint_a;     // do NOT delete it!!!
    IStream* endpoint_b;     // do NOT delete it!!!
    virtual int close() = 0;
};

class StringSocketStream : public photon::net::ISocketStream {
protected:
    std::string _in, _out;
    std::string_view _inv;
public:
    void set_input(std::string_view in) {
        _in.assign(in.data(), in.size());
        _inv = {_in};
    }
    void set_input(std::string_view in, bool copy) {
        if (!copy) _inv = in;
        else set_input(in);
    }
    std::string_view input() {
        return _inv;
    }
    std::string& output() {
        return _out;
    }
};
