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

// all the defined header keys should be in lower case
static const char OSS_HEADER_KEY_CONTENT_TYPE[]   = "content-type";
static const char OSS_HEADER_KEY_CONTENT_MD5[]    = "content-md5";
static const char OSS_HEADER_KEY_X_OSS_COPY_SOURCE[]        = "x-oss-copy-source";
static const char OSS_HEADER_KEY_X_OSS_COPY_SOURCE_RANGE[]  = "x-oss-copy-source-range";
static const char OSS_HEADER_KEY_X_OSS_METADATA_DIRECTIVE[] = "x-oss-metadata-directive";
static const char OSS_HEADER_KEY_X_OSS_RANGE_BEHAVIOR[]     = "x-oss-range-behavior";
static const char OSS_HEADER_KEY_X_OSS_FORBID_OVERWRITE[]   = "x-oss-forbid-overwrite";
static const char OSS_HEADER_KEY_X_OSS_SYMLINK_TARGET[]   = "x-oss-symlink-target";

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
static const char OSS_PARAM_KEY_SYMLINK[]         = "symlink";
static const char OSS_PARAM_KEY_BATCH_GET[]       = "x-oss-batchGet";

static constexpr int GMT_DATE_LIMIT = 64;
static constexpr int GMT_UPDATE_INTERVAL = 60; // update GMT time every 60 seconds

static constexpr int XML_LIMIT = 16 * 1024 * 1024;

DEFINE_CONST_STATIC_ORDERED_STRING_KV ( MIME_TYPE_MAP, {
  // must appear in dictionary order!
  {"3gp", "video/3gpp"},
  {"3gpp", "video/3gpp"},
  {"7z", "application/x-7z-compressed"},
  {"ai", "application/postscript"},
  {"apk", "application/vnd.android.package-archive"},
  {"asf", "video/x-ms-asf"},
  {"asx", "video/x-ms-asf"},
  {"atom", "application/atom+xml"},
  {"avi", "video/x-msvideo"},
  {"bin", "application/octet-stream"},
  {"bmp", "image/x-ms-bmp"},
  {"cco", "application/x-cocoa"},
  {"crt", "application/x-x509-ca-cert"},
  {"css", "text/css"},
  {"deb", "application/octet-stream"},
  {"der", "application/x-x509-ca-cert"},
  {"dll", "application/octet-stream"},
  {"dmg", "application/octet-stream"},
  {"doc ", "application/msword"},
  {"ear", "application/java-archive"},
  {"eot", "application/octet-stream"},
  {"eps", "application/postscript"},
  {"exe", "application/octet-stream"},
  {"flv", "video/x-flv"},
  {"gif", "image/gif"},
  {"hqx", "application/mac-binhex40"},
  {"htc", "text/x-component"},
  {"htm", "text/html"},
  {"html", "text/html"},
  {"ico", "image/x-icon"},
  {"img", "application/octet-stream"},
  {"iso", "application/octet-stream"},
  {"jad", "text/vnd.sun.j2me.app-descriptor"},
  {"jar", "application/java-archive"},
  {"jardiff", "application/x-java-archive-diff"},
  {"jng", "image/x-jng"},
  {"jnlp", "application/x-java-jnlp-file"},
  {"jpeg", "image/jpeg"},
  {"jpg", "image/jpeg"},
  {"js", "application/x-javascript"},
  {"kar", "audio/midi"},
  {"kml", "application/vnd.google-earth.kml+xml"},
  {"kmz", "application/vnd.google-earth.kmz"},
  {"m3u8", "application/x-mpegURL"},
  {"m4a", "audio/x-m4a"},
  {"m4v", "video/x-m4v"},
  {"mid", "audio/midi"},
  {"midi", "audio/midi"},
  {"mml", "text/mathml"},
  {"mng", "video/x-mng"},
  {"mov", "video/quicktime"},
  {"mp3", "audio/mpeg"},
  {"mp4", "video/mp4"},
  {"mpeg", "video/mpeg"},
  {"mpg", "video/mpeg"},
  {"msi", "application/octet-stream"},
  {"msm", "application/octet-stream"},
  {"msp", "application/octet-stream"},
  {"ogg", "audio/ogg"},
  {"pdb", "application/x-pilot"},
  {"pdf", "application/pdf"},
  {"pem", "application/x-x509-ca-cert"},
  {"pl", "application/x-perl"},
  {"pm", "application/x-perl"},
  {"png", "image/png"},
  {"ppt", "application/vnd.ms-powerpoint"},
  {"prc", "application/x-pilot"},
  {"ps", "application/postscript"},
  {"ra", "audio/x-realaudio"},
  {"rar", "application/x-rar-compressed"},
  {"rpm", "application/x-redhat-package-manager"},
  {"rss", "application/rss+xml"},
  {"rtf", "application/rtf"},
  {"run", "application/x-makeself"},
  {"sea", "application/x-sea"},
  {"shtml", "text/html"},
  {"sit", "application/x-stuffit"},
  {"svg", "image/svg+xml"},
  {"svgz", "image/svg+xml"},
  {"swf", "application/x-shockwave-flash"},
  {"tcl", "application/x-tcl"},
  {"tif", "image/tiff"},
  {"tiff", "image/tiff"},
  {"tk", "application/x-tcl"},
  {"ts", "video/MP2T"},
  {"txt", "text/plain"},
  {"war", "application/java-archive"},
  {"wbmp", "image/vnd.wap.wbmp"},
  {"webm", "video/webm"},
  {"webp", "image/webp"},
  {"wgz", "application/x-nokia-widget"},
  {"wml", "text/vnd.wap.wml"},
  {"wmlc", "application/vnd.wap.wmlc"},
  {"wmv", "video/x-ms-wmv"},
  {"xhtml", "application/xhtml+xml"},
  {"xls", "application/vnd.ms-excel"},
  {"xml", "text/xml"},
  {"xpi", "application/x-xpinstall"},
  {"zip", "application/zip"},
});

