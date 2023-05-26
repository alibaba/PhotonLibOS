if (LIBE2FS_PATH)
    find_path(LIBE2FS_INCLUDE_DIRS ext2fs/ext2fs.h HINTS ${LIBE2FS_PATH}/include)
    find_library(LIBE2FS_LIBRARIES ext2fs HINTS ${LIBE2FS_PATH}/lib)
else()
    find_path(LIBE2FS_INCLUDE_DIRS ext2fs/ext2fs.h)
    find_library(LIBE2FS_LIBRARIES ext2fs)
endif()

find_package_handle_standard_args(e2fs DEFAULT_MSG LIBE2FS_LIBRARIES LIBE2FS_INCLUDE_DIRS)

mark_as_advanced(LIBE2FS_INCLUDE_DIRS LIBE2FS_LIBRARIES)
