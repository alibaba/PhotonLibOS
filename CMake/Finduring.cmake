include(photon-find-helpers)

# Locate the installed headers and library without pinning a shared-library
# filename. Compatible newer releases and static installs have different names.
photon_find_package(uring HEADERS liburing.h LIBRARIES uring)
