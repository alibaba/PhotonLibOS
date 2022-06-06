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
#include <cstdint>

namespace photon {
namespace fs
{
    class IFile;
    class IFileSystem;
    class DIR;
    class IAsyncFile;
    class IAsyncFileSystem;
    class AsyncDIR;

    extern "C"
    {
        // The exports below wrap a sync file/fs/dir object into an
        // async/sync one for outside world, assuming that the exported
        // will be used in other OS thread(s) (not necessarily), so
        // the exports will act accordingly in a thread-safe manner.

        int exportfs_init(uint32_t thread_pool_capacity = 0);
        int exportfs_fini();

        // create exports from sync file/fs/dir, obtaining their ownship, so deleting
        // the adaptor objects means deleting the async fs objects, too.
        IAsyncFile*         export_as_async_file(IFile* file);
        IAsyncFileSystem*   export_as_async_fs(IFileSystem* fs);
        AsyncDIR*           export_as_async_dir(DIR* dir);

        IFile*         export_as_sync_file(IFile* file);
        IFileSystem*   export_as_sync_fs(IFileSystem* fs);
        DIR*           export_as_sync_dir(DIR* dir);

        IFile*         export_as_easy_sync_file(IFile* file);
        IFileSystem*   export_as_easy_sync_fs(IFileSystem* fs);
        DIR*           export_as_easy_sync_dir(DIR* dir);
    }
}
}
