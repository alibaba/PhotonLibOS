#!/bin/bash

rm -rf build

cmake -B build -DPHOTON_BUILD_TESTING=ON -DPHOTON_BUILD_DEPENDENCIES=ON -DPHOTON_ENABLE_SPDK=ON -DPHOTON_ENABLE_FSTACK_DPDK=ON
# cmake -B build -DPHOTON_BUILD_TESTING=ON -DPHOTON_BUILD_DEPENDENCIES=ON -DPHOTON_ENABLE_FSTACK_DPDK=ON
# cmake -B build -DPHOTON_BUILD_TESTING=ON -DPHOTON_BUILD_DEPENDENCIES=ON

cp ~/gflags/v2.2.2.tar.gz ./build/gflags-prefix/src/
cp ~/googletest/release-1.12.1.tar.gz ./build/googletest-prefix/src/
cp ~/libaio-0.3.113.tar.gz ./build/aio-prefix/src/
cp ~/zlib-1.2.13.tar.gz ./build/zlib-prefix/src/
cp ~/curl-7.88.1.tar.gz ./build/curl-prefix/src/
cp ~/openssl-1.1.1w.tar.gz ./build/openssl-prefix/src/
cp ~/dpdk-20.11.6.tar.gz ./build/dpdk-prefix/src/v20.11.6.tar.gz
cp ~/f-stack-1.22.tar.gz ./build/fstack-prefix/src/v1.22.tar.gz
cp ~/isa-l-2df39cf5f1b9ccaa2973f6ef273857e4dc46f0cf.zip ./build/isa-l-prefix/src/2df39cf5f1b9ccaa2973f6ef273857e4dc46f0cf.zip
cp ~/spdk-21.04.tar.gz ./build/spdk-prefix/src/

make -C build -j 16