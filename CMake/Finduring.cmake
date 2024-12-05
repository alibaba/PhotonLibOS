find_path(URING_INCLUDE_DIRS liburing.h)

find_library(URING_LIBRARIES uring)

find_package_handle_standard_args(uring DEFAULT_MSG URING_LIBRARIES URING_INCLUDE_DIRS)

mark_as_advanced(URING_INCLUDE_DIRS URING_LIBRARIES)