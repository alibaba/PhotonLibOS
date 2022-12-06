
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

#include <photon/common/string_view.h>

class IStream;

namespace photon {

namespace fs {
    class IFile;
}
namespace net {

class ISocketStream;

namespace http {

IStream *new_body_read_stream(ISocketStream *stream, std::string_view body, size_t body_remain);

IStream *new_chunked_body_read_stream(ISocketStream *stream, std::string_view body);

IStream *new_chunked_body_write_stream(ISocketStream *stream);

IStream *new_body_write_stream(ISocketStream *stream, size_t size);

} // namespace http
} // namespace net
} // namespace photon
