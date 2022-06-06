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
#include <photon/common/message-channel.h>
#include <photon/common/stream.h>

// a message channel based on any `IStream` object
namespace StreamMessenger
{
    // header of the stream message channel
    struct Header
    {
        const static uint64_t MAGIC   = 0x4962b4d24caa439e;
        const static uint32_t VERSION = 0;

        uint64_t magic   = MAGIC;       // the header magic
        uint32_t version = VERSION;     // version of the message
        uint32_t size;                  // size of the payload, not including the header
        uint64_t reserved = 0;          // padding to 24 bytes
    };

    // This class turns a stream into a message channel, by adding a `Header` to the
    // message before send it, and extracting the header immediately after recv it.
    // The stream can be any `IStream` object, like TCP socket or UNIX domain socket.
    IMessageChannel* new_messenger(IStream* stream);
}
