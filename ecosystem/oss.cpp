// #pragma GCC diagnostic push
// #pragma GCC diagnostic ignored "-Wpacked-bitfield-compat"

#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/hmac.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <photon/common/estring.h>
#include <photon/thread/timer.h>
#include <time.h>
#include <photon/ecosystem/simple_dom.h>

#include <map>
#include <unordered_set>
#include <string>
#include <unistd.h>

#include "common/faultinjector.h"
#include "common/logger.h"
#include "ossfs.h"
#include "photon/common/iovector.h"
#include "photon/fs/path.h"
#include "photon/net/http/url.h"
#include "photon/net/utils.h"
#include "metric/metrics.h"

using Verb = photon::net::http::Verb;
using FileSystemExt::OssMiniSdk::HTTP_OP;

static constexpr int XML_LIMIT = 16 * 1024 * 1024;
static photon::SimpleDOM::Node get_xml_node(const HTTP_OP &op) {
  auto length = op->resp.headers.content_length();
  if (length > XML_LIMIT) LOG_ERROR_RETURN(EINVAL, {}, "xml length limit excceed `", length);
  std::string body_buf;
  body_buf.resize(length);
  auto rc = op->resp.read(&body_buf[0], length);
  if (rc != (ssize_t)length) LOG_ERROR_RETURN(0, {}, "body read error ` `", rc, length);
  try {
    auto doc = photon::SimpleDOM::parse_copy(body_buf.data(), body_buf.size(),
                                             photon::SimpleDOM::DOC_XML);
    if (!doc) LOG_ERROR_RETURN(0, {}, "failed to parse resp_body");
    return doc;
  } catch (std::exception const &e) {
    LOG_ERROR("got exception when try to parse resp_body `", e.what());
  }
  LOG_ERROR_RETURN(0, {}, "");
}

LogBuffer &operator<<(LogBuffer &log, const HTTP_OP &op) {
  auto reader = get_xml_node(op);
  auto err = reader["Error"];
  return log.printf(" OSSError: Code[", err["Code"].to_string_view(), "], Message[",
                    err["Message"].to_string_view(), "]");
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

namespace FileSystemExt {
namespace OssMiniSdk {

// replace this later when photon is updated and has
// unordered_map_string_key_case_insensitive.
const std::map<std::string, std::string> MIME_TYPE_MAP = {
  {"html", "text/html"},
  {"htm", "text/html"},
  {"shtml", "text/html"},
  {"css", "text/css"},
  {"xml", "text/xml"},
  {"gif", "image/gif"},
  {"jpeg", "image/jpeg"},
  {"jpg", "image/jpeg"},
  {"js", "application/x-javascript"},
  {"atom", "application/atom+xml"},
  {"rss", "application/rss+xml"},
  {"mml", "text/mathml"},
  {"txt", "text/plain"},
  {"jad", "text/vnd.sun.j2me.app-descriptor"},
  {"wml", "text/vnd.wap.wml"},
  {"htc", "text/x-component"},
  {"png", "image/png"},
  {"tif", "image/tiff"},
  {"tiff", "image/tiff"},
  {"wbmp", "image/vnd.wap.wbmp"},
  {"ico", "image/x-icon"},
  {"jng", "image/x-jng"},
  {"bmp", "image/x-ms-bmp"},
  {"svg", "image/svg+xml"},
  {"svgz", "image/svg+xml"},
  {"webp", "image/webp"},
  {"jar", "application/java-archive"},
  {"war", "application/java-archive"},
  {"ear", "application/java-archive"},
  {"hqx", "application/mac-binhex40"},
  {"doc ", "application/msword"},
  {"pdf", "application/pdf"},
  {"ps", "application/postscript"},
  {"eps", "application/postscript"},
  {"ai", "application/postscript"},
  {"rtf", "application/rtf"},
  {"xls", "application/vnd.ms-excel"},
  {"ppt", "application/vnd.ms-powerpoint"},
  {"wmlc", "application/vnd.wap.wmlc"},
  {"kml", "application/vnd.google-earth.kml+xml"},
  {"kmz", "application/vnd.google-earth.kmz"},
  {"7z", "application/x-7z-compressed"},
  {"cco", "application/x-cocoa"},
  {"jardiff", "application/x-java-archive-diff"},
  {"jnlp", "application/x-java-jnlp-file"},
  {"run", "application/x-makeself"},
  {"pl", "application/x-perl"},
  {"pm", "application/x-perl"},
  {"prc", "application/x-pilot"},
  {"pdb", "application/x-pilot"},
  {"rar", "application/x-rar-compressed"},
  {"rpm", "application/x-redhat-package-manager"},
  {"sea", "application/x-sea"},
  {"swf", "application/x-shockwave-flash"},
  {"sit", "application/x-stuffit"},
  {"tcl", "application/x-tcl"},
  {"tk", "application/x-tcl"},
  {"der", "application/x-x509-ca-cert"},
  {"pem", "application/x-x509-ca-cert"},
  {"crt", "application/x-x509-ca-cert"},
  {"xpi", "application/x-xpinstall"},
  {"xhtml", "application/xhtml+xml"},
  {"zip", "application/zip"},
  {"wgz", "application/x-nokia-widget"},
  {"bin", "application/octet-stream"},
  {"exe", "application/octet-stream"},
  {"dll", "application/octet-stream"},
  {"deb", "application/octet-stream"},
  {"dmg", "application/octet-stream"},
  {"eot", "application/octet-stream"},
  {"iso", "application/octet-stream"},
  {"img", "application/octet-stream"},
  {"msi", "application/octet-stream"},
  {"msp", "application/octet-stream"},
  {"msm", "application/octet-stream"},
  {"mid", "audio/midi"},
  {"midi", "audio/midi"},
  {"kar", "audio/midi"},
  {"mp3", "audio/mpeg"},
  {"ogg", "audio/ogg"},
  {"m4a", "audio/x-m4a"},
  {"ra", "audio/x-realaudio"},
  {"3gpp", "video/3gpp"},
  {"3gp", "video/3gpp"},
  {"mp4", "video/mp4"},
  {"mpeg", "video/mpeg"},
  {"mpg", "video/mpeg"},
  {"mov", "video/quicktime"},
  {"webm", "video/webm"},
  {"flv", "video/x-flv"},
  {"m4v", "video/x-m4v"},
  {"mng", "video/x-mng"},
  {"asx", "video/x-ms-asf"},
  {"asf", "video/x-ms-asf"},
  {"wmv", "video/x-ms-wmv"},
  {"avi", "video/x-msvideo"},
  {"ts", "video/MP2T"},
  {"m3u8", "application/x-mpegURL"},
  {"apk", "application/vnd.android.package-archive"},
};

static std::string_view lookup_mime_type(std::string_view name) {
  auto last_pos = name.find_last_of('.');
  if (last_pos == std::string_view::npos || last_pos == name.size() - 1) {
    return "";
  }

  // extract the last extension
  auto ext = std::string(name.substr(last_pos + 1));
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  auto iter = MIME_TYPE_MAP.find(ext);
  if (iter != MIME_TYPE_MAP.end()) {
    return (*iter).second;
  }
  return "";
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
#define DO_CALL_WITH_RESP_CODE(op, ret, ossurl, code)                          \
  if (IS_FAULT_INJECTION_ENABLED(FileSystem::FI_OssError_Call_Timeout)) {      \
    LOG_ERROR_RETURN(ETIMEDOUT, ret, "mock oss request timeout");              \
  }                                                                            \
  int __retry_times = OSS_REQUEST_RETRY_TIMES;                                 \
  auto __retry_interval = OSS_REQUEST_RETRY_INTERVAL_US;                       \
  __retry:                                                                     \
                                                                               \
  int __saved_errno = errno;                                                   \
  {                                                                            \
    int r = 0;                                                                 \
    FAULT_INJECTION(FileSystem::FI_OssError_Failed_Without_Call, [&]() {       \
      r = -1;                                                                  \
      op->status_code = -1;                                                    \
      __saved_errno = EIO;                                                     \
    });                                                                        \
    auto start_time = std::chrono::steady_clock::now();                        \
    LOG_DEBUG("Sending oss request ` ` Range[`]", verbstr[op->req.verb()],     \
              op->req.target(), op->req.headers["Range"]);                     \
    if (r == 0) {                                                              \
      r = op->call();                                                          \
      __saved_errno = errno;                                                   \
    }                                                                          \
    auto latency = std::chrono::duration_cast<std::chrono::microseconds>(      \
        std::chrono::steady_clock::now() - start_time).count();                \
    DECLARE_METRIC_VALUE_WITH_NAMEVAR(                                         \
        OSS_METRIC_NAME[static_cast<int>(op->req.verb())], latency, 0, 1);     \
    LOG_DEBUG(                                                                 \
        "Got oss response ` ` Range[`], code=` content_length=` latency_us=`", \
        verbstr[op->req.verb()], op->req.target(), op->req.headers["Range"],   \
        op->resp.status_code(), op->resp.headers.content_length(), latency);   \
    FAULT_INJECTION(FileSystem::FI_OssError_Call_Failed, [&]() {               \
      r = -1;                                                                  \
      op->status_code = -1;                                                    \
      __saved_errno = EIO;                                                     \
    });                                                                        \
    if (r < 0) {                                                               \
      if (__retry_times-- > 0) {                                               \
        photon::thread_usleep(__retry_interval);                               \
        __retry_interval =                                                     \
            std::min(__retry_interval * 2, OSS_REQUEST_MAX_RETRY_INTERVAL_US); \
        LOG_ERROR("Retrying oss request ` `", verbstr[op->req.verb()],         \
                  op->req.target());                                           \
        goto __retry;                                                          \
      }                                                                        \
    }                                                                          \
    FAULT_INJECTION(FileSystem::FI_OssError_5xx, [&]() {                       \
      r = -1;                                                                  \
      op->status_code = 503;                                                   \
      __saved_errno = EIO;                                                     \
    });                                                                        \
  }                                                                            \
  if (op->status_code != 200 && op->status_code != 206 &&                      \
      op->status_code != 204) {                                                \
    auto reader = get_xml_node(op);                                            \
    std::string ec;                                                            \
    if (reader) {                                                              \
      auto error_res = reader["Error"];                                        \
      if (error_res) {                                                         \
        if (code) {                                                            \
          *code =error_res["Code"].to_string_view();                           \
        }                                                                      \
        ec = error_res["EC"].to_string_view();                                 \
      }                                                                        \
    }                                                                          \
    if (op->status_code != -1 && op->status_code != 404) {                     \
      LOG_ERROR("operation on [`] failed! `, RequestId[`], EC[`]",             \
                ossurl.object(), op, op->resp.headers["x-oss-request-id"],     \
                ec);                                                           \
    }                                                                          \
    if (op->status_code / 100 == 5) {                                          \
      if (__retry_times-- > 0) {                                               \
        photon::thread_usleep(__retry_interval);                               \
        __retry_interval =                                                     \
            std::min(__retry_interval * 2, OSS_REQUEST_MAX_RETRY_INTERVAL_US); \
        LOG_ERROR("Retrying oss request ` `", verbstr[op->req.verb()],         \
                  op->req.target());                                           \
        goto __retry;                                                          \
      }                                                                        \
    }                                                                          \
    switch (op->status_code) {                                                 \
      case -1:                                                                 \
        LOG_ERROR("operation on [`] failed!, http connection error",           \
                  ossurl.object());                                            \
        errno = __saved_errno;                                                 \
        return ret;                                                            \
      case 400:                                                                \
      case 409:                                                                \
        op.reset(nullptr);                                                     \
        errno = ENOTSUP;                                                       \
        break;                                                                 \
      case 403:                                                                \
        op.reset(nullptr);                                                     \
        errno = EACCES;                                                        \
        break;                                                                 \
      case 404:                                                                \
        op.reset(nullptr);                                                     \
        errno = ENOENT;                                                        \
        break;                                                                 \
      case 416:                                                                \
        op.reset(nullptr);                                                     \
        errno = EINVAL;                                                        \
        break;                                                                 \
    }                                                                          \
    return ret;                                                                \
  }

#define DO_CALL(op, ret, ossurl)                                            \
  std::string *do_call_resp_code_ptr = nullptr;                             \
  DO_CALL_WITH_RESP_CODE(op, ret, ossurl, do_call_resp_code_ptr)

#define VERIFY_CRC64_IF_NEEDED(oss_url, expected_crc64)                     \
  if (expected_crc64) {                                                     \
    if (op->resp.headers.find("x-oss-hash-crc64ecma") !=                    \
        op->resp.headers.end()) {                                           \
      auto upload_crc64 = op->resp.headers["x-oss-hash-crc64ecma"];         \
      if (std::to_string(*expected_crc64) != upload_crc64) {                \
        LOG_ERROR_RETURN(                                                   \
            EIO, -1, "crc64 mismatch of object: `, expected: `, actual: `", \
            oss_url.object(), *expected_crc64, upload_crc64);               \
      }                                                                     \
    } else {                                                                \
      LOG_WARN("crc64 not found of object: `", oss_url.object());           \
    }                                                                       \
  }

// all the defined header keys should be in lower case
static const std::string OSS_HEADER_KEY_CONTENT_TYPE = "content-type";
static const std::string OSS_HEADER_KEY_CONTENT_MD5 = "content-md5";
static const std::string OSS_HEADER_KEY_X_OSS_COPY_SOURCE = "x-oss-copy-source";
static const std::string OSS_HEADER_KEY_X_OSS_COPY_SOURCE_RANGE = "x-oss-copy-source-range";
static const std::string OSS_HEADER_KEY_X_OSS_METADATA_DIRECTIVE = "x-oss-metadata-directive";
static const std::string OSS_HEADER_KEY_X_OSS_RANGE_BEHAVIOR = "x-oss-range-behavior";
static const std::string OSS_HEADER_KEY_X_OSS_FORBID_OVERWRITE = "x-oss-forbid-overwrite";

static const std::string OSS_PARAM_KEY_OBJECT_META = "objectMeta";
static const std::string OSS_PARAM_KEY_LIST_TYPE = "list-type";
static const std::string OSS_PARAM_KEY_MAX_KEYS = "max-keys";
static const std::string OSS_PARAM_KEY_DELIMITER = "delimiter";
static const std::string OSS_PARAM_KEY_PREFIX = "prefix";
static const std::string OSS_PARAM_KEY_MARKER = "marker";
static const std::string OSS_PARAM_KEY_CONTINUATION_TOKEN = "continuation-token";
static const std::string OSS_PARAM_KEY_DELETE = "delete";
static const std::string OSS_PARAM_KEY_UPLOADS = "uploads";
static const std::string OSS_PARAM_KEY_PART_NUMBER = "partNumber";
static const std::string OSS_PARAM_KEY_UPLOAD_ID = "uploadId";
static const std::string OSS_PARAM_KEY_APPEND = "append";
static const std::string OSS_PARAM_KEY_POSITION = "position";

static constexpr int GMT_DATE_LIMIT = 64;
static constexpr int GMT_UPDATE_INTERVAL = 60; // update GMT time every 60 seconds

class OssSignedUrl {
 public:
  estring m_url, m_raw_object;
  uint64_t m_url_size;
  rstring_view16 m_bucket, m_object;
  char m_gmt_date[GMT_DATE_LIMIT];
  char m_gmt_date_iso8601[GMT_DATE_LIMIT];
  time_t m_last_tim = 0;
  OssSignedUrl() {}

  OssSignedUrl(std::string_view endpoint, std::string_view bucket,
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

  void append_params(estring &s,
                     const std::map<estring_view, estring_view> &params,
                     bool need_question_mark = true) {
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

  void append_headers(estring &s,
                      const std::map<estring_view, estring_view> &header_map,
                      bool v4_signature) {
    for (auto &kv : header_map) {
      if (kv.second.empty()) continue;
      if (kv.first.substr(0, 5) != "x-oss" &&
          (!v4_signature || (kv.first != OSS_HEADER_KEY_CONTENT_MD5 &&
                             kv.first != OSS_HEADER_KEY_CONTENT_TYPE)))
        continue;
      s.appends(kv.first, ":", kv.second, "\n");
    }
  }

  std::string update_v1(std::string_view key, std::string_view key_secret,
                        std::string_view token, std::string_view method,
                        const std::map<estring_view, estring_view> &params,
                        const std::map<estring_view, estring_view> &headers) {
    m_url.resize(m_url_size);
    append_params(m_url, params);

    estring ret, signature, data2sign;
    update_gmt_date();

    estring_view content_md5, content_type;
    auto it = headers.find(OSS_HEADER_KEY_CONTENT_MD5);
    if (it != headers.end()) content_md5 = it->second;
    it = headers.find(OSS_HEADER_KEY_CONTENT_TYPE);
    if (it != headers.end()) content_type = it->second;

    // oss signature v1 with token
    data2sign.appends(method, "\n", content_md5, "\n", content_type, "\n",
                      m_gmt_date, "\n");

    append_headers(data2sign, headers, false);

    data2sign.appends(
        make_ccl(!token.empty(), "x-oss-security-token:", token, "\n"), "/",
        bucket(), "/", object());

    // complete this list if needed. currently it's for ossfs use only.
    static const std::unordered_set<estring_view> sub_resources = {
        OSS_PARAM_KEY_OBJECT_META, OSS_PARAM_KEY_CONTINUATION_TOKEN,
        OSS_PARAM_KEY_APPEND,      OSS_PARAM_KEY_POSITION,
        OSS_PARAM_KEY_UPLOADS,     OSS_PARAM_KEY_UPLOAD_ID,
        OSS_PARAM_KEY_PART_NUMBER, OSS_PARAM_KEY_DELETE};

    std::map<estring_view, estring_view> sub_params;
    for (auto &it : params) {
      if (sub_resources.count(it.first)) {
        sub_params.emplace(it.first, it.second);
      }
    }
    append_params(data2sign, sub_params);

    photon::net::Base64Encode(hmac_sha1(key_secret, data2sign), signature);
    ret.appends("OSS ", key, ":", signature);
    return ret;
  }

  std::string update_v4(std::string_view region, std::string_view key,
                        std::string_view key_secret, std::string_view token,
                        std::string_view method,
                        const std::map<estring_view, estring_view> &params,
                        const std::map<estring_view, estring_view> &headers) {
    m_url.resize(m_url_size);
    append_params(m_url, params);

    estring ret, signature, canonical_request, string2sign, signing_key;
    update_gmt_date();

    canonical_request.appends(method, "\n", "/", bucket(), "/",
                              photon::net::http::url_escape(object(), false), "\n");
    append_params(canonical_request, params, false);

    canonical_request.appends("\n");

    // headers are aloso sorted in lexicographical order
    // header1:value1\n
    // header2:value2\n
    std::map<estring_view, estring_view> header_map = headers;
    header_map["x-oss-content-sha256"] = "UNSIGNED-PAYLOAD";
    header_map["x-oss-date"] = m_gmt_date_iso8601;
    if (!token.empty()) {
      header_map["x-oss-security-token"] = token;
    }
    append_headers(canonical_request, header_map, true);

    canonical_request.appends("\n", "\n" /*no additional headers*/,
                              "UNSIGNED-PAYLOAD");

    string2sign.appends("OSS4-HMAC-SHA256\n", m_gmt_date_iso8601, "\n",
                        estring_view(m_gmt_date_iso8601, 8), "/", region,
                        "/oss/aliyun_v4_request\n",
                        sha256_hash(canonical_request));

    signing_key = hmac_sha256(
        hmac_sha256(
            hmac_sha256(hmac_sha256(estring("aliyun_v4").appends(key_secret),
                                    estring_view(m_gmt_date_iso8601, 8)),
                        region),
            "oss"),
        "aliyun_v4_request");

    signature = hex(hmac_sha256(signing_key, string2sign));

    // Authorization: "OSS4-HMAC-SHA256 Credential=" + AccessKeyId + "/" +
    // SignDate + "/" + SignRegion + "/oss/aliyun_v4_request," + [
    // "AdditionalHeaders=" + AdditionalHeadersVal + "," ] + "Signature=" +
    // SignatureVal
    ret.appends("OSS4-HMAC-SHA256 Credential=", key, "/",
                estring_view(m_gmt_date_iso8601, 8), "/", region,
                "/oss/aliyun_v4_request,", "Signature=", signature);
    return ret;
  }

  std::string hmac_sha1(std::string_view key, std::string_view data) {
    HMAC_CTX ctx;
    unsigned char output[EVP_MAX_MD_SIZE];
    auto evp_md = EVP_sha1();
    unsigned int output_length;
    HMAC_CTX_init(&ctx);
    HMAC_Init_ex(&ctx, (const unsigned char *)key.data(), key.length(), evp_md,
                 nullptr);
    HMAC_Update(&ctx, (const unsigned char *)data.data(), data.length());
    HMAC_Final(&ctx, (unsigned char *)output, &output_length);
    HMAC_CTX_cleanup(&ctx);
    return std::string((const char *)output, output_length);
  }

  std::string hex(std::string_view bytes) {
    static const std::string hex_map = "0123456789abcdef";
    std::string hex_data;
    hex_data.reserve(bytes.size() * 2);
    for (size_t i = 0; i < bytes.size(); ++i) {
      hex_data += hex_map[((unsigned char)bytes[i]) >> 4];
      hex_data += hex_map[((unsigned char)bytes[i]) & 0xf];
    }
    return hex_data;
  }

  std::string sha256_hash(std::string_view data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, data.data(), data.size());
    SHA256_Final(hash, &sha256);

    return hex(std::string_view((char *)hash, SHA256_DIGEST_LENGTH));
  }

  std::string hmac_sha256(std::string_view key, std::string_view data) {
    HMAC_CTX ctx;
    unsigned char output[EVP_MAX_MD_SIZE];
    auto evp_md = EVP_sha256();
    unsigned int output_length;
    HMAC_CTX_init(&ctx);
    HMAC_Init_ex(&ctx, (const unsigned char *)key.data(), key.length(), evp_md,
                 nullptr);
    HMAC_Update(&ctx, (const unsigned char *)data.data(), data.length());
    HMAC_Final(&ctx, (unsigned char *)output, &output_length);
    HMAC_CTX_cleanup(&ctx);
    return std::string((const char *)output, output_length);
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
};

static ssize_t body_writer_cb(void *iov_view, photon::net::http::Request *req) {
  auto view = static_cast<iovector_view *>(iov_view);
  auto ret = req->writev(view->iov, view->iovcnt);
  if (ret != static_cast<ssize_t>(view->sum())) {
    LOG_ERROR_RETURN(0, -1, "stream writev failed!", VALUE(ret), VALUE(errno));
  }
  return 0;
}

static std::string content_md5(iovector_view view) {
  std::string ret;
  char md[MD5_DIGEST_LENGTH];
  MD5_CTX ctx;
  MD5_Init(&ctx);
  for (const auto &iov : view) {
    MD5_Update(&ctx, iov.iov_base, iov.iov_len);
  }
  MD5_Final((unsigned char *)md, &ctx);
  photon::net::Base64Encode(std::string_view(md, MD5_DIGEST_LENGTH), ret);
  return ret;
}

OssClient::OssClient(const OssOptions &options,
                     CredentialsProvider *credentialsProvider)
    : m_bucket(options.bucket),
      m_oss_options(options),
      m_credentialsProvider(credentialsProvider) {
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
  m_client->set_user_agent(m_oss_options.user_agent);

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
}

HTTP_OP OssClient::make_http_operation(Verb v, OssSignedUrl &oss_url,
                                       const std::map<estring_view, estring_view> &params,
                                       const std::map <estring_view, estring_view> &headers) {
  const auto& creds = m_credentialsProvider->getCredentials();
  const auto& token = creds.m_sessionToken;
  std::string signature;
  if (m_oss_options.region.empty()) {
    signature = oss_url.update_v1(creds.m_accessKeyId, creds.m_accessKeySecret,
                                  token, photon::net::http::verbstr[v], params, headers);
  } else {
    signature = oss_url.update_v4(m_oss_options.region, creds.m_accessKeyId,
                                  creds.m_accessKeySecret, token,
                                  photon::net::http::verbstr[v], params, headers);
  }
  auto op = m_client->new_operation(v, oss_url.m_url);
  op->req.headers.insert("Date", oss_url.m_gmt_date);
  op->req.headers.insert("Authorization", signature);
  for (const auto& kv : headers) {
    if (kv.second.empty()) continue;
    op->req.headers.insert(kv.first, kv.second);
  }
  if (!m_oss_options.region.empty()) {
     op->req.headers.insert("x-oss-content-sha256", "UNSIGNED-PAYLOAD");
     op->req.headers.insert("x-oss-date", oss_url.m_gmt_date_iso8601);
  }
  if (!token.empty()) op->req.headers.insert("x-oss-security-token", token);
  return HTTP_OP(op);
}

int OssClient::do_list_objects(std::string_view bucket, std::string_view prefix,
                               ListObjectsCallback cb, bool delimiters,
                               int maxKeys, std::string *marker, std::string *resp_code) {
  if (maxKeys > 1000 || maxKeys <= 0) maxKeys = m_oss_options.max_list_ret_cnt;
  estring_view _mark;
  if (marker) _mark = *marker;

  estring escaped_prefix = photon::net::http::url_escape(prefix);
  estring escaped_delimiter = photon::net::http::url_escape("/");
  estring escaped_marker = photon::net::http::url_escape(_mark);
  estring max_key_str = std::to_string(maxKeys);
  std::map<estring_view, estring_view> params = {
      {OSS_PARAM_KEY_LIST_TYPE, "2"},
      {OSS_PARAM_KEY_PREFIX, escaped_prefix},
      {OSS_PARAM_KEY_MAX_KEYS, max_key_str}
  };
  if (delimiters) params.emplace(OSS_PARAM_KEY_DELIMITER, escaped_delimiter);
  if (!_mark.empty()) params.emplace(OSS_PARAM_KEY_CONTINUATION_TOKEN, escaped_marker);

  OssSignedUrl target(m_endpoint, bucket, "", m_is_http);
  auto op = make_http_operation(Verb::GET, target, params);
  DO_CALL_WITH_RESP_CODE(op, -1, target, resp_code)

  auto reader = get_xml_node(op);
  if (!reader) LOG_ERROR_RETURN(0, -1, "failed to parse xml resp_body");
  auto list_bucket_result = reader["ListBucketResult"];
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
  if (marker) {
    auto next_marker = list_bucket_result["NextContinuationToken"];
    *marker = next_marker ? next_marker.to_string_view() : "";
  }
  return 0;
}

int OssClient::do_copy_object(OssSignedUrl &src_oss_url,
                              OssSignedUrl &dst_oss_url, bool set_mime) {
  estring oss_header;
  estring oss_copy_source = estring("/").appends(src_oss_url.bucket(), "/",
                                                 src_oss_url.object(true));
  std::map <estring_view, estring_view> headers = {
    {OSS_HEADER_KEY_X_OSS_COPY_SOURCE, oss_copy_source}
  };

  std::string_view dst_type = ""; // use the same content type as the source
  if (set_mime) {
    auto src_type = lookup_mime_type(src_oss_url.object());
    auto new_dst_type = lookup_mime_type(dst_oss_url.object());
    if (src_type != new_dst_type) dst_type = new_dst_type;
    if (!dst_type.empty()) {
      headers.emplace(OSS_HEADER_KEY_X_OSS_METADATA_DIRECTIVE, "REPLACE");
      headers.emplace(OSS_HEADER_KEY_CONTENT_TYPE, dst_type);
    }
  }

  auto op = make_http_operation(Verb::PUT, dst_oss_url, {}, headers);
  DO_CALL(op, -1, dst_oss_url)
  return 0;
}

int OssClient::do_delete_object(OssSignedUrl &oss_url) {
  auto op = make_http_operation(Verb::DELETE, oss_url);
  DO_CALL(op, -1, oss_url)
  return 0;
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
  bool need_slash = (prefix.size() && prefix.back() != '/');
  std::string_view req_head = "<Delete><Quiet>true</Quiet>";
  std::string_view req_tail = "</Delete>";
  estring req_list;
  for (size_t i = 1; i <= objects.size(); ++i) {
    req_list.appends("<Object><Key>", xml_escape(prefix), make_ccl(need_slash, "/"),
                     xml_escape(objects[i - 1]), "</Key></Object>");

    // a single request can at most hold 1000 objects
    if (i % 1000 == 0 || i == objects.size()) {
      struct iovec iov[3] = {{(void *)req_head.data(), req_head.size()},
                             {(void *)req_list.data(), req_list.size()},
                             {(void *)req_tail.data(), req_tail.size()}};
      iovector_view body_view(iov, 3);
      OssSignedUrl m_oss_url(m_endpoint, bucket, "/", m_is_http);
      auto md5 = content_md5(body_view);

      std::map<estring_view, estring_view> params = {
          {OSS_PARAM_KEY_DELETE, ""}
      };
      std::map<estring_view, estring_view> headers = {
          {OSS_HEADER_KEY_CONTENT_MD5, md5}
      };
      auto op = make_http_operation(Verb::POST, m_oss_url, params, headers);

      op->req.headers.content_length(body_view.sum());
      op->body_writer = {&body_view, &body_writer_cb};
      DO_CALL(op, -1, m_oss_url)
      req_list.clear();
    }
  }
  return 0;
}

int OssClient::rename_object(std::string_view src_path,
                             std::string_view dst_path, bool set_mime) {
  OssSignedUrl src_oss_url(m_endpoint, m_bucket, src_path, m_is_http);
  OssSignedUrl dst_oss_url(m_endpoint, m_bucket, dst_path, m_is_http);

  if (do_copy_object(src_oss_url, dst_oss_url, set_mime) < 0) return -1;
  return do_delete_object(src_oss_url);
}

int OssClient::delete_objects(std::string_view obj_prefix,
                              const std::vector<std::string> &objects) {
  return do_delete_objects(m_bucket, obj_prefix, objects);
}

int OssClient::list_objects(std::string_view prefix, ListObjectsCallback cb,
                            bool delimiter, std::string *context, int max_keys) {
  std::string marker;
  if (context) marker = *context;
  if (max_keys == 0) max_keys = m_oss_options.max_list_ret_cnt;
  do {
    int r =
        do_list_objects(m_bucket, prefix, cb, delimiter, max_keys, &marker);
    if (r < 0) return r;

  } while (!context && !marker.empty());
  if (context) *context = marker;
  return 0;
}

int OssClient::head_object(std::string_view object, ObjectHeaderMeta &meta) {
  OssSignedUrl oss_url(m_endpoint, m_bucket, object, m_is_http);
  auto op = make_http_operation(Verb::HEAD, oss_url);
  DO_CALL(op, -1, oss_url)
  auto len = op->resp.resource_size();
  if (len == -1) LOG_ERROR_RETURN(0, -1, "Unexpected http response header");

  meta.size = static_cast<size_t>(len);
  auto mtime = op->resp.headers["Last-Modified"];
  if (!mtime.empty()) {
    meta.mtime = get_lastmodified(mtime.data());
  }

  meta.type = estring_view(op->resp.headers["x-oss-object-type"]);
  meta.storage_class = estring_view(op->resp.headers["x-oss-storage-class"]);

  auto it = op->resp.headers.find("x-oss-hash-crc64ecma");
  meta.crc64.first = it != op->resp.headers.end();
  if (meta.crc64.first)
    meta.crc64.second = estring_view(it.second()).to_uint64();
  return 0;
}

ssize_t OssClient::get_object(std::string_view obj_path, const struct iovec *iov,
                              int iovcnt, off_t offset) {
  int retry_times = OSS_REQUEST_RETRY_TIMES;
  auto retry_interval = OSS_REQUEST_RETRY_INTERVAL_US;

retry:
  iovector_view view((struct iovec *)iov, iovcnt);
  auto cnt = view.sum();
  if (cnt == 0) return 0;
  OssSignedUrl oss_url(m_endpoint, m_bucket, obj_path, m_is_http);

  std::map<estring_view, estring_view> headers = {
      {OSS_HEADER_KEY_X_OSS_RANGE_BEHAVIOR, "standard"}};
  auto op = make_http_operation(Verb::GET, oss_url, {}, headers);
  op->req.headers.range(offset, offset + cnt - 1);
  DO_CALL(op, -1, oss_url)

  uint64_t content_length = op->resp.headers.content_length();
  // TODO: meta cache is expired, perhaps we should refresh automatically
  if (content_length != cnt) {
    op.reset(nullptr);
    LOG_ERROR_RETURN(EINVAL, -1,
                     "Got unexpected content length of `, expected: `, got: `",
                     obj_path, cnt, content_length);
  }

  auto ret = op->resp.readv(iov, iovcnt);
  FAULT_INJECTION(FileSystem::FI_OssError_Read_Partial, [&]() { ret--; });

  // we have encountered partial read issue because of the socket was
  // unexpectedly closed
  if (ret != static_cast<ssize_t>(content_length)) {
    LOG_ERROR("Get object ` return partial data, offset `, expected: `, got: `",
              obj_path, offset, cnt, ret);
    if (retry_times-- > 0) {
      photon::thread_usleep(retry_interval);
      retry_interval =
          std::min(retry_interval * 2, OSS_REQUEST_MAX_RETRY_INTERVAL_US);
      LOG_ERROR("Retrying oss request ` `", verbstr[op->req.verb()],
                op->req.target());
      op.reset(nullptr);
      goto retry;
    }
    ret = -1;
    errno = EIO;
  }

  return ret;
}

ssize_t OssClient::put_object(std::string_view object, const struct iovec *iov,
                              int iovcnt, uint64_t *expected_crc64) {
  iovector_view view((struct iovec *)iov, iovcnt);
  auto cnt = view.sum();

  OssSignedUrl oss_url(m_endpoint, m_bucket, object, m_is_http);
  auto content_type = lookup_mime_type(object);

  std::map<estring_view, estring_view> headers = {
      {OSS_HEADER_KEY_CONTENT_TYPE, content_type}};
  auto op = make_http_operation(Verb::PUT, oss_url, {}, headers);
  op->req.headers.content_length(cnt);
  op->body_writer = {&view, &body_writer_cb};
  DO_CALL(op, -1, oss_url)
  VERIFY_CRC64_IF_NEEDED(oss_url, expected_crc64)
  return cnt;
}

ssize_t OssClient::append_object(std::string_view object,
                                 const struct iovec *iov, int iovcnt,
                                 off_t position, uint64_t *expected_crc64) {
  iovector_view view((struct iovec *)iov, iovcnt);
  auto cnt = view.sum();

  OssSignedUrl oss_url(m_endpoint, m_bucket, object, m_is_http);

  estring position_str = std::to_string(position);
  std::map<estring_view, estring_view> params = {
      {OSS_PARAM_KEY_APPEND, ""}, {OSS_PARAM_KEY_POSITION, position_str}};

  std::map<estring_view, estring_view> headers;
  auto content_type = lookup_mime_type(object);
  if (!content_type.empty()) {
    headers.emplace(OSS_HEADER_KEY_CONTENT_TYPE, content_type);
  }

  auto op = make_http_operation(Verb::POST, oss_url, params, headers);
  op->req.headers.content_length(cnt);
  op->body_writer = {&view, &body_writer_cb};
  DO_CALL(op, -1, oss_url)
  VERIFY_CRC64_IF_NEEDED(oss_url, expected_crc64)
  return cnt;
}

int OssClient::copy_object(std::string_view src_object,
                           std::string_view dst_object, bool overwrite,
                           bool set_mime) {
  OssSignedUrl src_oss_url(m_endpoint, m_bucket, src_object, m_is_http);
  OssSignedUrl dst_oss_url(m_endpoint, m_bucket, dst_object, m_is_http);

  estring oss_copy_source =
      estring("/").appends(src_oss_url.bucket(), "/", src_oss_url.object(true));
  std::map<estring_view, estring_view> headers = {
    {OSS_HEADER_KEY_X_OSS_COPY_SOURCE, oss_copy_source}
  };
  if (!overwrite) {
    headers.emplace(OSS_HEADER_KEY_X_OSS_FORBID_OVERWRITE, "true");
  }

  std::string_view dst_type = ""; // use the same content type as the source
  if (set_mime) {
    auto src_type = lookup_mime_type(src_oss_url.object());
    auto new_dst_type = lookup_mime_type(dst_oss_url.object());
    if (src_type != new_dst_type) dst_type = new_dst_type;
    if (!dst_type.empty()) {
      headers.emplace(OSS_HEADER_KEY_X_OSS_METADATA_DIRECTIVE, "REPLACE");
      headers.emplace(OSS_HEADER_KEY_CONTENT_TYPE, dst_type);
    }
  }

  auto op = make_http_operation(Verb::PUT, dst_oss_url, {}, headers);
  DO_CALL(op, -1, dst_oss_url)
  return 0;
}

int OssClient::check_prefix(std::string_view prefix) {
  std::string res_code;

  auto noop = [](const ListObjectsCBParams &) { return 0; };

  int r =
      do_list_objects(m_bucket, prefix, noop, false, 1, nullptr, &res_code);
  if (r != 0) LOG_ERROR("Check bucket prefix failed with err: `", res_code);
  return r;
}

struct oss_multipart_context {
  estring obj_path;
  std::string upload_id;

  photon::spinlock lock;
  std::map<int, std::string> part_list;
};

int OssClient::init_multipart_upload(std::string_view object,
                                     void **context) {
  OssSignedUrl oss_url(m_endpoint, m_bucket, object, m_is_http);

  std::map<estring_view, estring_view> params = {{OSS_PARAM_KEY_UPLOADS, ""}};
  auto content_type = lookup_mime_type(object);
  std::map<estring_view, estring_view> headers;
  if (!content_type.empty())
    headers.emplace(OSS_HEADER_KEY_CONTENT_TYPE, content_type);

  auto op = make_http_operation(Verb::POST, oss_url, params, headers);
  DO_CALL(op, -1, oss_url)

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
  std::map<estring_view, estring_view> params = {
    {OSS_PARAM_KEY_PART_NUMBER, part_nums_str},
    {OSS_PARAM_KEY_UPLOAD_ID, ctx->upload_id}
  };

  OssSignedUrl oss_url(m_endpoint, m_bucket, ctx->obj_path.c_str(),
                         m_is_http);

  auto op = make_http_operation(Verb::PUT, oss_url, params);

  op->req.headers.content_length(cnt);
  op->body_writer = {&view, &body_writer_cb};
  DO_CALL(op, -1, oss_url);

  auto etag = op->resp.headers["ETag"];
  if (etag.empty()) LOG_ERROR_RETURN(EINVAL, -1, "unexpected response", op);

  VERIFY_CRC64_IF_NEEDED(oss_url, expected_crc64)

  ctx->lock.lock();
  ctx->part_list.emplace(part_number, std::string(etag));
  ctx->lock.unlock();

  return cnt;
}

int OssClient::upload_part_copy(void *context, off_t offset, size_t count,
                                int part_number) {
  assert(context);
  oss_multipart_context *ctx = (oss_multipart_context *)context;
  assert(!ctx->upload_id.empty());

  OssSignedUrl oss_url(m_endpoint, m_bucket, ctx->obj_path.c_str(),
                         m_is_http);

  estring part_num_str = std::to_string(part_number);
  std::map<estring_view, estring_view> params = {
      {OSS_PARAM_KEY_PART_NUMBER, part_num_str},
      {OSS_PARAM_KEY_UPLOAD_ID, ctx->upload_id}};

  estring oss_copy_source =
      estring("/").appends(oss_url.bucket(), "/", oss_url.object(true));
  std::string range = "bytes=" + std::to_string(offset) + "-" +
                      std::to_string(offset + count - 1);

  std::map<estring_view, estring_view> headers = {
      {OSS_HEADER_KEY_X_OSS_COPY_SOURCE, oss_copy_source},
      {OSS_HEADER_KEY_X_OSS_COPY_SOURCE_RANGE, range}};

  auto op = make_http_operation(Verb::PUT, oss_url, params, headers);
  DO_CALL(op, -1, oss_url)

  auto reader = get_xml_node(op);
  if (!reader) LOG_ERROR_RETURN(EINVAL, -1, "failed to parse xml resp_body");
  auto child = reader["CopyPartResult"]["ETag"];
  if (!child) LOG_ERROR_RETURN(EINVAL, -1, "invalid response with no etag provided");

  auto etag = child.to_string_view();
  if (etag.empty()) LOG_ERROR_RETURN(EINVAL, -1, "unexpected response with empty etag", op);

  ctx->lock.lock();
  ctx->part_list.emplace(part_number, std::string(etag));
  ctx->lock.unlock();

  return 0;
}

int OssClient::complete_multipart_upload(void *context,
                                         uint64_t *expected_crc64) {
  assert(context);

  oss_multipart_context *ctx = (oss_multipart_context *)context;
  assert(!ctx->upload_id.empty());

  estring req_body;
  req_body.appends("<CompleteMultipartUpload>");

  for (auto &part : ctx->part_list) {
    req_body.appends(
        "<Part>"
        "<PartNumber>",
        part.first,
        "</PartNumber>"
        "<ETag>",
        part.second,
        "</ETag>"
        "</Part>");
  }
  req_body.appends("</CompleteMultipartUpload>");

  struct iovec iov {
    (void *)(req_body.data()), req_body.size()
  };

  iovector_view view(&iov, 1);
  OssSignedUrl oss_url(m_endpoint, m_bucket, ctx->obj_path.c_str(),
                         m_is_http);


  DEFER(delete ctx);
  std::map<estring_view, estring_view> params = {
    {OSS_PARAM_KEY_UPLOAD_ID, ctx->upload_id}
  };

  auto op = make_http_operation(Verb::POST, oss_url, params);
  op->req.headers.content_length(req_body.size());
  op->body_writer = {&view, &body_writer_cb};
  DO_CALL(op, -1, oss_url)
  VERIFY_CRC64_IF_NEEDED(oss_url, expected_crc64)
  return 0;
}

int OssClient::abort_multipart_upload(void *context) {
  assert(context);

  oss_multipart_context *ctx = (oss_multipart_context *)context;
  assert(!ctx->upload_id.empty());

  OssSignedUrl oss_url(m_endpoint, m_bucket, ctx->obj_path.c_str(),
                         m_is_http);

  DEFER(delete ctx);
  std::map<estring_view, estring_view> params = {
    {OSS_PARAM_KEY_UPLOAD_ID, ctx->upload_id}
  };

  auto op = make_http_operation(Verb::DELETE, oss_url, params);
  DO_CALL(op, -1, oss_url)
  return 0;
}

int OssClient::get_object_meta(std::string_view object, ObjectMeta &meta) {
  OssSignedUrl oss_url(m_endpoint, m_bucket, object, m_is_http);
  std::map<estring_view, estring_view> params = {
      {OSS_PARAM_KEY_OBJECT_META, ""}};
  auto op = make_http_operation(Verb::HEAD, oss_url, params);
  DO_CALL(op, -1, oss_url)
  auto len = op->resp.resource_size();
  if (len == -1) LOG_ERROR_RETURN(0, -1, "Unexpected http response header");
  meta.size = static_cast<size_t>(len);
  auto mtime = op->resp.headers["Last-Modified"];
  if (!mtime.empty()) {
    meta.mtime = get_lastmodified(mtime.data());
  }

  auto obj_etag = op->resp.headers["ETag"];
  if (!obj_etag.empty()) {
    meta.etag.assign(obj_etag.data(), obj_etag.size());
  }
  return 0;
}

int OssClient::delete_object(std::string_view obj_path) {
  OssSignedUrl oss_url(m_endpoint, m_bucket, obj_path, m_is_http);
  return do_delete_object(oss_url);
}
}
}  // namespace FileSystemExt

// #pragma GCC diagnostic pop