find_path(AIO_INCLUDE_DIR libaio.h)

find_library(AIO_LIBRARIES aio)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(aio DEFAULT_MSG AIO_LIBRARIES AIO_INCLUDE_DIR)

mark_as_advanced(AIO_INCLUDE_DIR AIO_LIBRARIES)