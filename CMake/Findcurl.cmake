find_path(CURL_INCLUDE_DIRS curl/curl.h)

find_library(CURL_LIBRARIES curl)

find_package_handle_standard_args(curl DEFAULT_MSG CURL_LIBRARIES CURL_INCLUDE_DIRS)

mark_as_advanced(CURL_INCLUDE_DIRS CURL_LIBRARIES)