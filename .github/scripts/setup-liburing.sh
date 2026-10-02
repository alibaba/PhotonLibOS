#!/usr/bin/env bash
set -euo pipefail

# Shared runner images can carry a different liburing release. Use the version
# required by this checkout in a job-local prefix for configure and runtime.
source_dir="${GITHUB_WORKSPACE:?}"
build_dir="${RUNNER_TEMP:?}/photon-liburing"
version=$(sed -n 's/^set(URING_VERSION \([0-9.]*\)).*/\1/p' "$source_dir/CMake/Finduring.cmake")
if [[ -z "$version" ]]; then
    echo "Cannot determine the project's liburing version" >&2
    exit 1
fi

prefix="$build_dir/install"
mkdir -p "$build_dir"
curl --fail --location --retry 3 \
    "https://github.com/axboe/liburing/archive/refs/tags/liburing-$version.tar.gz" \
    --output "$build_dir/source.tar.gz"
tar -xzf "$build_dir/source.tar.gz" -C "$build_dir"
cd "$build_dir/liburing-liburing-$version"
./configure --prefix="$prefix" --libdir="$prefix/lib"
make -C src -j2
make install

# CMake environment path lists use ':' on the Linux runners.
{
    echo "CMAKE_PREFIX_PATH=$prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
    echo "LD_LIBRARY_PATH=$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
} >> "${GITHUB_ENV:?}"
