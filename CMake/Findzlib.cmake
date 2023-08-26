find_path(ZLIB_INCLUDE_DIRS zlib.h)

find_library(ZLIB_LIBRARIES z)

find_package_handle_standard_args(zlib DEFAULT_MSG ZLIB_LIBRARIES ZLIB_INCLUDE_DIRS)

mark_as_advanced(ZLIB_INCLUDE_DIRS ZLIB_LIBRARIES)