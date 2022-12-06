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

#include <photon/net/http/client.h>
#include <photon/net/http/url.h>
#include <photon/common/alog-stdstring.h>
#include <photon/thread/thread.h>
#include <photon/common/string-keyed.h>
#include <photon/common/string_view.h>
#include <photon/common/estring.h>
#include <photon/common/timeout.h>
#include <photon/common/utility.h>
#include <photon/fs/virtual-file.h>
#include <photon/common/iovector.h>

namespace photon {
namespace fs {

class HttpFs_v2 : public fs::IFileSystem {
protected:
    bool m_default_https;
    uint64_t m_conn_timeout;
    uint64_t m_stat_timeout;

    net::http::Client *m_client;

public:
    HttpFs_v2(bool default_https, uint64_t conn_timeout, uint64_t stat_timeout)
        : m_default_https(default_https),
          m_conn_timeout(conn_timeout),
          m_stat_timeout(stat_timeout) {
              m_client = net::http::new_http_client();
          }
    ~HttpFs_v2() {
        delete m_client;
    }
    net::http::Client* get_client() { return m_client; }
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
};

class HttpFile_v2 : public fs::VirtualReadOnlyFile {
public:
    std::string m_url;
    net::http::CommonHeaders<> m_common_header;
    HttpFs_v2* m_fs;
    struct stat m_stat;
    uint64_t m_stat_gettime = 0;
    uint64_t m_conn_timeout;
    uint64_t m_stat_timeout;

    bool m_etimeout = false;
    bool m_exists;
    bool m_authorized;

    std::string m_url_param;

    HttpFile_v2(const char* url, HttpFs_v2* httpfs, uint64_t conn_timeout,
             uint64_t stat_timeout)
        : m_url(url),
          m_fs((HttpFs_v2*)httpfs),
          m_conn_timeout(conn_timeout),
          m_stat_timeout(stat_timeout) {}

    HttpFile_v2(std::string&& url, HttpFs_v2* httpfs, uint64_t conn_timeout,
             uint64_t stat_timeout, const std::string_view& param)
        : m_url(std::move(url)),
          m_fs((HttpFs_v2*)httpfs),
          m_conn_timeout(conn_timeout),
          m_stat_timeout(stat_timeout),
          m_url_param(param) {}

    int update_stat_from_resp(const net::http::Client::Operation* op) {
        auto ret = op->status_code;
        m_stat_gettime = photon::now;
        m_authorized = (ret >= 0 && ret != 401 && ret != 403);
        m_exists = (ret == 200 || ret == 206);
        memset(&m_stat, 0, sizeof(m_stat));
        auto len = op->resp.resource_size();
        if (len == -1) {
            m_stat_gettime = 0;
            m_exists = false;
            LOG_ERROR_RETURN(0, -1, "Unexpected http response header");
        }
        m_stat.st_mode = S_IFREG;
        m_stat.st_size = len;
        return 0;
    }
    void send_read_request(net::http::Client::Operation &op, off_t offset, size_t length, const Timeout &tmo) {
    again:
        estring url;
        url.appends(m_url, "?", m_url_param);
        op.req.reset(net::http::Verb::GET, url);
        op.set_enable_proxy(m_fs->get_client()->has_proxy());
        op.req.headers.merge(m_common_header);
        op.req.headers.range(offset, offset + length - 1);
        op.req.headers.content_length(0);
        op.timeout = tmo.timeout();
        m_fs->get_client()->call(&op);
        if (op.status_code < 0) {
            if (tmo.timeout() == 0) {
                m_etimeout = true;
                LOG_ERROR_RETURN(ENOENT, , "http timedout");
            }
            photon::thread_sleep(1);
            goto again;
        }
    }
    using HTTP_OP = net::http::Client::OperationOnStack<64 * 1024 - 1>;
    int refresh_stat() {
        Timeout tmo(m_conn_timeout);
        HTTP_OP op;
        send_read_request(op, 0, 1, tmo);
        if (op.status_code < 0) return -1;
        update_stat_from_resp(&op);
        return 0;
    }

    IFileSystem* filesystem() override { return (IFileSystem*)m_fs; }
    ssize_t preadv(const struct iovec* iovec, int iovcnt,
                   off_t offset) override {
        Timeout tmo(m_conn_timeout);
        struct stat s;
        if (fstat(&s) < 0) LOG_ERROR_RETURN(0, -1, "Failed to get file length");
        if (offset >= s.st_size) return 0;
        iovector_view view((struct iovec*)iovec, iovcnt);
        auto count = std::min(view.sum(), (size_t)(s.st_size - offset));
        if (count == 0) return 0;
        HTTP_OP op;
        send_read_request(op, offset, count, tmo);
        if (op.status_code < 0) return -1;
        auto ret = op.status_code;
        if (ret != 200 && ret != 206) {
            if (ret == 401 || ret == 403) {
                m_authorized = false;
                LOG_ERROR_RETURN(EACCES, -1, "GET not authorized", VALUE(m_url));
            }
            LOG_ERROR_RETURN(ENOENT, -1, "GET response got unexpected stats ",
                             VALUE(m_url), VALUE(offset), VALUE(ret));
        }
        if (update_stat_from_resp(&op) < 0) {
            LOG_ERROR_RETURN(ENOENT, -1, "GET response got unexpected header ",
                             VALUE(m_url), VALUE(offset), VALUE(ret));
        }
        m_authorized = true;
        ret = op.resp.readv(iovec, iovcnt);
        return ret;
    }

    int fstat(struct stat* buf) override {
        m_etimeout = false;
        if (!m_stat_gettime || photon::now - m_stat_gettime >= m_stat_timeout ||
            !m_authorized) {
            auto ret = refresh_stat();
            if (ret < 0) return ret;
        }
        if (m_etimeout) return -1;
        if (!m_authorized) {
            errno = EACCES;
            return -1;
        }
        if (m_exists && buf) {
            memcpy(buf, &m_stat, sizeof(struct stat));
        } else if (!m_exists) {
            errno = ENOENT;
        }
        return m_exists ? 0 : -1;
    }

    //TODO: 这里是否需要考虑m_common_header被打爆的问题？
    void add_header(va_list args) {
        m_common_header.insert(va_arg(args, const char*), va_arg(args, const char*));
    }

    void add_url_param(va_list args) { m_url_param = va_arg(args, const char*); }

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

IFile* HttpFs_v2::open(const char* pathname, int flags) {
    if (!pathname) LOG_ERROR_RETURN(EINVAL, nullptr, "NULL is not allowed");
    if (flags != O_RDONLY) return nullptr;

    if (pathname[0] == '/') ++pathname;
    estring_view fn(pathname), prefix;
    if (0 == net::http::what_protocol(fn))
        prefix = net::http::http_or_s(!m_default_https);

    std::string_view param;
    auto pos = fn.find_first_of('?');
    if (pos != fn.npos) {
        param = fn.substr(pos + 1);
        fn = fn.substr(0, pos);
    }

    return new HttpFile_v2(std::move(estring().appends(prefix, fn)),
        this, m_conn_timeout, m_stat_timeout, param);
}

IFileSystem* new_httpfs_v2(bool default_https, uint64_t conn_timeout,
                        uint64_t stat_timeout) {
    return new HttpFs_v2(default_https, conn_timeout, stat_timeout);
}

IFile* new_httpfile_v2(const char* url, HttpFs_v2* httpfs, uint64_t conn_timeout,
                    uint64_t stat_timeout) {
    return new HttpFile_v2(url, httpfs, conn_timeout, stat_timeout);
}
}  // namespace fs
}
