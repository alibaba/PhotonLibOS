FROM debian:11

RUN apt update -y && \
    apt install -y build-essential git curl cmake libssl-dev libcurl4-openssl-dev libaio-dev zlib1g-dev libgtest-dev libgmock-dev libgflags-dev libfuse-dev libgsasl7-dev && \
    apt install -y python3-pip python3-pyelftools libnuma-dev nasm && \
    pip3 install ninja meson && \
    cd /root && \
    curl -LO https://github.com/F-Stack/f-stack/archive/refs/tags/v1.22.tar.gz && \
    tar xf v1.22.tar.gz && \
    cd /root/f-stack-1.22/dpdk/ && \
    CONFIG_RTE_LIBRTE_MLX5_PMD=y meson -Denable_kmods=false -Dtests=false build && \
    cd build && \
    ninja && \
    ninja install && \
    export FF_PATH=/root/f-stack-1.22 && \
    cd /root/f-stack-1.22/lib && \
    make -j && \
    make install