find_path(FUSE_INCLUDE_DIRS fuse.h)

find_library(FUSE_LIBRARIES fuse)

find_package_handle_standard_args(fuse DEFAULT_MSG FUSE_LIBRARIES FUSE_INCLUDE_DIRS)

mark_as_advanced(FUSE_INCLUDE_DIRS FUSE_LIBRARIES)