include(FetchContent)
set(FETCHCONTENT_QUIET false)

FetchContent_Declare(
  ocf_lib
  GIT_REPOSITORY https://github.com/Open-CAS/ocf.git
  GIT_TAG v21.6.4
)

FetchContent_GetProperties(ocf_lib)
if (NOT ocf_lib_POPULATED)
  FetchContent_Populate(ocf_lib)
endif()

FetchContent_MakeAvailable(ocf_lib)
