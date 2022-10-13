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

#include "httpfs.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdarg>
#include <string>
#include <vector>

#include "../../net/curl.h"
#include <photon/thread/thread.h>
#include <photon/common/string-keyed.h>
#include <photon/common/string_view.h>
#include <photon/common/timeout.h>
#include <photon/common/utility.h>
#include "../virtual-file.h"

namespace photon {
namespace fs {

constexpr static char HTTP_PREFIX[] = "http:/";
constexpr static char HTTPS_PREFIX[] = "https:/";

class HttpFs : public fs::IFileSystem {
protected:
    bool default_https;
    uint64_t conn_timeout;
    uint64_t stat_timeout;
    FileOpenCallback m_cb;

public:
    HttpFs(bool default_https, uint64_t conn_timeout, uint64_t stat_timeout, FileOpenCallback cb)
        : default_https(default_https),
          conn_timeout(conn_timeout),
          stat_timeout(stat_timeout),
          m_cb(cb) {}
    IFile* open(const char* pathname, int flags) override;
    IFile* open(const char* pathname, int flags, mode_t) override {
        return open(pathname, flags);
    }
    int stat(const char* pathname, struct stat* buf) override {
        auto file = open(pathname, O_RDONLY);
        DEFER(delete file);
        if (!file) return -1;
        return file->fstat(buf);
    }
    int lstat(const char* path, struct stat* buf) override { return stat(path, buf); }
    UNIMPLEMENTED_POINTER(IFile* creat(const char*, mode_t) override);
    UNIMPLEMENTED(int mkdir(const char*, mode_t) override);
    UNIMPLEMENTED(int rmdir(const char*) override);
    UNIMPLEMENTED(int link(const char*, const char*) override);
    UNIMPLEMENTED(int symlink(const char*, const char*) override);
    UNIMPLEMENTED(ssize_t readlink(const char*, char*, size_t) override);
    UNIMPLEMENTED(int rename(const char*, const char*) override);
    UNIMPLEMENTED(int chmod(const char*, mode_t) override);
    UNIMPLEMENTED(int chown(const char*, uid_t, gid_t) override);
    UNIMPLEMENTED(int statfs(const char* path, struct statfs* buf) override);
    UNIMPLEMENTED(int statvfs(const char* path, struct statvfs* buf) override);
    UNIMPLEMENTED(int access(const char* pathname, int mode) override);
    UNIMPLEMENTED(int truncate(const char* path, off_t length) override);
    UNIMPLEMENTED(int syncfs() override);
    UNIMPLEMENTED(int unlink(const char* filename) override);
    UNIMPLEMENTED(int lchown(const char* pathname, uid_t owner, gid_t group)
                      override);
    UNIMPLEMENTED(int utime(const char *path, const struct utimbuf *file_times) override);
    UNIMPLEMENTED(int utimes(const char *path, const struct timeval times[2]) override);
    UNIMPLEMENTED(int lutimes(const char *path, const struct timeval times[2]) override);
    UNIMPLEMENTED(int mknod(const char *path, mode_t mode, dev_t dev) override);
    UNIMPLEMENTED_POINTER(DIR* opendir(const char*) override);

    net::cURL* acquire_curl() { return new net::cURL(); }

    void release_curl(net::cURL* curl) { delete curl; }
};

class HttpFile : public fs::VirtualReadOnlyFile {
public:
    std::string url;
    HttpFs* fs;
    struct stat m_stat;
    uint64_t stat_gettime = 0;
    uint64_t conn_timeout;
    uint64_t stat_timeout;
    bool exists;
    bool authorized;

    std::unordered_map<std::string, std::string> common_header;
    std::string url_param;

    net::cURL* acuqire_curl() {
        net::cURL* curl;
        if (fs)
            curl = fs->acquire_curl();
        else
            curl = new net::cURL();
        curl->clear_header();
        for (auto& kv : common_header) {
            curl->append_header(kv.first, kv.second);
        }
        return curl;
    }

    std::string get_url() {
        auto geturl = url;
        if (!url_param.empty()) {
            geturl.push_back('?');
            geturl.append(url_param);
        }
        return geturl;
    }

    void release_curl(net::cURL* curl) {
        if (fs)
            fs->release_curl(curl);
        else
            delete curl;
    }

    int refresh_stat() {
        Timeout tmo(conn_timeout);
        int ret = 0;
    again:
        auto curl = acuqire_curl();
        DEFER(release_curl(curl));
        net::HeaderMap headers;
        curl->set_header_container(&headers);
        curl->set_range(0, 0);
        net::DummyReaderWriter dummy;
        ret = curl->GET(get_url().c_str(), &dummy, tmo.timeout());
        if (ret < 200) {
            if (photon::now >= tmo.expire()) {
                // set errno to ENOENT since stat should not ETIMEDOUT
                LOG_ERROR_RETURN(ENOENT, -1, "Failed to update file stat");
            }
            goto again;
        }
        stat_gettime = photon::now;
        authorized = (ret >= 0 && ret != 401 && ret != 403);
        exists = (ret == 200 || ret == 206);
        memset(&m_stat, 0, sizeof(m_stat));
        char buffer[256];
        uint64_t len = -1;
        if (headers.try_get("content-range", buffer) < 0) {
            if (headers.try_get("content-length", len) < 0) {
                LOG_ERROR("Unexpected http response header");
                stat_gettime = 0;
                exists = false;
            }
        } else {
            auto lenstr = strrchr(buffer, '/');
            if (lenstr == nullptr) {
                LOG_ERROR("Unexpected http response header");
                stat_gettime = 0;
                exists = false;
            }
            len = atoll(lenstr + 1);
        }
        m_stat.st_mode = S_IFREG;
        m_stat.st_size = len;
        return 0;
    }

    HttpFile(const char* url, IFileSystem* httpfs, uint64_t conn_timeout,
             uint64_t stat_timeout, FileOpenCallback cb)
        : url(url),
          fs((HttpFs*)httpfs),
          conn_timeout(conn_timeout),
          stat_timeout(stat_timeout) {
              cb(this->url.c_str(), this);
          }

    HttpFile(std::string&& url, IFileSystem* httpfs, uint64_t conn_timeout,
             uint64_t stat_timeout, const std::string_view& param, FileOpenCallback cb)
        : url(std::move(url)),
          fs((HttpFs*)httpfs),
          conn_timeout(conn_timeout),
          stat_timeout(stat_timeout),
          url_param(std::move(param)) {
              cb(this->url.c_str(), this);
          }

    IFileSystem* filesystem() override { return (IFileSystem*)fs; }
    ssize_t preadv(const struct iovec* iovec, int iovcnt,
                   off_t offset) override {
        Timeout tmo(conn_timeout);
        struct stat stat;
        ssize_t ret = fstat(&stat);
        if (ret < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to access file ", VALUE(url));
    again:
        auto curl = acuqire_curl();
        DEFER(release_curl(curl));
        net::IOVWriter writer(iovec, iovcnt);
        curl->set_range(
            offset, std::min(offset + (off_t)writer.sum(), stat.st_size) - 1);
        net::HeaderMap headers;
        curl->set_header_container(&headers);
        ret = curl->GET(get_url().c_str(), &writer, tmo.timeout());
        if (ret < 200) {
            if (photon::now > tmo.expire()) {
                LOG_ERROR_RETURN(ETIMEDOUT, -1, "Failed to perform GET ", VALUE(url),
                                 VALUE(offset));
            }
            goto again;
        }
        if (ret != 200 && ret != 206) {
            if (ret == 401 || ret == 403) {
                authorized = false;
                LOG_ERROR_RETURN(EACCES, -1, "GET not authorized", VALUE(url));
            }
            LOG_ERROR_RETURN(ENOENT, -1, "GET response got unexpected stats ",
                             VALUE(url), VALUE(offset), VALUE(ret));
        }
        authorized = true;
        headers.try_get("content-length", ret);
        return ret;
    }

    int fstat(struct stat* buf) override {
        if (!stat_gettime || photon::now - stat_gettime >= stat_timeout ||
            !authorized) {
            auto ret = refresh_stat();
            if (ret < 0) return ret;
        }
        if (!authorized) {
            errno = EACCES;
            return -1;
        }
        if (exists && buf) {
            memcpy(buf, &m_stat, sizeof(struct stat));
        } else if (!exists) {
            errno = ENOENT;
        }
        return exists ? 0 : -1;
    }

    void add_header(va_list args) {
        common_header[va_arg(args, const char*)] = va_arg(args, const char*);
    }

    void add_url_param(va_list args) { url_param = va_arg(args, const char*); }

    int vioctl(int request, va_list args) override {
        switch (request) {
            case HTTP_HEADER:
                add_header(args);
                break;
            case HTTP_URL_PARAM:
                add_url_param(args);
                break;
            default:
                LOG_ERROR_RETURN(EINVAL, -1,
                                 "Unknow ioctl request, supports ` and ` only",
                                 VALUE(HTTP_HEADER), VALUE(HTTP_URL_PARAM));
        }
        return 0;
    }
};

static bool strciprefix(char const* a, char const* b) {
    for (;; a++, b++) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d != 0 || !*a) return d;
    }
    return !*b;
}

IFile* HttpFs::open(const char* pathname, int flags) {
    std::string url;
    std::string_view fn(pathname);
    // ignore prefix '/'
    if (fn.front() == '/') fn = fn.substr(1);
    if (!strciprefix(fn.data(), HTTP_PREFIX) &&
        !strciprefix(fn.data(), HTTPS_PREFIX)) {
        url = default_https ? HTTPS_PREFIX : HTTP_PREFIX;
        url += '/';
    }
    std::string_view param;
    auto pos = fn.find_last_of('?');
    if (pos != fn.npos) {
        param = fn.substr(pos + 1);
        fn = fn.substr(0, pos);
    }
    url.append(fn.data(), fn.length());
    return new HttpFile(std::move(url), this, conn_timeout, stat_timeout,
                        param, m_cb);
}

IFileSystem* new_httpfs(bool default_https, uint64_t conn_timeout,
                        uint64_t stat_timeout, FileOpenCallback cb) {
    return new HttpFs(default_https, conn_timeout, stat_timeout, cb);
}

IFile* new_httpfile(const char* url, IFileSystem* httpfs, uint64_t conn_timeout,
                    uint64_t stat_timeout, FileOpenCallback cb) {
    return new HttpFile(url, httpfs, conn_timeout, stat_timeout, cb);
}
}  // namespace fs
}
