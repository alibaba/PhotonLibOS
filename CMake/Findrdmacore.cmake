if (NOT APPLE)
    find_path(RDMACORE_INCLUDE_DIRS rdma/rsocket.h)
    find_library(RDMACORE_LIBRARIES rdmacm)
endif ()

find_package_handle_standard_args(rdmacore DEFAULT_MSG RDMACORE_LIBRARIES RDMACORE_INCLUDE_DIRS)

mark_as_advanced(RDMACORE_INCLUDE_DIRS RDMACORE_LIBRARIES)