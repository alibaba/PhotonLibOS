photon_add_test(test-fs test.cpp)
photon_add_test(test-exportfs test_exportfs.cpp)
photon_add_test(test-filecopy test_filecopy.cpp)
photon_add_test(test-throttle-file test_throttledfile.cpp)

if (PHOTON_ENABLE_FUSE STREQUAL "ON" OR PHOTON_ENABLE_FUSE STREQUAL "2" OR PHOTON_ENABLE_FUSE STREQUAL "3")
    if (PHOTON_ENABLE_FUSE STREQUAL "3")
        photon_add_test(simplefuse simplefuse.cpp NO_REGISTER
            LIBS ${FUSE3_LIBRARIES} DEFS _FILE_OFFSET_BITS=64 FUSE_USE_VERSION=317)
    else ()
        photon_add_test(simplefuse simplefuse.cpp NO_REGISTER
            LIBS ${FUSE_LIBRARIES} DEFS _FILE_OFFSET_BITS=64 FUSE_USE_VERSION=29)
    endif ()
endif ()
