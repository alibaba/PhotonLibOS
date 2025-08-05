#pragma once

#include <photon/net/http/client.h>

#include <functional>
#include <map>

namespace FileSystemExt {
namespace OssMiniSdk {

extern const std::map<std::string, std::string> MIME_TYPE_MAP;

constexpr int OSS_REQUEST_RETRY_TIMES = 2;
constexpr int OSS_MAX_PATH_LEN = 1023;
constexpr uint64_t OSS_REQUEST_RETRY_INTERVAL_US = 20000ULL;
constexpr uint64_t OSS_REQUEST_MAX_RETRY_INTERVAL_US = 1000000ULL;

struct OssOptions {
  std::string endpoint;
  std::string bucket;
  std::string region;
  bool use_list_obj_v2 = true;
  int max_list_ret_cnt = 1000;
  std::string user_agent{"ossfs2-0.0.1"};
  std::string bind_ips;
  uint64_t request_timeout_us = 60ull * 1000 * 1000;
};

struct OSSCredentials {
  std::string m_accessKeyId;
  std::string m_accessKeySecret;
  std::string m_sessionToken;
};

class CredentialsProvider {
 public:
  CredentialsProvider() = default;
  virtual ~CredentialsProvider() = default;
  virtual OSSCredentials getCredentials() = 0;
  virtual void setCredentials(const OSSCredentials &cred) {}
};

struct ObjectMeta {
  size_t size = 0;
  time_t mtime = 0;
  std::string etag;
};

struct ObjectHeaderMeta {
  size_t size = 0;
  time_t mtime = 0;
  std::string etag;
  std::string type;  // Appendable/Normal/...
  std::string storage_class;
  std::pair<bool, uint64_t> crc64{false,
                                  0};  // crc64.first is true if crc64 is valid.
};

struct ListObjectsCBParams {
  std::string_view key;
  std::string_view etag;
  size_t size = 0;
  time_t mtime = 0;
  bool is_com_prefix;
  ListObjectsCBParams(std::string_view key_, std::string_view etag_,
                      size_t size_, time_t mtime_, bool is_com_prefix_)
      : key(key_),
        etag(etag_),
        size(size_),
        mtime(mtime_),
        is_com_prefix(is_com_prefix_) {}
};
using ListObjectsCallback =
    std::function<int(const ListObjectsCBParams &params)>;

struct OpDeleter {
  void operator()(photon::net::http::Client::Operation *x) { x->destroy(); }
};
using HTTP_OP =
    std::unique_ptr<photon::net::http::Client::Operation, OpDeleter>;

class OssSignedUrl;
class OssClient {
 public:
  OssClient(const OssOptions &options,
            CredentialsProvider *credentialsProvider);
  ~OssClient();

  int list_objects(std::string_view prefix, ListObjectsCallback cb,
                   bool delimiter, std::string *context = nullptr,
                   int max_keys = 0);

  int head_object(std::string_view object, ObjectHeaderMeta &meta);

  ssize_t get_object(std::string_view object, const struct iovec *iov,
                     int iovcnt, off_t offset);

  ssize_t put_object(std::string_view object, const struct iovec *iov,
                     int iovcnt, uint64_t *expected_crc64 = nullptr);

  ssize_t append_object(std::string_view object, const struct iovec *iov,
                        int iovcnt, off_t position,
                        uint64_t *expected_crc64 = nullptr);

  int copy_object(std::string_view src_object, std::string_view dst_object,
                  bool overwrite = false, bool set_mime = false);

  int init_multipart_upload(std::string_view object, void **context);

  ssize_t upload_part(void *context, const struct iovec *iov, int iovcnt,
                      int part_number, uint64_t *expected_crc64 = nullptr);

  int upload_part_copy(void *context, off_t offset, size_t count,
                       int part_number);

  int complete_multipart_upload(void *context, uint64_t *expected_crc64);

  int abort_multipart_upload(void *context);

  int delete_objects(std::string_view prefix,
                     const std::vector<std::string> &objects);

  int delete_object(std::string_view obj);

  int rename_object(std::string_view src_path, std::string_view dst_path,
                    bool set_mime = false);

  int check_prefix(std::string_view prefix);

  int get_object_meta(std::string_view obj, ObjectMeta &meta);

 private:
  HTTP_OP make_http_operation(
      photon::net::http::Verb v, OssSignedUrl &oss_url,
      const std::map<estring_view, estring_view> &params = {},
      const std::map<estring_view, estring_view> &headers = {});

  int do_list_objects(std::string_view bucket, std::string_view prefix,
                      ListObjectsCallback cb, bool delimiters, int maxKeys,
                      std::string *marker, std::string *resp_code = nullptr);

  int do_copy_object(OssSignedUrl &src_oss_url, OssSignedUrl &dst_oss_url,
                     bool set_mime = false);
  int do_delete_object(OssSignedUrl &oss_url);

  int do_delete_objects(estring_view bucket, estring_view prefix,
                        const std::vector<std::string> &objects);

  std::string m_endpoint, m_bucket;
  bool m_is_http = false;

  OssOptions m_oss_options;
  photon::net::http::Client *m_client = nullptr;
  CredentialsProvider *m_credentialsProvider = nullptr;
};
}  // namespace OssMiniSdk
}  // namespace FileSystemExt
