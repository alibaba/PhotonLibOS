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

#include <fcntl.h>
#include <fuse/fuse_opt.h>
#include <sys/stat.h>

#include <cstdio>
#include <thread>

#include <photon/common/alog.h>
#include <photon/common/executor/executor.h>
#include <photon/io/aio-wrapper.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>
#include "../aligned-file.h"
#include "../async_filesystem.h"
#include "../exportfs.h"
#include "../fuse_adaptor.h"
#include "../localfs.h"
#include "photon/common/executor/executor.h"
#include "photon/photon.h"

using namespace photon;

enum { KEY_SRC, KEY_IOENGINE };

struct localfs_config {
    char *src;
    char *ioengine;
    char *exportfs;
};

#define MYFS_OPT(t, p, v) \
    { t, offsetof(struct localfs_config, p), v }
struct fuse_opt localfs_opts[] = {MYFS_OPT("src=%s", src, 0), MYFS_OPT("ioengine=%s", ioengine, 0),
                                  MYFS_OPT("exportfs=%s", exportfs, 0), FUSE_OPT_END};

// this simple fuse test MUST run with -s (single thread)
int main(int argc, char *argv[]) {
    // currently they will be initialized inside fuser_go
    // photon::fd_events_init();
    // photon::libaio_wrapper_init();
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct localfs_config cfg;
    fuse_opt_parse(&args, &cfg, localfs_opts, NULL);
    int ioengine = fs::ioengine_libaio;
    if (cfg.ioengine) {
        switch (*cfg.ioengine) {
            case 'p':
                ioengine = fs::ioengine_psync;
                break;
            case 'l':
                ioengine = fs::ioengine_libaio;
                break;
            case 'a':
                ioengine = fs::ioengine_posixaio;
                break;
            default:
                LOG_ERROR_RETURN(EINVAL, -1, "Invalid ioengine ", cfg.ioengine);
        }
    }
    if (cfg.src == nullptr) LOG_ERROR_RETURN(EINVAL, -1, "Invalid source folder");
    LOG_DEBUG(VALUE(cfg.src));
    LOG_DEBUG(VALUE(cfg.ioengine));
    LOG_DEBUG(VALUE(cfg.exportfs));
    log_output_level = ALOG_INFO;
    if (cfg.exportfs && *cfg.exportfs == 't') {
        auto fs = fs::new_localfs_adaptor(cfg.src, ioengine);
        auto wfs = fs::new_aligned_fs_adaptor(fs, 4096, true, true);
        return fuser_go_exportfs(wfs, args.argc, args.argv);
    } else if (cfg.exportfs && *cfg.exportfs == 'c') {
        photon::Executor eth;
        auto afs = eth.perform([&]() {
            auto fs = fs::new_localfs_adaptor(cfg.src, ioengine);
            auto wfs = fs::new_aligned_fs_adaptor(fs, 4096, true, true);
            fs::exportfs_init();
            return export_as_sync_fs(wfs);
        });
        umask(0);
        set_fuse_fs(afs);
        auto oper = photon::fs::get_fuse_xmp_oper();
        return fuse_main(args.argc, args.argv, oper, NULL);
    } else {
        photon::init(INIT_EVENT_EPOLL, INIT_IO_LIBAIO);
        DEFER(photon::fini());
        auto fs = fs::new_localfs_adaptor(cfg.src, ioengine);
        auto wfs = fs::new_aligned_fs_adaptor(fs, 4096, true, true);
        DEFER(delete wfs);
        return fuser_go(wfs, args.argc, args.argv);
    }
}
