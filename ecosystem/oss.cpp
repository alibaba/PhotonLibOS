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

#include "oss.h"

#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/alog.h>
#include <photon/common/estring.h>
#include <photon/common/iovector.h>
#include <photon/common/expirecontainer.h>
#include <photon/ecosystem/simple_dom.h>
#include <photon/net/http/client.h>
#include <photon/net/http/url.h>
#include <photon/net/utils.h>
#include <photon/thread/timer.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <unordered_map>

#include "../common/checksum/digest.h"

// #include "common/faultinjector.h"
// #include "common/logger.h"
// #include "metric/metrics.h"

namespace photon {
namespace objstore {

// unused constants will trigger compile error in photon,
// unless they are defined in a header file
#include "oss_constants.h"

using Verb = net::http::Verb;

using HTTP_STACK_OP = photon::net::http::Client::OperationOnStack<16 * 1024 - 1>;
#define DEFINE_ONSTACK_OP(client, verb, url)                                \
    int _CONCAT(__eno__, __LINE__) = 0;                                     \
    DEFER(errno = _CONCAT(__eno__, __LINE__));                              \
    HTTP_STACK_OP op(client, verb, (url));                                  \
    auto _CONCAT(_CONCAT(__defer__, __LINE__), __1__) =                     \
    make_defer([&]() __INLINE__ { _CONCAT(__eno__, __LINE__) = errno; });

static SimpleDOM::Node get_xml_node(HTTP_STACK_OP &op) {
  auto length = op.resp.headers.content_length();
  if (length > XML_LIMIT)
    LOG_ERROR_RETURN(EINVAL, {}, "xml length limit excceed `", length);
  auto body_buf = (char *)malloc(length + 1);
  auto rc = op.resp.read(body_buf, length);
  body_buf[length] = '\0';
  if (rc != (ssize_t)length) {
    free(body_buf);
    LOG_ERRNO_RETURN(0, {}, "body read error ` `", rc, length);
  }

  try {  // todo try catch inside simple_dom
    auto doc = SimpleDOM::parse(body_buf, length,
                                SimpleDOM::DOC_XML | SimpleDOM::DOC_OWN_TEXT);
    if (!doc) LOG_ERROR_RETURN(0, {}, "failed to parse resp_body");
    return doc;
  } catch (std::exception const &e) {
    free(body_buf);
    LOG_ERROR("got exception when try to parse resp_body `", e.what());
  }
  LOG_ERROR_RETURN(0, {}, "");
}

LogBuffer &operator<<(LogBuffer &log, HTTP_STACK_OP *op) {
  auto reader = get_xml_node(*op);
  auto err = reader["Error"];
  auto Code = err["Code"].to_string_view();
  auto Message = err["Message"].to_string_view();
  return log.printf(" OSSError: ", VALUE(Code), VALUE(Message));
}

// convert oss last modified time, e.g. "Fri, 04 Mar 2022 02:46:25 GMT"
static time_t get_lastmodified(const char *s) {
  struct tm tm;
  memset(&tm, 0, sizeof(struct tm));
  strptime(s, "%a, %d %b %Y %H:%M:%S %Z", &tm);
  return timegm(&tm);  // GMT
}

// convert oss last modified time in listobjects, e.g.
// "2023-10-12T02:03:53.000Z"
static time_t get_list_lastmodified(std::string_view sv) {
  struct tm tm;
  memset(&tm, 0, sizeof(struct tm));
  if (sv.size() != 24) {
    LOG_ERROR("invalid lastmodified time: ", sv);
    return 0;
  }
  if (!strptime(sv.data(), "%Y-%m-%dT%H:%M:%S.000%Z", &tm)) {
    LOG_ERROR("invalid lastmodified time: ", sv);
    return 0;
  }
  return timegm(&tm);  // GMT
}

std::string_view lookup_mime_type(std::string_view name) {
  auto last_pos = name.find_last_of('.');
  if (last_pos != std::string_view::npos)
    name = name.substr(last_pos + 1);

  // extract the last extension
  const size_t MAX_EXT_LENGTH = 8;
  if (name.size() > MAX_EXT_LENGTH)
    name = name.substr(0, MAX_EXT_LENGTH);
  char ext[MAX_EXT_LENGTH];
  std::transform(name.begin(), name.end(), ext, ::tolower);
  return MIME_TYPE_MAP[ext];
}

using photon::net::http::verbstr;
const std::vector<std::string> OSS_METRIC_NAME = []() {
  int sz = verbstr.size();
  std::vector<std::string> ret(sz);
  for (int i = 0; i < sz; i++) {
    ret[i] = "oss" + std::string(verbstr[static_cast<Verb>(i)]) + "Req";
  }
  return ret;
}();

#define make_ccl estring::make_conditional_cat_list
#define IS_FAULT_INJECTION_ENABLED(x) false
#define FAULT_INJECTION(x, y)
#define DECLARE_METRIC_VALUE_WITH_NAMEVAR(a, b, c, d)

class OssUrl {
 public:
  estring m_url, m_raw_object;
  uint64_t m_url_size;
  rstring_view16 m_bucket, m_object;
   OssUrl() {}

  OssUrl(std::string_view endpoint, std::string_view bucket,
               std::string_view object, bool is_http) {
    assert(!bucket.empty());
    init(endpoint, bucket, object, is_http);
  }
  void init(std::string_view endpoint, std::string_view bucket,
            std::string_view object, bool is_http) {
    bool has_slash = (object.size() > 0 && object[0] == '/');
    if (has_slash) object = object.substr(1);

    auto escaped_obj = photon::net::http::url_escape(object);
    m_url.appends(photon::net::http::http_or_s(is_http), bucket, ".", endpoint,
                  "/", escaped_obj);

    auto escaped = escaped_obj.size() > object.size();
    if (escaped) m_raw_object = object;

    m_url_size = m_url.size();
    m_bucket = {(is_http ? 7ul : 8ul), bucket.size()};
    m_object = {m_bucket.offset() + bucket.size() + endpoint.size() + 1 + 1,
                escaped_obj.size()};  // start without prefix /
  }
  estring_view bucket() const { return m_url | m_bucket; }
  estring_view object(bool url_escaped = false) const {
    if (url_escaped || m_raw_object.empty()) {
      return m_url | m_object;
    }
    return m_raw_object;
  }

  estring_view append_params(const StringKV &params) {
    if (!params.empty()) {
      auto it = params.begin();
      m_url.appends("?", it->first,
                    make_ccl(!it->second.empty(), "=", it->second));
      while (++it != params.end()) {
        m_url.appends("&", it->first,
                      make_ccl(!it->second.empty(), "=", it->second));
      }
    }
    return m_url;
  }

  estring_view url() {
    return m_url;
  }
};

static int do_http_call(HTTP_STACK_OP &op, const OssOptions &options,
                        std::string_view object, std::string *code = nullptr) {
  int ret = -1;
  if (IS_FAULT_INJECTION_ENABLED(FileSystem::FI_OssError_Call_Timeout)) {
    LOG_ERROR_RETURN(ETIMEDOUT, ret, "mock oss request timeout");
  }
  int retry_times = options.retry_times;
  auto retry_interval = options.retry_interval_us;
__retry:

  int __saved_errno = errno;
  {
    int r = 0;
    FAULT_INJECTION(FileSystem::FI_OssError_Failed_Without_Call, [&]() {
      r = -1;
      op.status_code = -1;
      __saved_errno = EIO;
    });
    auto start_time = std::chrono::steady_clock::now();
    LOG_DEBUG("Sending oss request ` ` Range[`]", verbstr[op.req.verb()],
              op.req.target(), op.req.headers["Range"]);
    if (r == 0) {
      r = op.call();
      __saved_errno = errno;
    }
    auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - start_time)
                       .count();
    DECLARE_METRIC_VALUE_WITH_NAMEVAR(
        OSS_METRIC_NAME[static_cast<int>(op.req.verb())], latency, 0, 1);
    LOG_DEBUG(
        "Got oss response ` ` Range[`], code=` content_length=` latency_us=`",
        verbstr[op.req.verb()], op.req.target(), op.req.headers["Range"],
        op.resp.status_code(), op.resp.headers.content_length(), latency);
    FAULT_INJECTION(FileSystem::FI_OssError_Call_Failed, [&]() {
      r = -1;
      op.status_code = -1;
      __saved_errno = EIO;
    });
    if (r < 0) {
      if (retry_times-- > 0) {
        photon::thread_usleep(retry_interval);
        retry_interval =
            std::min(retry_interval * 2, options.max_retry_interval_us);
        LOG_ERROR("Retrying oss request ` `", verbstr[op.req.verb()],
                  op.req.target());
        goto __retry;
      }
    }
    FAULT_INJECTION(FileSystem::FI_OssError_5xx, [&]() {
      r = -1;
      op.status_code = 503;
      __saved_errno = EIO;
    });
  }
  if (op.status_code != 200 && op.status_code != 206 && op.status_code != 204) {
    auto reader = get_xml_node(op);
    std::string ec;
    if (reader) {
      auto error_res = reader["Error"];
      if (error_res) {
        if (code) {
          *code = error_res["Code"].to_string_view();
        }
        ec = error_res["EC"].to_string_view();
      }
    }
    if (op.status_code != -1 && op.status_code != 404) {
      LOG_ERROR("operation on [`] failed! `, RequestId[`], EC[`]", object, &op,
                op.resp.headers["x-oss-request-id"], ec);
    }
    if (op.status_code / 100 == 5) {
      if (retry_times-- > 0) {
        photon::thread_usleep(retry_interval);
        retry_interval =
            std::min(retry_interval * 2, options.max_retry_interval_us);
        LOG_ERROR("Retrying oss request ` `", verbstr[op.req.verb()],
                  op.req.target());
        goto __retry;
      }
    }
    switch (op.status_code) {
      case -1:
        LOG_ERROR("operation on [`] failed!, http connection error", object);
        errno = __saved_errno;
        return ret;
      case 400:
      case 409:
        errno = ENOTSUP;
        break;
      case 403:
        errno = EACCES;
        break;
      case 404:
        errno = ENOENT;
        break;
      case 416:
        errno = EINVAL;
        break;
    }
    return ret;
  }
  return 0;
}

static int verify_crc64_if_needed(HTTP_STACK_OP& op, std::string_view object, uint64_t* expected_crc64) {
  if (!expected_crc64) return 0;
  auto it = op.resp.headers.find("x-oss-hash-crc64ecma");
  if (it == op.resp.headers.end()) {
    LOG_WARN("crc64 not found in object ", object);
    return 0;
  }
  uint64_t crc64;
  if (!estring_view(it.second()).to_uint64_check(&crc64) ||
                              *expected_crc64 != crc64) {
    LOG_ERROR_RETURN(EIO, -1,
        "crc64 mismatch of object: `, expected: `, actual: `",
        object, *expected_crc64, it.second());
  }
  return 0;
}


static ssize_t body_writer_cb(void *iov_view, photon::net::http::Request *req) {
  auto view = static_cast<iovector_view *>(iov_view);
  auto ret = req->writev(view->iov, view->iovcnt);
  if (ret != static_cast<ssize_t>(view->sum())) {
    LOG_ERROR_RETURN(0, -1, "stream writev failed!", VALUE(ret), VALUE(errno));
  }
  return 0;
}

static std::string md5_base64(iovector_view view) {
  std::string ret;
  char md[MD5_DIGEST_LENGTH];
#if OPENSSL_VERSION_NUMBER < 0x30000000L
  MD5_CTX ctx;
  MD5_Init(&ctx);
  for (const auto &iov : view) {
    MD5_Update(&ctx, iov.iov_base, iov.iov_len);
  }
  MD5_Final((unsigned char *)md, &ctx);
#else
  EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex(mdctx, EVP_md5(), NULL);
  for (const auto &iov : view) {
    EVP_DigestUpdate(mdctx, iov.iov_base, iov.iov_len);
  }
  unsigned int md5_digest_len = MD5_DIGEST_LENGTH;
  EVP_DigestFinal_ex(mdctx, (unsigned char*)md, &md5_digest_len);
  EVP_MD_CTX_free(mdctx);
#endif
  photon::net::Base64Encode({md, MD5_DIGEST_LENGTH}, ret);
  return ret;
}

class OssClientImpl : public OssClient {
 public:
  OssClientImpl(const OssOptions &options,
            Authenticator *authenticator);
  ~OssClientImpl();

  int list_objects_v2(std::string_view prefix, ListObjectsCallback cb,
            bool delimiter, int max_keys = 0,
            std::string *next_continuation_token = nullptr);
  int list_objects_v1(std::string_view prefix, ListObjectsCallback cb,
            bool delimiter, int max_keys = 0,
            std::string *next_marker = nullptr);

  int head_object(std::string_view object, ObjectHeaderMeta &meta);

  int fill_meta(HTTP_STACK_OP &op, ObjectMeta &meta);
  int fill_meta(HTTP_STACK_OP &op, ObjectHeaderMeta &meta);

  ssize_t get_object_range(std::string_view object, const struct iovec *iov,
          int iovcnt, off_t offset, ObjectHeaderMeta* meta = nullptr);

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
                       int part_number, std::string_view from = "");

  int complete_multipart_upload(void *context, uint64_t *expected_crc64);

  int abort_multipart_upload(void *context);

  int delete_objects(std::string_view prefix,
                     const std::vector<std::string> &objects);

  int delete_object(std::string_view obj);

  int rename_object(std::string_view src_path, std::string_view dst_path,
                    bool set_mime = false);

  int check_prefix_with_list_objects(std::string_view prefix, bool use_list_objects_v2 = true);

  int get_object_meta(std::string_view obj, ObjectMeta &meta);

  void set_credentials(Authenticator::CredentialParameters &&credentials);

 private:
  int append_auth_headers(photon::net::http::Verb v, OssUrl &oss_url,
                          photon::net::http::Headers &headers,
                          const StringKV &query_params = {},
                          bool invalidate_cache = false);

  int walk_list_results(const SimpleDOM::Node &node, ListObjectsCallback cb);
  int do_list_objects_v2(std::string_view bucket, std::string_view prefix,
                         ListObjectsCallback cb, bool delimiters, int maxKeys,
                         std::string *marker, std::string *resp_code = nullptr);
  int do_list_objects_v1(std::string_view bucket, std::string_view prefix,
                         ListObjectsCallback cb, bool delimiters, int maxKeys,
                         std::string *marker, std::string *resp_code = nullptr);

  int do_copy_object(OssUrl &src_oss_url, OssUrl &dst_oss_url,
                     bool overwrite, bool set_mime);
  int do_delete_object(OssUrl &oss_url);

  int do_delete_objects(estring_view bucket, estring_view prefix,
                        const std::vector<std::string> &objects);

  std::string m_endpoint, m_bucket;
  bool m_is_http = false;

  OssOptions m_oss_options;
  photon::net::http::Client *m_client = nullptr;
  Authenticator *m_authenticator = nullptr;
};

#define OssClient OssClientImpl

OssClient::OssClient(const OssOptions &options,
                     Authenticator *authenticator)
    : m_bucket(options.bucket),
      m_oss_options(options),
      m_authenticator(authenticator) {
  estring_view esv(options.endpoint);
  if (esv.starts_with("https://")) {
    m_endpoint = options.endpoint.substr(8);
    m_is_http = false;
  } else if (esv.starts_with("http://")) {
    m_endpoint = options.endpoint.substr(7);
    m_is_http = true;
  } else {
    m_endpoint = options.endpoint;
  }

  m_client = photon::net::http::new_http_client();
  m_client->timeout(m_oss_options.request_timeout_us);
  m_client->set_user_agent(m_oss_options.user_agent.size() ?
    std::string_view(m_oss_options.user_agent) :
    std::string_view("Photon-OSS-Client"));

  if (!options.bind_ips.empty()) {
    std::vector<photon::net::IPAddr> ip_vec;
    auto ips = estring_view(options.bind_ips).split(',');
    for (const auto &ip : ips) {
      photon::net::IPAddr addr(ip.to_string().c_str());
      if (!addr.undefined()) {
        ip_vec.push_back(addr);
      } else {
        LOG_WARN("invalid bind ip: `", ip);
      }
    }

    if (!ip_vec.empty()) {
      m_client->set_bind_ips(ip_vec);
      LOG_INFO("Use customized bind_ips: `", options.bind_ips);
    } else {
      LOG_ERROR("Invalid bind_ips: `, fallback to default mode",
                options.bind_ips);
    }
  }
}

OssClient::~OssClient() {
  delete m_client;
  delete m_authenticator;
}

#undef  OssClient
#define OssClient inline OssClientImpl

int OssClient::append_auth_headers(photon::net::http::Verb v, OssUrl &oss_url,
                                   photon::net::http::Headers &headers,  const StringKV &query_params, bool invalidate_cache) {
  Authenticator::SignParameters params;
  params.verb = v;
  params.query_params = query_params;
  params.region = m_oss_options.region;
  params.endpoint = m_oss_options.endpoint;
  params.bucket = m_oss_options.bucket;
  params.object = oss_url.object();
  params.invalidate_cache = invalidate_cache;

  return m_authenticator->sign(headers, params);
}

int OssClient::walk_list_results(const SimpleDOM::Node &list_bucket_result, ListObjectsCallback cb) {
  for (auto child: list_bucket_result.enumerable_children("Contents")) {
    auto key = child["Key"];
    if (!key) LOG_ERROR_RETURN(EINVAL, -1, "unexpected response");
    auto size = child["Size"];
    if (!size) LOG_ERROR_RETURN(EINVAL, -1, "unexpected response");
    auto mtime = child["LastModified"];
    auto mtim = get_list_lastmodified(mtime.to_string_view());
    auto obj_key = key.to_string_view();
    auto dsize = size.to_int64_t();
    auto etag = child["ETag"].to_string_view();
    auto r = cb({obj_key, etag, (size_t)dsize, mtim, false/*not comm prefix*/});
    if (r < 0) return r;
  }
  for (auto child: list_bucket_result.enumerable_children("CommonPrefixes")) {
    auto key = child["Prefix"];
    if (!key) LOG_ERROR_RETURN(EINVAL, -1, "unexpected response");
    auto com_prefix = key.to_string_view();
    auto r = cb({com_prefix, "", 0, 0, true/*comm prefix*/});
    if (r < 0) return r;
  }
  return 0;
}

class AppendableStringKV : public StringKV {
  size_t _capacity;
public:
  using StringKV::StringKV;
  using StringKV::operator=;
  constexpr AppendableStringKV(std::initializer_list<SKV> l)
      : StringKV(l), _capacity(l.size()) {
    if (empty()) return;
    size_t n = l.size();
    while (n > 0 && (*this)[n - 1].first.empty()) n--;
    *this = subspan(0, n);
  }

  template <size_t N>
  AppendableStringKV(SKV (&arr)[N]) : StringKV(arr), _capacity(N) {
    if (empty()) return;
    size_t n = N;
    while (n > 0 && (*this)[n - 1].first.empty()) n--;
    *this = subspan(0, n);
  }

  void push_back(SKV kv) {
    assert(size() < _capacity);
    if (size() >= _capacity) return;
    *this = AppendableStringKV(this->data(), size()+1);
    (*this)[size()-1] = kv;
  }
  void emplace(std::string_view key, std::string_view val) {
    assert(size() < _capacity);
    if (size() >= _capacity) return;
    *this = AppendableStringKV(this->data(), size()+1);
    auto& arr = *this;
    size_t i = size() - 1;
    for (; i > 0; i--) {
      if (key >= arr[i-1].first) break;
      else arr[i] = arr[i-1];
    }
    arr[i] = {key, val};
  }
};

int OssClient::do_list_objects_v2(std::string_view bucket, std::string_view prefix,
                                  ListObjectsCallback cb, bool delimiters,
                                  int maxKeys, std::string *marker, std::string *resp_code) {
  if (maxKeys > 1000 || maxKeys <= 0) maxKeys = m_oss_options.max_list_ret_cnt;
  estring_view _mark;
  if (marker) _mark = *marker;

  estring escaped_prefix = photon::net::http::url_escape(prefix);
  estring escaped_delimiter = photon::net::http::url_escape("/");
  estring escaped_marker = photon::net::http::url_escape(_mark);
  estring max_key_str = std::to_string(maxKeys);
  // must appear in dictionary order!
  AppendableStringKV query_params = {
      {OSS_PARAM_KEY_LIST_TYPE, "2"},
      {OSS_PARAM_KEY_MAX_KEYS, max_key_str},
      {OSS_PARAM_KEY_PREFIX, escaped_prefix},
      {{},{}}, {{},{}},   // reserve space for later emplace()
  };

  if (delimiters) query_params.emplace(OSS_PARAM_KEY_DELIMITER, escaped_delimiter);
  if (!_mark.empty()) query_params.emplace(OSS_PARAM_KEY_CONTINUATION_TOKEN, escaped_marker);

  OssUrl oss_url(m_endpoint, bucket, "", m_is_http);
  DEFINE_ONSTACK_OP(m_client, Verb::GET, oss_url.append_params(query_params));
  int r = append_auth_headers(Verb::GET, oss_url, op.req.headers, query_params);
  if (r < 0) return r;
  r = do_http_call(op, m_oss_options, "", resp_code);
  if (r < 0) return r;

  auto reader = get_xml_node(op);
  if (!reader) LOG_ERROR_RETURN(0, -1, "failed to parse xml resp_body");
  auto list_bucket_result = reader["ListBucketResult"];
  r = walk_list_results(list_bucket_result, cb);
  if (r < 0) return r;
  if (marker) {
    auto next_marker = list_bucket_result["NextContinuationToken"];
    *marker = next_marker ? next_marker.to_string_view() : "";
  }
  return 0;
}

int OssClient::do_list_objects_v1(std::string_view bucket, std::string_view prefix,
                                  ListObjectsCallback cb, bool delimiters,
                                  int maxKeys, std::string *marker, std::string *resp_code) {
  if (maxKeys > 1000 || maxKeys <= 0) maxKeys = m_oss_options.max_list_ret_cnt;
  estring_view _mark;
  if (marker) _mark = *marker;

  estring escaped_prefix = photon::net::http::url_escape(prefix);
  estring escaped_delimiter = photon::net::http::url_escape("/");
  estring escaped_marker = photon::net::http::url_escape(_mark);
  estring max_key_str = std::to_string(maxKeys);
  // must appear in dictionary order!
  AppendableStringKV query_params = {
      {OSS_PARAM_KEY_MAX_KEYS, max_key_str},
      {OSS_PARAM_KEY_PREFIX, escaped_prefix},
      {{},{}}, {{},{}}, // reserve space for later emplace()
  };
  if (delimiters) query_params.emplace(OSS_PARAM_KEY_DELIMITER, escaped_delimiter);
  if (!_mark.empty()) query_params.emplace(OSS_PARAM_KEY_MARKER, escaped_marker);

  OssUrl oss_url(m_endpoint, bucket, "", m_is_http);
  DEFINE_ONSTACK_OP(m_client, Verb::GET, oss_url.append_params(query_params));
  int r = append_auth_headers(Verb::GET, oss_url, op.req.headers, query_params);
  if (r < 0) return r;
  r = do_http_call(op, m_oss_options, "", resp_code);
  if (r < 0) return r;

  auto reader = get_xml_node(op);
  if (!reader) LOG_ERROR_RETURN(0, -1, "failed to parse xml resp_body");
  auto list_bucket_result = reader["ListBucketResult"];
  r = walk_list_results(list_bucket_result, cb);
  if (r < 0) return r;
  if (marker) {
    auto next_marker = list_bucket_result["NextMarker"];
    *marker = next_marker ? next_marker.to_string_view() : "";
  }
  return 0;
}

int OssClient::do_copy_object(OssUrl &src_oss_url,
                              OssUrl &dst_oss_url, bool overwrite, bool set_mime) {
  DEFINE_ONSTACK_OP(m_client, Verb::PUT, dst_oss_url.url());

  estring oss_copy_source = estring("/").appends(src_oss_url.bucket(), "/",
                                                 src_oss_url.object(true));
  op.req.headers.insert(OSS_HEADER_KEY_X_OSS_COPY_SOURCE, oss_copy_source);
  if (!overwrite) {
    op.req.headers.insert(OSS_HEADER_KEY_X_OSS_FORBID_OVERWRITE, "true");
  }

  std::string_view dst_type = ""; // use the same content type as the source
  if (set_mime) {
    auto src_type = lookup_mime_type(src_oss_url.object());
    auto new_dst_type = lookup_mime_type(dst_oss_url.object());
    if (src_type != new_dst_type) dst_type = new_dst_type;
    if (!dst_type.empty()) {
      op.req.headers.insert(OSS_HEADER_KEY_X_OSS_METADATA_DIRECTIVE, "REPLACE");
      op.req.headers.insert(OSS_HEADER_KEY_CONTENT_TYPE, dst_type);
    }
  }

  int r = append_auth_headers(Verb::PUT, dst_oss_url, op.req.headers);
  if (r < 0) return r;
  r = do_http_call(op, m_oss_options, dst_oss_url.object());
  return r;
}

int OssClient::do_delete_object(OssUrl &oss_url) {
  DEFINE_ONSTACK_OP(m_client, Verb::DELETE, oss_url.url());
  int r = append_auth_headers(Verb::DELETE, oss_url, op.req.headers);
  if (r < 0) return r;
  r = do_http_call(op, m_oss_options, oss_url.object());
  return r;
}

static std::string xml_escape(std::string_view object) {
  static const std::unordered_map<char, std::string> xml_escape_map = {
      {'&', "&amp;"}, {'<', "&lt;"},  {'>', "&gt;"},
      {'"', "&quot;"}, {'\'', "&apos;"},
      // refer to go sdk to handle belowing charactors in deleting multiple
      // objects
      {'\t', "&#x9;"}, {'\n', "&#xA;"},  {'\r', "&#xD;"},
  };

  std::string escaped_object;
  escaped_object.reserve(object.size()*2);
  for (auto c : object) {
    auto it = xml_escape_map.find(c);
    if (it != xml_escape_map.end())
      escaped_object += it->second;
    else
      escaped_object += c;
  }
  return escaped_object;
}

int OssClient::do_delete_objects(estring_view bucket, estring_view prefix,
                                 const std::vector<std::string> &objects) {
  std::string_view req_head = "<Delete><Quiet>true</Quiet>";
  std::string_view req_tail = "</Delete>";
  estring req_list;
  for (size_t i = 1; i <= objects.size(); ++i) {
    req_list.appends("<Object><Key>", xml_escape(prefix),
                     xml_escape(objects[i - 1]), "</Key></Object>");

    // a single request can at most hold 1000 objects
    if (i % 1000 == 0 || i == objects.size()) {
      struct iovec iov[3] = {{(void *)req_head.data(), req_head.size()},
                             {(void *)req_list.data(), req_list.size()},
                             {(void *)req_tail.data(), req_tail.size()}};
      iovector_view body_view(iov, 3);
      OssUrl oss_url(m_endpoint, bucket, "/", m_is_http);
      auto md5 = md5_base64(body_view);

      // must appear in dictionary order!
      const StringKV query_params = {
          {OSS_PARAM_KEY_DELETE, ""}
      };

      DEFINE_ONSTACK_OP(m_client, Verb::POST, oss_url.append_params(query_params));
      op.req.headers.insert(OSS_HEADER_KEY_CONTENT_MD5, md5);
      op.req.headers.content_length(body_view.sum());
      op.body_writer = {&body_view, &body_writer_cb};
      int r = append_auth_headers(Verb::POST, oss_url, op.req.headers, query_params);
      if (r < 0) return r;
      r = do_http_call(op, m_oss_options, oss_url.object());
      if (r < 0) return r;
      req_list.clear();
    }
  }
  return 0;
}

int OssClient::rename_object(std::string_view src_path,
                             std::string_view dst_path, bool set_mime) {
  OssUrl src_oss_url(m_endpoint, m_bucket, src_path, m_is_http);
  OssUrl dst_oss_url(m_endpoint, m_bucket, dst_path, m_is_http);

  if (do_copy_object(src_oss_url, dst_oss_url, true, set_mime) < 0) return -1;
  return do_delete_object(src_oss_url);
}

int OssClient::delete_objects(std::string_view obj_prefix,
                              const std::vector<std::string> &objects) {
  return do_delete_objects(m_bucket, obj_prefix, objects);
}

int OssClient::list_objects_v2(std::string_view prefix, ListObjectsCallback cb,
                               bool delimiter, int max_keys, std::string *nct) {
  std::string marker;
  if (nct) marker = *nct;
  if (max_keys == 0) max_keys = m_oss_options.max_list_ret_cnt;
  do {
    int r =
        do_list_objects_v2(m_bucket, prefix, cb, delimiter, max_keys, &marker);
    if (r < 0) return r;
  } while (!nct && !marker.empty());
  if (nct) *nct = marker;
  return 0;
}

int OssClient::list_objects_v1(std::string_view prefix, ListObjectsCallback cb,
                               bool delimiter, int max_keys, std::string *nm) {
  std::string marker;
  if (nm) marker = *nm;
  if (max_keys == 0) max_keys = m_oss_options.max_list_ret_cnt;
  do {
    int r =
        do_list_objects_v1(m_bucket, prefix, cb, delimiter, max_keys, &marker);
    if (r < 0) return r;
  } while (!nm && !marker.empty());
  if (nm) *nm = marker;
  return 0;
}

int OssClient::head_object(std::string_view object, ObjectHeaderMeta &meta) {
  OssUrl oss_url(m_endpoint, m_bucket, object, m_is_http);
  DEFINE_ONSTACK_OP(m_client, Verb::HEAD, oss_url.url());
  int r = append_auth_headers(Verb::HEAD, oss_url, op.req.headers);
  if (r < 0) return r;
  r = do_http_call(op, m_oss_options, oss_url.object());
  if (r < 0) return r;
  return fill_meta(op, meta);
}

int OssClient::fill_meta(HTTP_STACK_OP &op, ObjectHeaderMeta &meta) {
  int ret = fill_meta(op, (ObjectMeta &)meta);
  if (ret < 0) return ret;

  auto it = op.resp.headers.find("x-oss-object-type");
  if (it != op.resp.headers.end()) {
    meta.set_type();
    meta.type.assign(it.second().data(), it.second().size());
  }

  it = op.resp.headers.find("x-oss-storage-class");
  if (it != op.resp.headers.end()) {
    meta.set_storage_class();
    meta.storage_class.assign(it.second().data(), it.second().size());
  }


  it = op.resp.headers.find("x-oss-hash-crc64ecma");
  if (it != op.resp.headers.end()) {
    meta.set_crc64(estring_view(it.second()).to_uint64());
  }
  return 0;
}

int OssClient::fill_meta(HTTP_STACK_OP &op, ObjectMeta &meta) {
  auto len = op.resp.resource_size();
  if (len == -1) LOG_ERROR_RETURN(0, -1, "Unexpected http response header");

  meta.set_size(len);
  auto it = op.resp.headers.find("Last-Modified");
  if (it != op.resp.headers.end()) {
    if (it.second().empty()) meta.set_mtime(0);
    else meta.set_mtime(get_lastmodified(it.second().data()));
  }

  it = op.resp.headers.find("ETag");
  if (it != op.resp.headers.end()) {
    meta.set_etag();
    meta.etag.assign(it.second().data(), it.second().size());
  }
  return 0;
}

ssize_t OssClient::get_object_range(std::string_view obj_path, const struct iovec *iov,
                              int iovcnt, off_t offset, ObjectHeaderMeta* meta) {
  int retry_times = m_oss_options.retry_times;
  auto retry_interval = m_oss_options.retry_interval_us;

  iovector_view view((struct iovec *)iov, iovcnt);
  auto cnt = view.sum();
  if (cnt == 0) return 0;

  bool invalidate_cache = false;
retry:
  OssUrl oss_url(m_endpoint, m_bucket, obj_path, m_is_http);
  DEFINE_ONSTACK_OP(m_client, Verb::GET, oss_url.url());
  op.req.headers.insert(OSS_HEADER_KEY_X_OSS_RANGE_BEHAVIOR, "standard");
  op.req.headers.range(offset, offset + cnt - 1);
  int r = append_auth_headers(Verb::GET, oss_url, op.req.headers, {}, invalidate_cache);
  if (r < 0) return r;
  r = do_http_call(op, m_oss_options, oss_url.object());
  if (r < 0) {
    if (errno == EACCES && !invalidate_cache) {
      invalidate_cache = true;
      errno = 0;
      goto retry;
    }
    return r;
  }

  uint64_t content_length = op.resp.headers.content_length();
  auto ret = op.resp.readv(iov, iovcnt);
  FAULT_INJECTION(FileSystem::FI_OssError_Read_Partial, [&]() { ret--; });

  // we have encountered partial read issue because of the socket was
  // unexpectedly closed
  if (ret != static_cast<ssize_t>(content_length)) {
    LOG_ERROR("Get object ` return partial data, offset `, expected: `, got: `",
              obj_path, offset, cnt, ret);
    if (retry_times-- > 0) {
      photon::thread_usleep(retry_interval);
      retry_interval =
          std::min(retry_interval * 2, m_oss_options.max_retry_interval_us);
      LOG_ERROR("Retrying oss request ` `", verbstr[op.req.verb()],
                op.req.target());
      //op.reset(nullptr);
      goto retry;
    }
    ret = -1;
    errno = EIO;
  } else {
    if (meta) fill_meta(op, *meta);
  }

  return ret;
}

ssize_t OssClient::put_object(std::string_view object, const struct iovec *iov,
                              int iovcnt, uint64_t *expected_crc64) {
  iovector_view view((struct iovec *)iov, iovcnt);
  auto cnt = view.sum();

  OssUrl oss_url(m_endpoint, m_bucket, object, m_is_http);
  auto content_type = lookup_mime_type(object);

  DEFINE_ONSTACK_OP(m_client, Verb::PUT, oss_url.url());
  if (!content_type.empty()) {
    op.req.headers.insert(OSS_HEADER_KEY_CONTENT_TYPE, content_type);
  }
  op.req.headers.content_length(cnt);
  op.body_writer = {&view, &body_writer_cb};
  int r = append_auth_headers(Verb::PUT, oss_url, op.req.headers);
  if (r < 0) return r;
  r = do_http_call(op, m_oss_options, oss_url.object());
  if (r < 0) return r;
  r = verify_crc64_if_needed(op, oss_url.object(), expected_crc64);
  if (r < 0) return r;
  return cnt;
}

ssize_t OssClient::append_object(std::string_view object,
                                 const struct iovec *iov, int iovcnt,
                                 off_t position, uint64_t *expected_crc64) {
  iovector_view view((struct iovec *)iov, iovcnt);
  auto cnt = view.sum();

  OssUrl oss_url(m_endpoint, m_bucket, object, m_is_http);

  estring position_str = std::to_string(position);

  // must appear in dictionary order!
  StringKV query_params = {
      {OSS_PARAM_KEY_APPEND, ""}, {OSS_PARAM_KEY_POSITION, position_str}};

  DEFINE_ONSTACK_OP(m_client, Verb::POST, oss_url.append_params(query_params));
  auto content_type = lookup_mime_type(object);
  if (!content_type.empty()) {
    op.req.headers.insert(OSS_HEADER_KEY_CONTENT_TYPE, content_type);
  }
  op.req.headers.content_length(cnt);
  op.body_writer = {&view, &body_writer_cb};
  int r = append_auth_headers(Verb::POST, oss_url, op.req.headers, query_params);
  if (r < 0) return r;
  r = do_http_call(op, m_oss_options, oss_url.object());
  if (r < 0) return r;
  r = verify_crc64_if_needed(op, oss_url.object(), expected_crc64);
  if (r < 0) return r;
  return cnt;
}

int OssClient::copy_object(std::string_view src_object,
                           std::string_view dst_object, bool overwrite,
                           bool set_mime) {
  OssUrl src_oss_url(m_endpoint, m_bucket, src_object, m_is_http);
  OssUrl dst_oss_url(m_endpoint, m_bucket, dst_object, m_is_http);

  return do_copy_object(src_oss_url, dst_oss_url, overwrite, set_mime);
}

int OssClient::check_prefix_with_list_objects(std::string_view prefix, bool use_list_objects_v2) {
  std::string res_code;

  auto noop = [](const ListObjectsCBParams &) { return 0; };

  int r = 0;
  if (use_list_objects_v2) {
    r = do_list_objects_v2(m_bucket, prefix, noop, false, 1, nullptr, &res_code);
  }  else {
    r = do_list_objects_v1(m_bucket, prefix, noop, false, 1, nullptr, &res_code);
  }
  if (r != 0) LOG_ERROR("Check bucket prefix failed with err: `", res_code);
  return r;
}

struct oss_multipart_context {
  estring obj_path;
  std::string upload_id;
  std::vector<std::pair<int, std::string>> part_list;
  photon::spinlock lock;
};

int OssClient::init_multipart_upload(std::string_view object,
                                     void **context) {
  OssUrl oss_url(m_endpoint, m_bucket, object, m_is_http);

  // must appear in dictionary order!
  const StringKV query_params = {
    {OSS_PARAM_KEY_UPLOADS, ""}};

  DEFINE_ONSTACK_OP(m_client, Verb::POST, oss_url.append_params(query_params));

  auto content_type = lookup_mime_type(object);
  if (!content_type.empty())
    op.req.headers.insert(OSS_HEADER_KEY_CONTENT_TYPE, content_type);

  int r = append_auth_headers(Verb::POST, oss_url, op.req.headers, query_params);
  if (r < 0) return r;
  r = do_http_call(op, m_oss_options, oss_url.object());
  if (r < 0) return r;

  auto reader = get_xml_node(op);
  if (!reader) LOG_ERROR_RETURN(EINVAL, -1, "failed to parse xml resp_body");
  auto child = reader["InitiateMultipartUploadResult"]["UploadId"];
  if (!child) LOG_ERROR_RETURN(EINVAL, -1, "invalid response with no upload id provided");

  oss_multipart_context *ctx = new oss_multipart_context;
  ctx->obj_path.appends(object);
  ctx->upload_id = child.to_string_view();

  *context = ctx;
  return 0;
}

ssize_t OssClient::upload_part(void *context, const struct iovec *iov,
                               int iovcnt, int part_number,
                               uint64_t *expected_crc64) {
  iovector_view view((struct iovec *)iov, iovcnt);
  auto cnt = view.sum();
  assert(cnt > 0);

  assert(context);

  oss_multipart_context *ctx = (oss_multipart_context *)context;
  assert(!ctx->upload_id.empty());

  estring part_nums_str = std::to_string(part_number);

  // must appear in dictionary order!
  StringKV query_params = {
    {OSS_PARAM_KEY_PART_NUMBER, part_nums_str},
    {OSS_PARAM_KEY_UPLOAD_ID, ctx->upload_id}
  };

  OssUrl oss_url(m_endpoint, m_bucket, ctx->obj_path.c_str(),
                         m_is_http);

  DEFINE_ONSTACK_OP(m_client, Verb::PUT, oss_url.append_params(query_params));

  op.req.headers.content_length(cnt);
  op.body_writer = {&view, &body_writer_cb};
  int r = append_auth_headers(Verb::PUT, oss_url, op.req.headers, query_params);
  if (r < 0) return r;
  r = do_http_call(op, m_oss_options, oss_url.object());
  if (r < 0) return r;

  auto etag = op.resp.headers["ETag"];
  if (etag.empty()) LOG_ERROR_RETURN(EINVAL, -1, "unexpected response", &op);

  r = verify_crc64_if_needed(op, oss_url.object(), expected_crc64);
  if (r < 0) return r;

  SCOPED_LOCK(ctx->lock);
  ctx->part_list.emplace_back(part_number, etag);
  return cnt;
}

int OssClient::upload_part_copy(void *context, off_t offset, size_t count,
                                int part_number, std::string_view from) {
  assert(context);
  oss_multipart_context *ctx = (oss_multipart_context *)context;
  assert(!ctx->upload_id.empty());

  OssUrl oss_url(m_endpoint, m_bucket, ctx->obj_path.c_str(),
                         m_is_http);

  estring part_num_str = std::to_string(part_number);

  // must appear in dictionary order!
  StringKV query_params = {
      {OSS_PARAM_KEY_PART_NUMBER, part_num_str},
      {OSS_PARAM_KEY_UPLOAD_ID, ctx->upload_id}};

  estring oss_copy_source;
  if (from.empty()) { // just copy myself
    oss_copy_source.appends("/", oss_url.bucket(), "/", oss_url.object(true));
  } else {
    oss_copy_source.appends("/", oss_url.bucket(), "/", photon::net::http::url_escape(from));
  }
  std::string range = "bytes=" + std::to_string(offset) + "-" +
                      std::to_string(offset + count - 1);

  DEFINE_ONSTACK_OP(m_client, Verb::PUT, oss_url.append_params(query_params));
  op.req.headers.insert(OSS_HEADER_KEY_X_OSS_COPY_SOURCE, oss_copy_source);
  op.req.headers.insert(OSS_HEADER_KEY_X_OSS_COPY_SOURCE_RANGE, range);
  int r = append_auth_headers(Verb::PUT, oss_url, op.req.headers, query_params);
  if (r < 0) return r;
  r = do_http_call(op, m_oss_options, oss_url.object());
  if (r < 0) return r;

  auto reader = get_xml_node(op);
  if (!reader) LOG_ERROR_RETURN(EINVAL, -1, "failed to parse xml resp_body");
  auto child = reader["CopyPartResult"]["ETag"];
  if (!child) LOG_ERROR_RETURN(EINVAL, -1, "invalid response with no etag provided");

  auto etag = child.to_string_view();
  if (etag.empty()) LOG_ERROR_RETURN(EINVAL, -1, "unexpected response with empty etag", &op);

  SCOPED_LOCK(ctx->lock);
  ctx->part_list.emplace_back(part_number, etag);
  return 0;
}

int OssClient::complete_multipart_upload(void *context,
                                         uint64_t *expected_crc64) {
  assert(context);

  oss_multipart_context *ctx = (oss_multipart_context *)context;
  assert(!ctx->upload_id.empty());

  estring req_body;
  req_body.appends("<CompleteMultipartUpload>");

  std::sort(ctx->part_list.begin(), ctx->part_list.end(),
    [](auto &a, auto &b) { return a.first < b.first; });
  for (auto &part : ctx->part_list) {
    req_body.appends("<Part>"
        "<PartNumber>", part.first, "</PartNumber>"
        "<ETag>", part.second, "</ETag>"
        "</Part>");
  }
  req_body.appends("</CompleteMultipartUpload>");

  struct iovec iov {
    (void *)(req_body.data()), req_body.size()
  };

  iovector_view view(&iov, 1);
  OssUrl oss_url(m_endpoint, m_bucket, ctx->obj_path.c_str(),
                         m_is_http);

  DEFER(delete ctx);

  // must appear in dictionary order!
  StringKV query_params = {
    {OSS_PARAM_KEY_UPLOAD_ID, ctx->upload_id}
  };
  DEFINE_ONSTACK_OP(m_client, Verb::POST, oss_url.append_params(query_params));
  op.req.headers.content_length(req_body.size());
  op.body_writer = {&view, &body_writer_cb};
  int r = append_auth_headers(Verb::POST, oss_url, op.req.headers, query_params);
  if (r < 0) return r;
  r = do_http_call(op, m_oss_options, oss_url.object());
  if (r < 0) return r;
  return verify_crc64_if_needed(op, oss_url.object(), expected_crc64);
}

int OssClient::abort_multipart_upload(void *context) {
  assert(context);

  oss_multipart_context *ctx = (oss_multipart_context *)context;
  assert(!ctx->upload_id.empty());

  OssUrl oss_url(m_endpoint, m_bucket, ctx->obj_path.c_str(),
                         m_is_http);

  DEFER(delete ctx);

  // must appear in dictionary order!
  StringKV query_params = {
    {OSS_PARAM_KEY_UPLOAD_ID, ctx->upload_id}
  };

  DEFINE_ONSTACK_OP(m_client, Verb::DELETE, oss_url.append_params(query_params));
  int r = append_auth_headers(Verb::DELETE, oss_url, op.req.headers, query_params);
  if (r < 0) return r;
  r = do_http_call(op, m_oss_options, oss_url.object());
  return r;
}

int OssClient::get_object_meta(std::string_view object, ObjectMeta &meta) {
  // must appear in dictionary order!
  const StringKV query_params = {
      {OSS_PARAM_KEY_OBJECT_META, ""}};
  OssUrl oss_url(m_endpoint, m_bucket, object, m_is_http);
  DEFINE_ONSTACK_OP(m_client, Verb::HEAD, oss_url.append_params(query_params));
  int r = append_auth_headers(Verb::HEAD, oss_url, op.req.headers, query_params);
  if (r < 0) return r;
  r = do_http_call(op, m_oss_options, oss_url.object());
  if (r < 0) return r;
  return fill_meta(op, meta);
}

int OssClient::delete_object(std::string_view obj_path) {
  OssUrl oss_url(m_endpoint, m_bucket, obj_path, m_is_http);
  return do_delete_object(oss_url);
}

void OssClient::set_credentials(
    Authenticator::CredentialParameters &&credentials) {
  if (m_authenticator) {
    m_authenticator->set_credentials(std::move(credentials));
  }
}

#undef OssClient

class BasicAuthenticator : public Authenticator {
  char m_gmt_date[GMT_DATE_LIMIT];
  char m_gmt_date_iso8601[GMT_DATE_LIMIT];
  time_t m_last_tim = 0;
  CredentialParameters m_credentials;
  void append_query_params(estring &s, const StringKV &params, bool need_question_mark = true) {
    if (!params.empty()) {
      auto it = params.begin();
      s.appends(make_ccl(need_question_mark, "?"), it->first,
                make_ccl(!it->second.empty(), "=", it->second));
      while (++it != params.end()) {
        s.appends("&", it->first,
                  make_ccl(!it->second.empty(), "=", it->second));
      }
    }
  }

  void append_headers(estring &s, const photon::net::http::Headers& header_map, bool v4_signature) {
    for (auto kv : header_map) {
      if (kv.first.substr(0, 5) != "x-oss" &&
          (!v4_signature || (kv.first != OSS_HEADER_KEY_CONTENT_MD5 &&
                             kv.first != OSS_HEADER_KEY_CONTENT_TYPE)))
        continue;
      s.appends(kv.first, ":", kv.second, "\n");
    }
  }

  void sign_v1(photon::net::http::Headers& headers,
               const Authenticator::SignParameters& params, const Authenticator::CredentialParameters& cred) {
    estring auth, data2sign;
    std::string_view ak = cred.accessKey;
    std::string_view sk = cred.accessKeySecret;
    std::string_view token = cred.sessionToken;

    update_gmt_date();

    std::string_view method = photon::net::http::verbstr[params.verb];
    auto content_md5 = headers.get_value(OSS_HEADER_KEY_CONTENT_MD5);
    auto content_type = headers.get_value(OSS_HEADER_KEY_CONTENT_TYPE);

    data2sign.appends(method, "\n", content_md5, "\n", content_type, "\n",
                      m_gmt_date, "\n");

    append_headers(data2sign, headers, false);

    data2sign.appends(
        make_ccl(!token.empty(), "x-oss-security-token:", token, "\n"), "/",
        params.bucket, "/", params.object);
    if (!token.empty()) {
        headers.insert("x-oss-security-token", token);
    }

    // complete this list if needed. currently it's for ossfs use only.
    // must appear in dictionary order!
    const static std::string_view sub_resources[] = {
        OSS_PARAM_KEY_APPEND,      OSS_PARAM_KEY_CONTINUATION_TOKEN,
        OSS_PARAM_KEY_DELETE,      OSS_PARAM_KEY_OBJECT_META,
        OSS_PARAM_KEY_PART_NUMBER, OSS_PARAM_KEY_POSITION,
        OSS_PARAM_KEY_UPLOAD_ID,   OSS_PARAM_KEY_UPLOADS,
    };

    if (params.query_params.size()) {
      SKV _skv[LEN(sub_resources)] = {};
      AppendableStringKV sub_params(_skv);

      for (auto x: sub_resources) { // x is ordered
        auto it = params.query_params.find(x);
        if (it != params.query_params.end()) {
          sub_params.emplace(it->first, it->second);
        }
      }
      append_query_params(data2sign, sub_params);
    }
    // LOG_INFO("data2sign is `", data2sign);

    estring signature;
    net::Base64Encode(HMAC_SHA1(sk, data2sign), signature);

    auth.appends("OSS ", ak, ":", signature);
    headers.insert("Authorization", auth);
    headers.insert("Date", m_gmt_date);
  }

  void sign_v4(photon::net::http::Headers& headers,
               const Authenticator::SignParameters& params, const Authenticator::CredentialParameters& cred) {
    estring auth, canonical_request, string2sign, signing_key;
    std::string_view ak = cred.accessKey;
    std::string_view sk = cred.accessKeySecret;
    std::string_view token = cred.sessionToken;

    update_gmt_date();

    std::string_view method = photon::net::http::verbstr[params.verb];
    canonical_request.appends(method, "\n", "/", params.bucket, "/",
                              photon::net::http::url_escape(params.object, false), "\n");
    if (params.query_params.size()) {
      append_query_params(canonical_request, params.query_params, false);
    }

    canonical_request.appends("\n");

    // headers are aloso sorted in lexicographical order
    // header1:value1\n
    // header2:value2\n
    // StringKV header_map = headers;
    headers.insert("x-oss-content-sha256", "UNSIGNED-PAYLOAD");
    headers.insert("x-oss-date", m_gmt_date_iso8601);
    if (!token.empty()) {
      headers.insert("x-oss-security-token", token);
    }
    append_headers(canonical_request, headers, true);

    canonical_request.appends("\n", "\n"/*no additional headers*/,
                              "UNSIGNED-PAYLOAD");

    string2sign.appends("OSS4-HMAC-SHA256\n", m_gmt_date_iso8601, "\n",
                        estring_view(m_gmt_date_iso8601, 8), "/", params.region,
                        "/oss/aliyun_v4_request\n",
                        sha256_hash(canonical_request));

    auto signature = hex(HMAC_SHA256(
        HMAC_SHA256(
            HMAC_SHA256(
                HMAC_SHA256(HMAC_SHA256(estring("aliyun_v4").appends(sk),
                                        estring_view(m_gmt_date_iso8601, 8)),
                            params.region),
                "oss"),
            "aliyun_v4_request"),
        string2sign));

    // LOG_INFO("string2sing ` canonical request `", string2sign, canonical_request);

    // Authorization: "OSS4-HMAC-SHA256 Credential=" + AccessKeyId + "/" +
    // SignDate + "/" + SignRegion + "/oss/aliyun_v4_request," + [
    // "AdditionalHeaders=" + AdditionalHeadersVal + "," ] + "Signature=" +
    // SignatureVal
    auth.appends("OSS4-HMAC-SHA256 Credential=", ak, "/",
                estring_view(m_gmt_date_iso8601, 8), "/", params.region,
                "/oss/aliyun_v4_request,", "Signature=", signature);
    headers.insert("Authorization", auth);
  }

  std::string hex(std::string_view bytes) {
    static const std::string hex_map = "0123456789abcdef";
    std::string hex_data;
    hex_data.reserve(bytes.size() * 2);
    for (unsigned char c: bytes) {
      hex_data += hex_map[c >> 4];
      hex_data += hex_map[c & 0xf];
    }
    return hex_data;
  }

  std::string sha256_hash(std::string_view data) {
#if OPENSSL_VERSION_NUMBER < 0x30000000L
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, data.data(), data.size());
    SHA256_Final(hash, &sha256);
#else
    unsigned char hash[SHA256_DIGEST_LENGTH] = {0};
    SHA256((unsigned char *)data.data(), data.size(), hash);
#endif
    return hex({(char *)hash, SHA256_DIGEST_LENGTH});
  }

  void update_gmt_date() {  // avoid updating GMT Time every time
    time_t t = photon::now / 1000 / 1000;
    if (t - m_last_tim > GMT_UPDATE_INTERVAL || m_last_tim == 0) {
      m_last_tim = t;
      std::time_t tm = std::time(nullptr);
      struct tm *p = gmtime(&tm);
      strftime(m_gmt_date, GMT_DATE_LIMIT, "%a, %d %b %Y %H:%M:%S %Z", p);
      strftime(m_gmt_date_iso8601, GMT_DATE_LIMIT, "%Y%m%dT%H%M%SZ", p);
    }
  }

 public:
  BasicAuthenticator(CredentialParameters &&credentials)
      : m_credentials(std::move(credentials)) {}

  void set_credentials(CredentialParameters &&credentials) {
    m_credentials = credentials;
  }

  int sign(photon::net::http::Headers &headers,
           const Authenticator::SignParameters &params) {
    if (params.region.empty()) {
      sign_v1(headers, params, m_credentials);
    } else {
      sign_v4(headers, params, m_credentials);
    }
    return 0;
  }
};

class CachedAuthenticator : public Authenticator {
  Authenticator* m_auth = nullptr;
  int m_cached_ttl_secs = 300;
  using CachedObjHeader = std::vector<std::pair<std::string, std::string>>;
  std::shared_ptr<ObjectCache<std::string, CachedObjHeader *>> m_cached_headers;

 public:
  CachedAuthenticator(Authenticator *auth, int cache_ttl_secs)
      : m_auth(auth),
        m_cached_ttl_secs(cache_ttl_secs),
        m_cached_headers(
            std::make_shared<ObjectCache<std::string, CachedObjHeader *>>(
                1000ll * 1000 * cache_ttl_secs)) {}
  ~CachedAuthenticator() { delete m_auth; }
  int sign(photon::net::http::Headers &headers,
           const Authenticator::SignParameters &params) override {
    // only get obj range request among our supported interfaces will fall into
    // this catagory currently
    bool can_use_cache = params.verb == Verb::GET &&
                         params.query_params.empty();
    if (!can_use_cache) return m_auth->sign(headers, params);

    std::string cached_key =
        estring().appends(params.bucket, "/", params.object);

    static const std::vector<std::string_view> to_cache_keys = {
        "Authorization", "x-oss-security-token", "x-oss-date",
        "x-oss-content-sha256", "Date"/*v1 signature needed*/};

    bool evict_cache = params.invalidate_cache;
    int r = 0;

    auto ctor = [&]() -> CachedObjHeader * {
      if (evict_cache) evict_cache = false;
      r = m_auth->sign(headers, params);
      if (r != 0) return nullptr;
      auto *cached = new CachedObjHeader();
      for (const auto &k : to_cache_keys) {
        auto it = headers.find(k);
        if (it != headers.end()) {
          cached->emplace_back(k, it.second());
        }
      }
      // LOG_INFO("cached headers for key ` `", cached_key,  cached);
      return cached;
    };

    bool should_retry = false;
    do {
      should_retry = false;
      auto cached = m_cached_headers->borrow(cached_key, ctor);
      if (cached) {
        if (evict_cache) {
          // LOG_INFO("recycling cached headers for key `", cached_key);
          cached.recycle(true);
          evict_cache = false;
          should_retry = true;
          continue;
        }

        if (!cached->empty()) {
          for (const auto &kv : *cached) {
            headers.insert(kv.first, kv.second);
          }
          // LOG_INFO("using cached headers for key `", cached_key);
          return 0;
        }
      }
    } while (should_retry);
    return r;
  }

  void set_credentials(CredentialParameters &&credentials) override {
    // create one new cache instance to discard all the old entries
    m_cached_headers =
        std::make_shared<ObjectCache<std::string, CachedObjHeader *>>(
                               1000ll * 1000 * m_cached_ttl_secs);
    m_auth->set_credentials(std::move(credentials));
  }
};

OssClient *new_oss_client(const OssOptions &opt, Authenticator *auth) {
  return new OssClientImpl(opt, auth);
}

Authenticator* new_basic_authenticator(
    Authenticator::CredentialParameters&& credentials) {
  return new BasicAuthenticator(std::move(credentials));
}

Authenticator *new_cached_authenticator(Authenticator *auth, int cache_ttl_secs) {
  return new CachedAuthenticator(auth, cache_ttl_secs);
}

}
}
