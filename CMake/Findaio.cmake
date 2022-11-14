find_path(AIO_INCLUDE_DIRS libaio.h)

find_library(AIO_LIBRARIES aio)

find_package_handle_standard_args(aio DEFAULT_MSG AIO_LIBRARIES AIO_INCLUDE_DIRS)

mark_as_advanced(AIO_INCLUDE_DIRS AIO_LIBRARIES)