find_path(E2FS_INCLUDE_DIRS ext2fs/ext2fs.h)

find_library(E2FS_LIBRARIES ext2fs)

find_package_handle_standard_args(e2fs DEFAULT_MSG E2FS_LIBRARIES E2FS_INCLUDE_DIRS)

mark_as_advanced(E2FS_INCLUDE_DIRS E2FS_LIBRARIES)
