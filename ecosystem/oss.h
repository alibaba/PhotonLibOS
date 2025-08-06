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
#include <photon/common/object.h>
#include <photon/common/callback.h>
#include <photon/net/http/client.h>
#include <photon/common/string_view.h>

namespace photon {
namespace objstore {

struct OssOptions {
  std::string endpoint;
  std::string bucket;
  std::string region;
  bool use_list_obj_v2 = true;
  int max_list_ret_cnt = 1000;
  std::string user_agent;       // "Photon-OSS-Client" by default
  std::string bind_ips;
  uint64_t request_timeout_us = 60ull * 1000 * 1000;
};

struct OSSCredentials {
  std::string m_accessKeyId;
  std::string m_accessKeySecret;
  std::string m_sessionToken;
};

class CredentialsProvider : Object {
 public:
  virtual OSSCredentials getCredentials() = 0;
  virtual void setCredentials(const OSSCredentials &cred) {}
};

struct ObjectMeta {
  size_t size = 0;
  time_t mtime = 0;
  std::string etag;
};

struct ObjectHeaderMeta : public ObjectMeta {
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
};

using ListObjectsCallback = Delegate<int, const ListObjectsCBParams&>;

class OssClient : public Object {
 public:

  virtual
  int list_objects(std::string_view prefix, ListObjectsCallback cb,
                   bool delimiter, std::string *context = nullptr,
                   int max_keys = 0) = 0;

  virtual
  int head_object(std::string_view object, ObjectHeaderMeta &meta) = 0;

  virtual
  ssize_t get_object(std::string_view object, const struct iovec *iov,
          int iovcnt, off_t offset, ObjectHeaderMeta* meta = nullptr) = 0;

  virtual
  ssize_t put_object(std::string_view object, const struct iovec *iov,
                     int iovcnt, uint64_t *expected_crc64 = nullptr) = 0;

  virtual
  ssize_t append_object(std::string_view object, const struct iovec *iov,
                        int iovcnt, off_t position,
                        uint64_t *expected_crc64 = nullptr) = 0;

  virtual
  int copy_object(std::string_view src_object, std::string_view dst_object,
                  bool overwrite = false, bool set_mime = false) = 0;

  virtual
  int init_multipart_upload(std::string_view object, void **context) = 0;

  virtual
  ssize_t upload_part(void *context, const struct iovec *iov, int iovcnt,
                      int part_number, uint64_t *expected_crc64 = nullptr) = 0;

  virtual
  int upload_part_copy(void *context, off_t offset, size_t count,
                       int part_number) = 0;

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

    int copy(off_t offset, size_t count, int part_number) {
      return _client->upload_part_copy(_ctx, offset, count, part_number);
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

  virtual
  int delete_objects(std::string_view prefix,
                     const std::vector<std::string> &objects) = 0;

  virtual
  int delete_object(std::string_view obj) = 0;

  virtual
  int rename_object(std::string_view src_path, std::string_view dst_path,
                    bool set_mime = false) = 0;

  virtual
  int check_prefix(std::string_view prefix) = 0;

  virtual
  int get_object_meta(std::string_view obj, ObjectMeta &meta) = 0;
};

OssClient* new_oss_client(const OssOptions& opt, CredentialsProvider* cp);

}  // namespace OssMiniSdk
}  // namespace FileSystemExt
