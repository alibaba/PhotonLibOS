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
#include <inttypes.h>
#include <string>
#include <vector>
#include <sys/uio.h>
#include <photon/common/object.h>
#include <photon/common/callback.h>
#include <photon/common/string_view.h>
#include <photon/net/http/headers.h>
#include <photon/net/http/verb.h>
#include <photon/common/ordered_span.h>

namespace photon {
namespace objstore {

using StringKV = ordered_string_kv;

std::string_view lookup_mime_type(std::string_view name);

static constexpr int OSS_MAX_PATH_LEN = 1023;

struct OssOptions {
  std::string endpoint;
  std::string bucket;
  std::string region;
  int max_list_ret_cnt = 1000;
  std::string user_agent;       // "Photon-OSS-Client" by default
  std::string bind_ips;
  uint64_t request_timeout_us = 60ull * 1000 * 1000;
  int retry_times = 2;
  uint64_t retry_interval_us = 20000ULL;
  uint64_t max_retry_interval_us = 1000000ULL;
};

struct ObjectMeta {
  uint8_t flags = 0;

  void reset() { flags = 0; }

#define DEFINE_OPTIONAL_FIELD(type, name, flag)                   \
  static_assert((flag) > 0 && ((flag) & ((flag) - 1)) == 0,       \
                "Flag must be a power of two");                   \
  static_assert(((flag) & ~((uint8_t)0xFF)) == 0,                 \
                "Flag must fit within uint8_t bit range (0-7)");  \
  type name{};                                                    \
  bool has_##name() const { return flags & (flag); }              \
  void set_##name() { flags |= (flag); }                          \
  void set_##name(const type &value) {                            \
    name = value;                                                 \
    flags |= (flag);                                              \
  }                                                               \
  void reset_##name() {                                           \
    name = {};                                                    \
    flags &= ~(flag);                                             \
  }

  DEFINE_OPTIONAL_FIELD(size_t, size, 1)
  DEFINE_OPTIONAL_FIELD(time_t, mtime, 1 << 1)
  DEFINE_OPTIONAL_FIELD(std::string, etag, 1 << 2)
};

struct ObjectHeaderMeta : public ObjectMeta {
  DEFINE_OPTIONAL_FIELD(std::string, type, 1 << 3)  // Appendable/Normal/...
  DEFINE_OPTIONAL_FIELD(std::string, storage_class, 1 << 4)
  DEFINE_OPTIONAL_FIELD(uint64_t, crc64, 1 << 5)

#undef DEFINE_OPTIONAL_FIELD
};

struct ListObjectsCBParams {
  std::string_view key;
  std::string_view etag;
  size_t size = 0;
  time_t mtime = 0;
  bool is_com_prefix = false;
};

using ListObjectsCallback = Delegate<int, const ListObjectsCBParams&>;

class Authenticator : Object {
 public:
  struct SignParameters {
    std::string_view region, endpoint, bucket, object;
    StringKV query_params;
    photon::net::http::Verb verb;
    bool invalidate_cache = false;
  };

  virtual int sign(photon::net::http::Headers &headers,
                   const SignParameters &params) = 0;

  struct CredentialParameters {
    std::string accessKey;
    std::string accessKeySecret;
    std::string sessionToken;
  };

  virtual const CredentialParameters* get_credentials() {
    return nullptr;
  }

  // may be ignored for some implementations
  virtual void set_credentials(CredentialParameters &&credentials) = 0;
};

class OssClient : public Object {
 public:

  virtual int list_objects_v2(std::string_view prefix, ListObjectsCallback cb,
              bool delimiter, int max_keys = 0,
              std::string *next_continuation_token = nullptr) = 0;

  virtual int list_objects_v1(std::string_view prefix, ListObjectsCallback cb,
              bool delimiter, int max_keys = 0, std::string *marker = nullptr) = 0;

  int list_objects(std::string_view prefix, ListObjectsCallback cb,
        bool delimiter, int max_keys = 0, std::string *marker = nullptr) {
    return list_objects_v2(prefix, cb, delimiter, max_keys, marker);
  }

  virtual
  int head_object(std::string_view object, ObjectHeaderMeta &meta) = 0;

  // return value is the real size if the operation successfully, otherwise return -1
  virtual
  ssize_t get_object_range(std::string_view object, const struct iovec *iov,
          int iovcnt, off_t offset, ObjectHeaderMeta* meta = nullptr) = 0;

  // return value is the object size if the operation successfully, otherwise return -1
  virtual
  ssize_t put_object(std::string_view object, const struct iovec *iov,
                     int iovcnt, uint64_t *expected_crc64 = nullptr) = 0;

  // return value is the newly appended size if the operation successfully, otherwise return -1
  virtual
  ssize_t append_object(std::string_view object, const struct iovec *iov,
                        int iovcnt, off_t position,
                        uint64_t *expected_crc64 = nullptr) = 0;

  virtual
  int copy_object(std::string_view src_object, std::string_view dst_object,
                  bool overwrite = false, bool set_mime = false) = 0;

  virtual
  int init_multipart_upload(std::string_view object, void **context) = 0;

  // return value is the part size if the operation successfully, otherwise return -1
  virtual
  ssize_t upload_part(void *context, const struct iovec *iov, int iovcnt,
                      int part_number, uint64_t *expected_crc64 = nullptr) = 0;

  virtual
  int upload_part_copy(void *context, off_t offset, size_t count,
                       int part_number, std::string_view from = "") = 0;

  virtual
  int complete_multipart_upload(void *context, uint64_t *expected_crc64) = 0;

  virtual
  int abort_multipart_upload(void *context) = 0;

  class MultipartUploadContext {
    OssClient* _client;
    void* _ctx;
   public:
    MultipartUploadContext(OssClient* client, void* ctx) :
      _client(client), _ctx(ctx) { }

    ssize_t upload(const struct iovec *iov, int iovcnt, int part_number,
                                  uint64_t *expected_crc64 = nullptr) {
      return _client->upload_part(_ctx, iov, iovcnt, part_number, expected_crc64);
    }

    int copy(off_t offset, size_t count, int part_number, std::string_view from = "") {
      return _client->upload_part_copy(_ctx, offset, count, part_number, from);
    }

    int complete(uint64_t *expected_crc64) {
      return _client->complete_multipart_upload(_ctx, expected_crc64);
    }

    int abort() {
      return _client->abort_multipart_upload(_ctx);
    }

    operator bool() { return _ctx; }
  };

  MultipartUploadContext init_multipart_upload(std::string_view object) {
    void* ctx = nullptr;
    init_multipart_upload(object, &ctx);
    return {this, ctx};
  }

  // prefix + objects are to be deleted
  // no slash will be added after the prefix
  virtual
  int delete_objects(std::string_view prefix,
                     const std::vector<std::string> &objects) = 0;

  virtual
  int delete_object(std::string_view obj) = 0;

  virtual
  int rename_object(std::string_view src_path, std::string_view dst_path,
                    bool set_mime = false) = 0;

  virtual
  int check_prefix_with_list_objects(std::string_view prefix, bool list_objects_v2 = true) = 0;

  virtual
  int get_object_meta(std::string_view obj, ObjectMeta &meta) = 0;

  virtual void set_credentials(
      Authenticator::CredentialParameters &&credentials) = 0;
};

OssClient* new_oss_client(const OssOptions& opt, Authenticator* cp);

Authenticator *new_basic_authenticator(
    Authenticator::CredentialParameters &&credentials);

Authenticator *new_cached_authenticator(Authenticator *auth,
                                        int cache_ttl_secs = 60);

//add one typical CustomAutheticator example
/*class CustomAuthenticator : public Authenticator {
  Authenticator* auth_ = nullptr;
 public:
  CustomAuthenticator(Authenticator* auth) : auth_(auth) {}
  virtual int sign(photon::net::http::Headers& headers,
                   const SignParameters& params) override {
    // add your own logic here
    return auth_->sign(headers, params);
  }

  virtual const CredentialParameters* get_credentials() override {
    // add your own logic here
    return auth_->get_credentials();
  }

  virtual void set_credentials(CredentialParameters&& credentials) override {
    auth_->set_credentials(std::move(credentials));
  }
};*/
}
}
