FROM almalinux:8

RUN dnf install -y git cmake gcc-c++ openssl-devel libcurl-devel libaio-devel sudo zstd wget && \
    dnf install -y epel-release 'dnf-command(config-manager)' && \
    dnf config-manager --set-enabled powertools && \
    dnf install -y gtest-devel gmock-devel fuse-devel fuse3-devel libgsasl-devel e2fsprogs-devel && \
    dnf install -y gcc-toolset-9-gcc-c++ gcc-toolset-10-gcc-c++ gcc-toolset-11-gcc-c++ gcc-toolset-12-gcc-c++ && \
    dnf install -y redis nasm && \
    dnf clean all && rm -rf /var/cache/yum && \
    wget https://github.com/gflags/gflags/archive/refs/tags/v2.1.2.tar.gz -O - | tar -xvz && \
    cd gflags-2.1.2 && export CXXFLAGS="-fPIC" && cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_SHARED_LIBS=1 -DBUILD_STATIC_LIBS=1 . && make -j 8 && make install && \
    cd .. && rm -rf gflags-2.1.2 && ln -s libgflags.so.2.1.2 /usr/local/lib/libgflags.so.2.1 && \
    wget https://github.com/axboe/liburing/archive/refs/tags/liburing-2.3.tar.gz -O - | tar -xvz && \
    cd liburing-liburing-2.3 && export CFLAGS="-fPIC -O3 -Wall -Wextra -fno-stack-protector" && ./configure && make -C src -j $(nprocs) && make install && cd .. && rm -rf liburing-liburing-2.3 && \
    wget https://github.com/cpp-redis/cpp_redis/archive/refs/tags/4.3.1.tar.gz -O - | tar -xvz && \
    cd cpp_redis-4.3.1 && cmake -B build -DCMAKE_BUILD_TYPE=Release -DUSE_CUSTOM_TCP_CLIENT=on; cmake --build build --config Release -j -t install && cd .. && rm -rf cpp_redis-4.3.1

ENV LD_LIBRARY_PATH=/usr/local/lib:/usr/local/lib64:/usr/lib:/usr/lib64:/lib:/lib64
