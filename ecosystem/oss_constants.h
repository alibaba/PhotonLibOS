// all the defined header keys should be in lower case
static const char OSS_HEADER_KEY_CONTENT_TYPE[]   = "content-type";
static const char OSS_HEADER_KEY_CONTENT_MD5[]    = "content-md5";
static const char OSS_HEADER_KEY_X_OSS_COPY_SOURCE[]        = "x-oss-copy-source";
static const char OSS_HEADER_KEY_X_OSS_COPY_SOURCE_RANGE[]  = "x-oss-copy-source-range";
static const char OSS_HEADER_KEY_X_OSS_METADATA_DIRECTIVE[] = "x-oss-metadata-directive";
static const char OSS_HEADER_KEY_X_OSS_RANGE_BEHAVIOR[]     = "x-oss-range-behavior";
static const char OSS_HEADER_KEY_X_OSS_FORBID_OVERWRITE[]   = "x-oss-forbid-overwrite";

static const char OSS_PARAM_KEY_CONTINUATION_TOKEN[]        = "continuation-token";
static const char OSS_PARAM_KEY_OBJECT_META[]     = "objectMeta";
static const char OSS_PARAM_KEY_LIST_TYPE[]       = "list-type";
static const char OSS_PARAM_KEY_MAX_KEYS[]        = "max-keys";
static const char OSS_PARAM_KEY_DELIMITER[]       = "delimiter";
static const char OSS_PARAM_KEY_PREFIX[]          = "prefix";
static const char OSS_PARAM_KEY_MARKER[]          = "marker";
static const char OSS_PARAM_KEY_DELETE[]          = "delete";
static const char OSS_PARAM_KEY_UPLOADS[]         = "uploads";
static const char OSS_PARAM_KEY_PART_NUMBER[]     = "partNumber";
static const char OSS_PARAM_KEY_UPLOAD_ID[]       = "uploadId";
static const char OSS_PARAM_KEY_APPEND[]          = "append";
static const char OSS_PARAM_KEY_POSITION[]        = "position";

static constexpr int GMT_DATE_LIMIT = 64;
static constexpr int GMT_UPDATE_INTERVAL = 60; // update GMT time every 60 seconds

static constexpr int OSS_REQUEST_RETRY_TIMES = 2;
static constexpr int OSS_MAX_PATH_LEN = 1023;
static constexpr uint64_t OSS_REQUEST_RETRY_INTERVAL_US = 20000ULL;
static constexpr uint64_t OSS_REQUEST_MAX_RETRY_INTERVAL_US = 1000000ULL;
static constexpr int XML_LIMIT = 16 * 1024 * 1024;


// using StringKV = unordered_map_string_kv;
// replace this later when photon is updated and has
// unordered_map_string_key_case_insensitive.
const static StringKV MIME_TYPE_MAP = {
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

