# Cross-compilation toolchain for Windows x64 using MinGW-w64
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# MinGW-w64 cross compilers (install the mingw-w64 package for your platform)
set(CMAKE_C_COMPILER    x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER  x86_64-w64-mingw32-g++)
set(CMAKE_ASM_COMPILER  x86_64-w64-mingw32-gcc)
set(CMAKE_RC_COMPILER   x86_64-w64-mingw32-windres)
set(CMAKE_AR            x86_64-w64-mingw32-ar)
set(CMAKE_RANLIB        x86_64-w64-mingw32-ranlib)

# Search for programs only in the build host directories.
# Search for libs/headers/packages only in the cross-compilation sysroot.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Disable compiler checks that require linking (cross-compiling can't run binaries)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
