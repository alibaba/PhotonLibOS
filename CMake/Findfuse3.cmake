find_path(FUSE3_INCLUDE_DIRS fuse3/fuse.h)
 
find_library(FUSE3_LIBRARIES fuse3)
 
find_package_handle_standard_args(fuse3 DEFAULT_MSG FUSE3_LIBRARIES FUSE3_INCLUDE_DIRS)
 
mark_as_advanced(FUSE3_INCLUDE_DIRS FUSE3_LIBRARIES)