find_path(FSTACK_INCLUDE_DIRS NAMES ff_api.h PATHS /usr/local/include NO_DEFAULT_PATH)

find_library(FSTACK_LIBRARIES NAMES fstack PATHS /usr/local/lib NO_DEFAULT_PATH)

find_package_handle_standard_args(fstack DEFAULT_MSG FSTACK_LIBRARIES FSTACK_INCLUDE_DIRS)

mark_as_advanced(FSTACK_INCLUDE_DIRS FSTACK_LIBRARIES)