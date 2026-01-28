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
#include <photon/common/callback.h>
#include <photon/common/object.h>
#include <photon/common/ordered_span.h>
#include <photon/common/string_view.h>
#include <photon/net/http/headers.h>
#include <photon/net/http/verb.h>
#include <sys/uio.h>

#include <string>
#include <vector>

namespace photon {
namespace objstore {

using StringKV = ordered_string_kv;

std::string_view lookup_mime_type(std::string_view name);

static constexpr int OSS_MAX_PATH_LEN = 1023;

struct ClientOptions {
  std::string endpoint;
  std::string bucket;
  std::string region;
  std::string proxy;
  int max_list_ret_cnt = 1000;
  std::string user_agent = "Photon-ObjStore-Client";
  std::string bind_ips;
  uint64_t request_timeout_us = 60ull * 1000 * 1000;
  int retry_times = 2;

  // When the request timeouts or encounters 5xx error, we will
  // retry the request with times of the base interval.
  // For QPSLimit case, the policy is different. We will retry more
  // times until we have waited the "request time out" period.
  uint64_t retry_base_interval_us = 100'000;
};

struct ObjectMeta {
  uint8_t flags = 0;

  void reset() { flags = 0; }

#define DEFINE_OPTIONAL_FIELD(type, name, flag)                  \
  static_assert((flag) > 0 && ((flag) & ((flag) - 1)) == 0,      \
                "Flag must be a power of two");                  \
  static_assert(((flag) & ~((uint8_t)0xFF)) == 0,                \
                "Flag must fit within uint8_t bit range (0-7)"); \
  type name{};                                                   \
  bool has_##name() const { return flags & (flag); }             \
  void set_##name() { flags |= (flag); }                         \
  void set_##name(const type& value) {                           \
    name = value;                                                \
    flags |= (flag);                                             \
  }                                                              \
  void reset_##name() {                                          \
    name = {};                                                   \
    flags &= ~(flag);                                            \
  }

  DEFINE_OPTIONAL_FIELD(size_t, size, 1)
  DEFINE_OPTIONAL_FIELD(time_t, mtime, 1 << 1)
  DEFINE_OPTIONAL_FIELD(std::string, etag, 1 << 2)
  DEFINE_OPTIONAL_FIELD(std::string, type, 1 << 3)  // Appendable/Normal/...
};

struct ObjectHeaderMeta : public ObjectMeta {
  DEFINE_OPTIONAL_FIELD(std::string, storage_class, 1 << 4)
  DEFINE_OPTIONAL_FIELD(uint64_t, crc64, 1 << 5)

#undef DEFINE_OPTIONAL_FIELD
};

struct ListObjectsParameters {
  uint8_t ver = 2;
  bool slash_delimiter = false;
  uint16_t max_keys = 0;
};

struct ListObjectsCBParameters {
  std::string_view key;
  std::string_view etag;
  std::string_view type;
  size_t size = 0;
  time_t mtime = 0;
  bool is_com_prefix = false;
};

using ListObjectsCallback = Delegate<int, const ListObjectsCBParameters&>;

struct CredentialParameters {
  std::string accessKeyId;
  std::string accessKeySecret;
  std::string securityToken;
};

class Authenticator : Object {
 public:
  struct SignParameters {
    std::string_view region, endpoint, bucket, object;
    StringKV query_params;
    photon::net::http::Verb verb;
    bool invalidate_cache = false;
  };

  virtual int sign(photon::net::http::Headers& headers,
                   const SignParameters& params) = 0;

  virtual const CredentialParameters* get_credentials() { return nullptr; }

  // may be ignored for some implementations
  virtual void set_credentials(CredentialParameters&& credentials) = 0;
};

struct GetObjectParameters {
  std::string_view object;
  const iovec* iov = nullptr;
  int iovcnt = 0;
  off_t offset = 0;

  ObjectHeaderMeta* meta = nullptr;
  int result = -1;
};

class Client : public Object {
 public:
  virtual int list_objects(std::string_view prefix, ListObjectsCallback cb,
                           ListObjectsParameters = {},
                           std::string* marker = nullptr) = 0;

  virtual int head_object(std::string_view object, ObjectHeaderMeta& meta) = 0;

  // return value is the real size if the operation succeeds, otherwise
  // return -1
  virtual ssize_t get_object_range(std::string_view object,
                                   const struct iovec* iov, int iovcnt,
                                   off_t offset,
                                   ObjectHeaderMeta* meta = nullptr) = 0;

  // return value is the object count which data is successfully downloaded.
  // It's possible only some objects get to be downloaded successfully.
  // return -1 if some other errors happen.
  // The batch_get_objects interface works only when whitelisting enabled 
  // at OSS server side.
  virtual int batch_get_objects(std::vector<GetObjectParameters>& params) = 0;

  // return value is the object size if the operation succeeds, otherwise
  // return -1.
  // if expected_crc64 is specified, we will compare the value with the
  // returned object crc64 to validate the object integrity.
  virtual ssize_t put_object(std::string_view object, const struct iovec* iov,
                             int iovcnt,
                             uint64_t* expected_crc64 = nullptr) = 0;

  // return value is the newly appended size if the operation succeeds,
  // otherwise return -1.
  // if expected_crc64 is specified, we will compare the value with the
  // returned object crc64 to validate the object integrity.
  virtual ssize_t append_object(std::string_view object,
                                const struct iovec* iov, int iovcnt,
                                off_t position,
                                uint64_t* expected_crc64 = nullptr) = 0;

  virtual int copy_object(std::string_view src_object,
                          std::string_view dst_object, bool overwrite = false,
                          bool set_mime = false) = 0;

  virtual int init_multipart_upload(std::string_view object,
                                    void** context) = 0;

  // return value is the part size if the operation succeeds, otherwise
  // return -1.
  // if expected_crc64 is specified, we will compare the value with the
  // returned part crc64 to validate the part integrity.
  virtual ssize_t upload_part(void* context, const struct iovec* iov,
                              int iovcnt, int part_number,
                              uint64_t* expected_crc64 = nullptr) = 0;

  virtual int upload_part_copy(void* context, off_t offset, size_t count,
                               int part_number, std::string_view from = {}) = 0;

  // if expected_crc64 is specified, we will compare the value with the
  // returned object crc64 to validate the object integrity.
  virtual int complete_multipart_upload(void* context,
                                        uint64_t* expected_crc64) = 0;

  virtual int abort_multipart_upload(void* context) = 0;

  class MultipartUploadContext {
    Client* _client;
    void* _ctx;

   public:
    MultipartUploadContext(Client* client, void* ctx)
        : _client(client), _ctx(ctx) {}

    ssize_t upload(const struct iovec* iov, int iovcnt, int part_number,
                   uint64_t* expected_crc64 = nullptr) {
      return _client->upload_part(_ctx, iov, iovcnt, part_number,
                                  expected_crc64);
    }

    int copy(off_t offset, size_t count, int part_number,
             std::string_view from = {}) {
      return _client->upload_part_copy(_ctx, offset, count, part_number, from);
    }

    int complete(uint64_t* expected_crc64) {
      return _client->complete_multipart_upload(_ctx, expected_crc64);
    }

    int abort() { return _client->abort_multipart_upload(_ctx); }

    operator bool() { return _ctx; }
  };

  MultipartUploadContext init_multipart_upload(std::string_view object) {
    void* ctx = nullptr;
    init_multipart_upload(object, &ctx);
    return {this, ctx};
  }

  // prefix + objects are to be deleted
  // no slash will be added after the prefix
  virtual int delete_objects(const std::vector<std::string_view>& objects,
                             std::string_view prefix = {}) = 0;

  virtual int delete_object(std::string_view obj) = 0;

  virtual int rename_object(std::string_view src_path,
                            std::string_view dst_path,
                            bool set_mime = false) = 0;

  virtual int put_symlink(std::string_view obj, std::string_view target) = 0;

  virtual int get_symlink(std::string_view obj, std::string& target) = 0;

  virtual int get_object_meta(std::string_view obj, ObjectMeta& meta) = 0;

  virtual void set_credentials(CredentialParameters&& credentials) = 0;
};

Client* new_oss_client(const ClientOptions& opt, Authenticator* auth);

// if cache_ttl_secs is 0, no cache authenticator will be created.
Client* new_oss_client(const ClientOptions& opt, uint32_t cache_ttl_secs = 60,
                       CredentialParameters&& credentials = {});

Authenticator* new_basic_oss_authenticator(
    CredentialParameters&& credentials = {});

// if cache_ttl_secs is 0, the original authenticator will be returned.
Authenticator* new_cached_oss_authenticator(Authenticator* auth,
                                            uint32_t cache_ttl_secs = 60);

// one typical CustomAutheticator example
/*class CustomAuthenticator : public Authenticator {
  Authenticator* auth_ = nullptr;
 public:
  CustomAuthenticator(Authenticator* auth) : auth_(auth) {}

  ~CustomAuthenticator() { delete auth_; }

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
}  // namespace objstore
}  // namespace photon
