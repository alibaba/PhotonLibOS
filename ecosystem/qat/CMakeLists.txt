# QAT LZ4 compression support for PhotonLibOS
# QAT pkg_check_modules is done in the root CMakeLists.txt

# Add qat-lz4 source to photon_obj
target_sources(photon_obj PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/lz4.cpp
)
target_include_directories(photon_obj PRIVATE
    ${QAT_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_directories(photon_obj PRIVATE ${QAT_LIBRARY_DIRS})
target_link_libraries(photon_obj PRIVATE ${QAT_LIBRARIES})
target_include_directories(photon_shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${QAT_INCLUDE_DIRS}
)
target_link_directories(photon_shared PUBLIC ${QAT_LIBRARY_DIRS})
target_link_libraries(photon_shared PUBLIC ${QAT_LIBRARIES})
