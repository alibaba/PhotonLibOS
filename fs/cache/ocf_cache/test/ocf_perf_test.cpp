#include <fcntl.h>
#include <unistd.h>
#include <ctime>
#include <vector>

#include <gflags/gflags.h>
#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/event-loop.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#include <photon/fs/aligned-file.h>
#include <photon/fs/localfs.h>
#include <photon/net/curl.h>
// #include "../../../zfile/crc32/crc32c.h"
#include "../../cache.h"

// Common params
DEFINE_bool(ut_pass, false, "pass unit test directly. This suite is only for manual test");
DEFINE_bool(multi_files_test, false, "single or multiple files");
DEFINE_string(cache_type, "ocf", "block or ocf");
DEFINE_uint64(page_size, 4096, "page size or block size");
DEFINE_string(media_file, "/tmp/cache-bench/media", "media file path");
DEFINE_uint64(media_file_size_gb, 1, "media file size in gb");
DEFINE_int64(io_engine, 0, "0: psync, 1: libaio, 3: iouring");
DEFINE_uint64(concurrency, 16, "read concurrency");
DEFINE_uint64(ocf_prefetch_unit, 0, "prefetch unit in bytes");

// Single file test params
DEFINE_bool(random_read, true, "random read or sequential read");
DEFINE_string(src_file, "src", "src file path. Should be fully written before testing");
DEFINE_string(dst_file, "",
              "dst file path. Could be empty if writing is not needed. "
              "Verify its checksum manually after test");
DEFINE_uint64(total_requests, 0, "when to stop test");

// Multi files test params
DEFINE_uint64(num_files, 100, "num of files");
DEFINE_uint64(file_size_mb, 10, "file size in mb");

// Global variables
int qps, last_qps = 0;
uint64_t total_req = 0;
bool stop_test = false;

// static void handle_signal(int) {
//     LOG_INFO("try to stop test");
//     stop_test = true;
// }

static void handle_qps() {
    if (qps - last_qps > 3000 || last_qps - qps > 3000) {
        // avoid CPU 100% and no log
        last_qps = qps;
        photon::thread_yield();
    }
    ++qps;
    if (FLAGS_total_requests != 0 && ++total_req >= FLAGS_total_requests) {
        stop_test = true;
    }
}

static void show_qps_loop() {
    while (!stop_test) {
        photon::thread_sleep(1);
        LOG_INFO("qps: `", qps);
        qps = 0;
    }
}

/* Single file test */

template <typename T>
static int random_read(T *file, void *buf, size_t num_pages, photon::fs::IFile *dst_file) {
    int ret;
    IOVector iov;
    iov.push_back(buf, FLAGS_page_size);
    LOG_DEBUG("random_read: num_pages = `", num_pages);
    while (!stop_test) {
        int index = rand() % num_pages;
        off_t offset = FLAGS_page_size * index;
        ret = file->preadv(iov.iovec(), iov.iovcnt(), offset);
        if (ret != (int)FLAGS_page_size) {
            stop_test = true;
            LOG_ERRNO_RETURN(0, -1, "read failed, offset `, ret `", offset, ret);
        }
        if (dst_file) {
            ret = dst_file->pwritev(iov.iovec(), iov.iovcnt(), offset);
            if (ret != (int)FLAGS_page_size) {
                stop_test = true;
                LOG_ERRNO_RETURN(0, -1, "write dst failed, offset `, ret `", offset, ret);
            }
        }
        handle_qps();
    }
    return 0;
}

template <typename T>
static int sequential_read(T *file, void *buf, size_t start_index, size_t num_pages,
                           photon::fs::IFile *dst_file) {
    int ret;
    size_t index = start_index;
    LOG_DEBUG("sequential_read: start_index = `, num_pages = `", start_index, num_pages);
    IOVector iov;
    iov.push_back(buf, FLAGS_page_size);
    while (!stop_test) {
        off_t offset = FLAGS_page_size * index;
        ret = file->preadv(iov.iovec(), iov.iovcnt(), offset);
        if (ret != (int)FLAGS_page_size) {
            stop_test = true;
            LOG_ERRNO_RETURN(0, -1, "read failed, offset `, ret `", offset, ret);
        }
        if (dst_file) {
            ret = dst_file->pwritev(iov.iovec(), iov.iovcnt(), offset);
            if (ret != (int)FLAGS_page_size) {
                stop_test = true;
                LOG_ERRNO_RETURN(0, -1, "write dst failed, offset `, ret `", offset, ret);
            }
        }
        if (++index >= num_pages) {
            index = 0;
        }
        handle_qps();
    }
    return 0;
}

template <typename T>
static int work(T *cache_file, IOAlloc *io_alloc) {
    struct stat st_buf {};
    if (cache_file->fstat(&st_buf)) {
        LOG_ERROR_RETURN(0, -1, "failed to get size");
    }

    photon::fs::IFile *dst_file = nullptr;
    if (!FLAGS_dst_file.empty()) {
        dst_file = photon::fs::open_localfile_adaptor(
            FLAGS_dst_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644, FLAGS_io_engine);
        if (!dst_file) {
            LOG_ERROR_RETURN(0, -1, "failed to open dst file");
        }
        LOG_INFO("open new dst file, truncate its size to `", st_buf.st_size);
        dst_file->ftruncate(st_buf.st_size);
    }

    size_t num_pages = st_buf.st_size / FLAGS_page_size;
    std::vector<photon::join_handle *> join_hdls;

    void *buf = io_alloc->alloc(FLAGS_page_size);
    DEFER(io_alloc->dealloc(buf));

    if (FLAGS_random_read) {
        for (uint64_t i = 0; i < FLAGS_concurrency; i++) {
            auto th = photon::thread_create11(random_read<T>, cache_file, buf, num_pages, dst_file);
            join_hdls.push_back(photon::thread_enable_join(th));
        }
    } else {
        for (uint64_t i = 0; i < FLAGS_concurrency; i++) {
            ssize_t start_index = num_pages / FLAGS_concurrency * i;
            auto th = photon::thread_create11(sequential_read<T>, cache_file, buf, start_index,
                                              num_pages, dst_file);
            join_hdls.push_back(photon::thread_enable_join(th));
        }
    }

    for (auto join_hdl : join_hdls) {
        photon::thread_join(join_hdl);
    }
    return 0;
}

static int single_file_ocf_cache(IOAlloc *io_alloc, photon::fs::IFileSystem *src_fs,
                                 const std::string &root_dir) {
    LOG_INFO("Start single file ocf cache test");
    auto namespace_dir = root_dir + "/namespace/";
    if (::access(namespace_dir.c_str(), F_OK) != 0 && ::mkdir(namespace_dir.c_str(), 0755) != 0) {
        LOG_ERRNO_RETURN(0, -1, "failed to create namespace_dir");
    }
    auto namespace_fs = photon::fs::new_localfs_adaptor(namespace_dir.c_str());
    if (namespace_fs == nullptr) {
        LOG_ERROR_RETURN(0, -1, "failed tp create namespace_fs");
    }
    DEFER(delete namespace_fs);

    bool reload_media;
    photon::fs::IFile *media_file;
    if (::access(FLAGS_media_file.c_str(), F_OK) != 0) {
        reload_media = false;
        media_file = photon::fs::open_localfile_adaptor(FLAGS_media_file.c_str(), O_RDWR | O_CREAT,
                                                        0644, FLAGS_io_engine);
        media_file->fallocate(0, 0, FLAGS_media_file_size_gb * 1024 * 1024 * 1024);
    } else {
        reload_media = true;
        media_file = photon::fs::open_localfile_adaptor(FLAGS_media_file.c_str(), O_RDWR, 0644,
                                                        FLAGS_io_engine);
    }
    DEFER(delete media_file);

    auto ocf_cached_fs =
        photon::fs::new_ocf_cached_fs(src_fs, namespace_fs, FLAGS_page_size,
                                      FLAGS_ocf_prefetch_unit, media_file, reload_media, io_alloc);
    if (ocf_cached_fs == nullptr) {
        LOG_ERROR_RETURN(0, -1, "new_ocf_cached_fs error");
    }
    DEFER(delete ocf_cached_fs);

    auto file = ocf_cached_fs->open(FLAGS_src_file.c_str(), O_RDONLY, 0644);
    if (file == nullptr) {
        LOG_ERROR_RETURN(0, -1, "open file error");
    }
    DEFER(delete file);

    work(file, io_alloc);
    return 0;
}

static int single_file_file_cache(IOAlloc *io_alloc, photon::fs::IFileSystem *src_fs,
                                  const std::string &root_dir) {
    LOG_INFO("Start single file full file cache test");
    auto media_fs = photon::fs::new_localfs_adaptor(root_dir.c_str(), FLAGS_io_engine);
    if (media_fs == nullptr) {
        LOG_ERROR_RETURN(0, -1, "failed to create media fs");
    }
    auto cached_fs = photon::fs::new_full_file_cached_fs(
        src_fs, media_fs, FLAGS_page_size, FLAGS_media_file_size_gb, 1000 * 1000,
        2UL * FLAGS_media_file_size_gb * 1024 * 1024 * 1024, io_alloc, 0);
    if (cached_fs == nullptr) {
        LOG_ERROR_RETURN(0, -1, "new_ocf_cached_fs error");
    }
    DEFER(delete cached_fs);

    estring filename = FLAGS_src_file;
    if (!filename.starts_with("/")) {
        filename = estring().appends("/", filename);
    }

    auto file = cached_fs->open(filename.c_str(), O_RDONLY, 0644);
    if (file == nullptr) {
        LOG_ERROR_RETURN(0, -1, "open file error");
    }
    DEFER(delete file);

    work(file, io_alloc);
    return 0;
}

static int single_file_test(IOAlloc *io_alloc) {
    if (!FLAGS_dst_file.empty() && FLAGS_concurrency != 1) {
        LOG_ERROR_RETURN(0, -1, "Doesn't make sense to do concurrent writes on the same file")
    }

    auto root_dir = FLAGS_media_file.substr(0, FLAGS_media_file.rfind('/') + 1);

    auto src_fs = photon::fs::new_localfs_adaptor("/", FLAGS_io_engine);
    if (src_fs == nullptr) {
        LOG_ERROR_RETURN(0, -1, "failed to create fs");
    }
    DEFER(delete src_fs);

    auto qps_th = photon::thread_create11(show_qps_loop);
    auto qps_join_hdl = photon::thread_enable_join(qps_th);
    int ret = 0;

    if (FLAGS_cache_type == "ocf") {
        ret = single_file_ocf_cache(io_alloc, src_fs, root_dir);
    } else if (FLAGS_cache_type == "file") {
        ret = single_file_file_cache(io_alloc, src_fs, root_dir);
    }
    if (ret != 0) {
        stop_test = true;
    }
    photon::thread_join(qps_join_hdl);
    return 0;
}

/* Multiple files test */

struct fill_data_args {
    int64_t &num_files;
    photon::fs::IFileSystem *copied_fs;
    photon::fs::IFile *urandom_file;
    IOAlloc *io_alloc;
};

static void *fill_random_data(void *args_) {
    auto args = (fill_data_args *)args_;

    void *buf = args->io_alloc->alloc(FLAGS_page_size);
    DEFER(args->io_alloc->dealloc(buf));

    while (true) {
        if (args->num_files-- <= 0) {
            return nullptr;
        }
        uint64_t file_index = args->num_files;

        LOG_INFO("generate random test file ` in copied_fs", file_index);
        auto file = args->copied_fs->open(std::to_string(file_index).c_str(),
                                          O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (file == nullptr) {
            LOG_ERRNO_RETURN(0, nullptr, "error open file `", file_index);
        }
        DEFER(delete file);

        auto num_pages = FLAGS_file_size_mb * 1024 * 1024 / FLAGS_page_size;
        for (size_t j = 0; j < num_pages; ++j) {
            if (args->urandom_file->read(buf, FLAGS_page_size) != (ssize_t)FLAGS_page_size) {
                LOG_ERRNO_RETURN(0, nullptr, "error read urandom");
            }
            if (file->write(buf, FLAGS_page_size) != (ssize_t)FLAGS_page_size) {
                LOG_ERRNO_RETURN(0, nullptr, "error write file");
            }
        }
    }
    return nullptr;
}

static photon::fs::IFileSystem *prepare_copied_fs(IOAlloc *io_alloc, const std::string &root_dir) {
    // Prepare test files in copied_fs, and fill with random data
    bool need_init = true;
    if (access((root_dir + "/copied_fs").c_str(), F_OK) == 0) {
        need_init = false;
    } else {
        system(("mkdir -p " + root_dir + "/copied_fs").c_str());
    }

    auto copied_fs = photon::fs::new_localfs_adaptor((root_dir + "/copied_fs").c_str(),
                                                     photon::fs::ioengine_libaio);
    if (copied_fs == nullptr) {
        LOG_ERROR_RETURN(0, nullptr, "error create copied_fs");
    }

    if (!need_init) {
        LOG_INFO("copied fs exists");
        return copied_fs;
    }

    auto urandom_file = photon::fs::open_localfile_adaptor("/dev/urandom", O_RDONLY, 0644);
    DEFER(delete urandom_file);

    // Multi-thread fill random data
    int64_t num_files = FLAGS_num_files;
    auto args = new fill_data_args{num_files, copied_fs, urandom_file, io_alloc};
    photon::threads_create_join(32, fill_random_data, args);
    delete args;

    // Copy them up to src_fs dir
    LOG_INFO("copy test files ...");
    system(("cp -rf " + root_dir + "/copied_fs/* " + root_dir).c_str());

    LOG_INFO("clean page cache ...");
    system("echo 3 > /proc/sys/vm/drop_caches");
    return copied_fs;
}

static int crc_read(std::vector<photon::fs::IFile *> &cached_files,
                    std::vector<photon::fs::IFile *> &copied_files, IOAlloc *io_alloc) {
    // void *buf = io_alloc->alloc(FLAGS_page_size);
    // DEFER(io_alloc->dealloc(buf));
    // auto num_pages = FLAGS_file_size_mb * 1024 * 1024 / FLAGS_page_size;

    // while (!stop_test) {
    //     auto file_index = rand() % FLAGS_num_files;
    //     auto page_index = rand() % num_pages;

    //     if (cached_files[file_index]->pread(buf, FLAGS_page_size, page_index * FLAGS_page_size) <
    //         0) {
    //         LOG_ERRNO_RETURN(0, -1, "error read cached_file");
    //     }
    //     auto cached_crc = crc32::crc32c(buf, FLAGS_page_size);
    //     if (copied_files[file_index]->pread(buf, FLAGS_page_size, page_index * FLAGS_page_size) <
    //         0) {
    //         LOG_ERRNO_RETURN(0, -1, "error read copied_file");
    //     }
    //     auto copied_crc = crc32::crc32c(buf, FLAGS_page_size);

    //     if (cached_crc != copied_crc) {
    //         LOG_FATAL("FATAL: crc not equal !!! offset = `", page_index * FLAGS_page_size);
    //         abort();
    //     }
    //     qps++;
    // }
    return 0;
}

static int multiple_files_test(IOAlloc *io_alloc) {
    LOG_INFO("Start multiple files test");
    auto root_dir = FLAGS_media_file.substr(0, FLAGS_media_file.rfind('/') + 1);

    auto src_fs = photon::fs::new_localfs_adaptor(root_dir.c_str(), photon::fs::ioengine_libaio);
    if (src_fs == nullptr) {
        LOG_ERROR_RETURN(0, -1, "failed to create fs");
    }
    DEFER(delete src_fs);

    // The files in copied_fs are completely identical to those in src_fs
    auto copied_fs = prepare_copied_fs(io_alloc, root_dir);
    if (copied_fs == nullptr) {
        return -1;
    }
    DEFER(delete copied_fs);

    // Create ocf_cached_fs at src_fs dir
    auto namespace_dir = root_dir + "/namespace/";
    if (::access(namespace_dir.c_str(), F_OK) != 0 && ::mkdir(namespace_dir.c_str(), 0755) != 0) {
        LOG_ERRNO_RETURN(0, -1, "failed to create namespace_dir");
    }
    auto namespace_fs = photon::fs::new_localfs_adaptor(namespace_dir.c_str());
    if (namespace_fs == nullptr) {
        LOG_ERROR_RETURN(0, -1, "failed tp create namespace_fs");
    }
    DEFER(delete namespace_fs);

    bool reload_media;
    photon::fs::IFile *media_file;
    if (::access(FLAGS_media_file.c_str(), F_OK) != 0) {
        reload_media = false;
        media_file = photon::fs::open_localfile_adaptor(FLAGS_media_file.c_str(), O_RDWR | O_CREAT,
                                                        0644, FLAGS_io_engine);
        media_file->fallocate(0, 0, FLAGS_media_file_size_gb * 1024 * 1024 * 1024);
    } else {
        reload_media = true;
        media_file = photon::fs::open_localfile_adaptor(FLAGS_media_file.c_str(), O_RDWR, 0644,
                                                        FLAGS_io_engine);
    }
    DEFER(delete media_file);

    auto ocf_fs =
        photon::fs::new_ocf_cached_fs(src_fs, namespace_fs, FLAGS_page_size,
                                      FLAGS_ocf_prefetch_unit, media_file, reload_media, io_alloc);
    if (ocf_fs == nullptr) {
        LOG_ERROR_RETURN(0, -1, "error create ocf_fs");
    }
    DEFER(delete ocf_fs);

    std::vector<photon::fs::IFile *> cached_files;
    std::vector<photon::fs::IFile *> copied_files;

    // DEFER close all files
    auto release_resource = [&] {
        for (auto &f : cached_files) {
            delete f;
        }
        for (auto &f : copied_files) {
            delete f;
        }
    };
    DEFER(release_resource());

    // Open all files
    for (size_t i = 0; i < FLAGS_num_files; ++i) {
        auto cached_file = ocf_fs->open(std::to_string(i).c_str(), O_RDONLY, 0644);
        if (cached_file == nullptr) {
            LOG_ERRNO_RETURN(0, -1, "error open cached_file `", i);
        }
        cached_files.push_back(cached_file);
        auto copied_file = copied_fs->open(std::to_string(i).c_str(), O_RDONLY, 0644);
        if (copied_file == nullptr) {
            LOG_ERRNO_RETURN(0, -1, "error open copied_file `", i);
        }
        copied_files.push_back(copied_file);
    }

    // qps thread
    auto qps_th = photon::thread_create11(show_qps_loop);
    auto qps_join_hdl = photon::thread_enable_join(qps_th);

    // Concurrently run crc_read
    std::vector<photon::join_handle *> join_hdls;

    for (uint64_t i = 0; i < FLAGS_concurrency; i++) {
        auto th = photon::thread_create11(crc_read, cached_files, copied_files, io_alloc);
        join_hdls.push_back(photon::thread_enable_join(th));
    }

    for (auto join_hdl : join_hdls) {
        photon::thread_join(join_hdl);
    }
    photon::thread_join(qps_join_hdl);
    return 0;
}

int main(int argc, char *argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    log_output_level = ALOG_INFO;
    srand(time(nullptr));
    if (FLAGS_ut_pass) {
        LOG_INFO("pass unit test");
        return 0;
    }

photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);


    auto pooled_allocator = new PooledAllocator<2 * 1024 * 1024, 1024, 4096>;
    DEFER(delete pooled_allocator);
    IOAlloc io_alloc = pooled_allocator->get_io_alloc();

    if (!FLAGS_multi_files_test) {
        single_file_test(&io_alloc);
    } else {
        multiple_files_test(&io_alloc);
    }
    LOG_INFO("test stopped");
    return 0;
}
