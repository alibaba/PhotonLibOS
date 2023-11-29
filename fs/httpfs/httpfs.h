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

#include <sys/ioctl.h>

#include <cinttypes>
#include <string>
#include <utility>

#include <photon/common/callback.h>
#include <photon/fs/filesystem.h>

namespace photon {
namespace net {
namespace http {
class Client;
}
}

namespace fs {
enum HTTPFileFlags {
    HTTP_HEADER = 0xF01,  // (const char*, const char*) ... for header
    HTTP_URL_PARAM =
        0xF02,  // const char* ... for url param (concat by '?' in url)
};

using FileOpenCallback = Delegate<void, const char*, IFile*>;

/**
 * @brief create httpfs object
 *
 * @param default_https once fn does not contains a protocol part, assume it is
 * a https url
 * @param conn_timeout timeout for http connection. -1 as forever.
 * @param stat_expire stat info will store till expired, then refresh.
 * @param open_cb callback when httpfile created, as AOP to set params just
 * before open function returned
 * @return IFileSystem* created httpfs
 */
IFileSystem* new_httpfs(bool default_https = false,
                        uint64_t conn_timeout = -1UL,
                        uint64_t stat_expire = -1UL,
                        FileOpenCallback open_cb = {});
/**
 * @brief create http file object
 *
 * @param url URL for file
 * @param httpfs set associated httpfs, set `nullptr` to create without httpfs,
 * means self-holding cURL object on demand
 * @param conn_timeout timeout for http connection, -1 as forever.
 * @param stat_expire stat info will store till expired, then refresh.
 * @param open_cb callback when httpfile created, as AOP to set params just
 * before open function returned
 * @return IFile* created httpfile
 */
IFile* new_httpfile(const char* url, IFileSystem* httpfs = nullptr,
                    uint64_t conn_timeout = -1UL, uint64_t stat_expire = -1UL,
                    FileOpenCallback open_cb = {});

IFileSystem* new_httpfs_v2(bool default_https = false,
                           uint64_t conn_timeout = -1UL,
                           uint64_t stat_expire = -1UL,
                           net::http::Client* client = nullptr,
                           bool client_ownership = false);

IFile* new_httpfile_v2(const char* url, IFileSystem* httpfs = nullptr,
                       uint64_t conn_timeout = -1UL,
                       uint64_t stat_expire = -1UL);
}  // namespace fs
}
