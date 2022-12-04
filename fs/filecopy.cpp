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

#include "filecopy.h"

#include <cstdlib>
#include <stddef.h>
#include <sys/stat.h>

#include <photon/common/alog.h>
#include "filesystem.h"

namespace photon {
namespace fs {

static constexpr size_t ALIGNMENT = 4096;

ssize_t filecopy(IFile* infile, IFile* outfile, size_t bs, int retry_limit) {
    if (bs == 0) LOG_ERROR_RETURN(EINVAL, -1, "bs should not be 0");
    void* buff = nullptr;
    ;
    // buffer allocate, with 4K alignment
    ::posix_memalign(&buff, ALIGNMENT, bs);
    if (buff == nullptr)
        LOG_ERROR_RETURN(ENOMEM, -1, "Fail to allocate buffer with ",
                         VALUE(bs));
    DEFER(free(buff));
    off_t offset = 0;
    ssize_t count = bs;
    while (count == (ssize_t)bs) {
        int retry = retry_limit;
    again_read:
        if (!(retry--))
            LOG_ERROR_RETURN(EIO, -1, "Fail to read at ", VALUE(offset),
                             VALUE(count));
        auto rlen = infile->pread(buff, bs, offset);
        if (rlen < 0) {
            LOG_DEBUG("Fail to read at ", VALUE(offset), VALUE(count),
                      " retry...");
            goto again_read;
        }
        retry = retry_limit;
    again_write:
        if (!(retry--))
            LOG_ERROR_RETURN(EIO, -1, "Fail to write at ", VALUE(offset),
                             VALUE(count));
        // cause it might write into file with O_DIRECT
        // keep write length as bs
        auto wlen = outfile->pwrite(buff, bs, offset);
        // but once write lenth larger than read length treats as OK
        if (wlen < rlen) {
            LOG_DEBUG("Fail to write at ", VALUE(offset), VALUE(count),
                      " retry...");
            goto again_write;
        }
        count = rlen;
        offset += count;
    }
    // truncate after write, for O_DIRECT
    outfile->ftruncate(offset);
    return offset;
}

}  // namespace fs
}
