# How to run Photon with F-Stack and DPDK ?

&emsp;&emsp;Since version 0.6, Photon can run on an userspace TCP/IP stack if enabled the `INIT_IO_FSTACK_DPDK` io engine. 

&emsp;&emsp;[F-Stack](https://www.f-stack.org/) is an open-source project that has ported the entire **FreeBSD** 
network stack on top of **DPDK**, and provided userspace sockets and events API. 
We have integrated Photon's coroutine scheduler with F-Stack, and made a busy-polling program more friendly to DPDK 
developers than ever before. In terms of performance, the network app has seen the improvement of 20% ~ 40%, compared with
the Linux kernel based on interrupt.

&emsp;&emsp;This article will introduce how to configure SR-IOV on a Mellanox NIC, how to set up F-Stack
and DPDK environment, how to enable the [Flow Bifurcation](https://doc.dpdk.org/guides/howto/flow_bifurcation.html) 
to filter the specific TCP/IP flow that you only concern, and finally how to run Photon on top of them, in order 
to build a high performance net server.

### Configure SR-IOV on Mellanox ConnectX-4

#### 1. Enable IOMMU

```shell
# Edit /etc/default/grub, expand GRUB_CMDLINE_LINUX with 'intel_iommu=on iommu=pt pci=realloc'
grub2-mkconfig -o /boot/grub2/grub.cfg
reboot
```

Note the `pci=realloc` is a work-around solution for CentOS and RHEL.
Without this, kernel would report 'not enough MMIO resources for SR-IOV',
see this [issue](https://access.redhat.com/solutions/37376).

#### 2. Set VF number

```shell
echo 4 > /sys/class/net/eth0/device/sriov_numvfs
```

&emsp;&emsp;If you are having an Intel NIC, this step is likely to succeed. However, for the Mellanox one, 
it might fail because of the lack of proper mlx driver in your kernel.
Then you would need to download the official driver from NVidia, and make a full install.

&emsp;&emsp;There are many available releases in https://network.nvidia.com/products/infiniband-drivers/linux/mlnx_ofed/,
you should choose one that matches to your kernel version and OS version the best.
An improper version might lead to compiling error when building kernel modules later. 
My test machine is CentOS 7 with kernel 5.x, so I downloaded MLNX_OFED_LINUX-5.4-3.6.8.1-rhel7.2-x86_64.tgz.

#### 3. Install mlnx_ofed driver

&emsp;&emsp;First you need to check your gcc version. It has to be the same one that built your kernel.
Otherwise you will need to upgrade your gcc.

```shell
gcc --version
cat /proc/version
```

&emsp;&emsp;Note that the NVidia official doc said we should install 'createrepo', but in CentOS 7, 
there are some tiny bugs of its Python scripts. The 'createrepo_c' package will solve this.

```shell
yum install python-devel tcl tk elfutils-libelf-devel createrepo_c
```

&emsp;&emsp;Because the mlnx_ofed driver has already included rdma packages, to avoid collision,
I decided to remove all rdma-related rpms previously installed in my test machine.

```shell
rpm -qa | grep rdma
rpm -e ...
```

&emsp;&emsp;Build and install the driver and the additional packages.

```shell
cd MLNX_OFED_LINUX-5.4-3.6.8.1-rhel7.2-x86_64/
./mlnxofedinstall --skip-distro-check --add-kernel-support --without-mlnx-nvme --dpdk

rpm -qa | grep rdma
# There will be rdma-core, rdma-core-devel, librdmacm and librdmacm-utils.
```

&emsp;&emsp;Now we need to restart the server. Be careful, there is a possibility that the interface name
of your NIC might change, for example, from `eth0` to something like `enp3s0f0`. It will incur connection failure
of your server and unable to log in. You'd better have a backup management network, or BMC console, 
and modify the NIC configs in `/etc/sysconfig/network-scripts/` once you re-logged in.

&emsp;&emsp;Please note that this part of procedure is still under evaluation. If you have better ways to
persist network configurations after installing the driver, please give us feed back. :-)

```shell
# Update initramfs before next boot
dracut -f
reboot 
```

&emsp;&emsp;After reboot:

```shell
# Start Mellanox Software Tools Service
mst start

# Show device name and port mapping
mst status
ibdev2netdev

# Check firmware capabilities
mlxconfig -d /dev/mst/mt4117_pciconf0 query | grep NUM_OF_VFS

# Set VF number. Should succeed now
echo 4 > /sys/class/net/enp3s0f0/device/sriov_numvfs
lspci -nn | grep 'Ethernet controller'
```

### Install DPDK

&emsp;&emsp;The F-Stack version we choose is [1.22](https://github.com/F-Stack/f-stack/releases/tag/v1.22), 
and it has explicitly required DPDK version to be [20.11](https://github.com/DPDK/dpdk/releases/tag/v20.11).

&emsp;&emsp;Install dependencies:

```shell
yum install python3-pip
yum install numactl-devel zlib-devel ninja
pip3 install meson pyelftools
```

&emsp;&emsp;Build and install:

```shell
cd dpdk-20.11
CONFIG_RTE_LIBRTE_MLX5_PMD=y meson -Denable_kmods=true -Dtests=false build
cd build
ninja
ninja install
```

&emsp;&emsp;Run simple test:

```shell
# Allocate 10GB huge-pages
echo 5120 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# Attach your PF (with main IP) and one of the VFs (idle) to the poll-mode-driver test
./build/app/dpdk-testpmd -l 0-3 -n 4 -a 0000:03:00.0 -a 0000:03:00.2 -- --nb-cores=2 --flow-isolate-all -i -a
```

&emsp;&emsp;The `--flow-isolate-all` option is a MUST do. It enables Flow Bifurcation and ensures that all the
undetermined flow will be forwarded to the Linux kernel. Because the default behavior is to drop all packets, so
unless you configure the flow table or enable the `--flow-isolate-all` option, 
your network connection will be lost again ...

### Install F-Stack

#### Upgrade pkg-config

&emsp;&emsp;The `pkg-config` command in CentOS 7 is of version 0.27.1, and it has a [bug](https://bugs.freedesktop.org/show_bug.cgi?id=56699)
that does not correctly handle gcc's `--whole-archive` option.
As per F-Stack's document, we can upgrade it to [0.29.2](https://pkg-config.freedesktop.org/releases/pkg-config-0.29.2.tar.gz).

#### Modify make scripts

1. Edit `lib/Makefile`, comment out `DEBUG=...`. We want a release build.
2. Edit `lib/Makefile`, enable `FF_FLOW_ISOLATE=1`. It is the trigger of Flow Bifurcation for TCP. The hardcoded TCP port is 80.
3. Edit `mk/kern.mk`, add `-Wno-error=format-overflow` to `CWARNFLAGS`, in case a compiler warning being regarded as error.

#### Build and install

```shell
export FF_PATH=/root/f-stack-1.22  # Change to your own dir
export REGULAR_PKG_CONFIG_DIR=/usr/lib64/pkgconfig/
export DPDK_PKG_CONFIG_DIR=/usr/local/lib64/pkgconfig/
export PKG_CONFIG_PATH=$(pkg-config --variable=pc_path pkg-config):${REGULAR_PKG_CONFIG_DIR}:${DPDK_PKG_CONFIG_DIR}

cd f-stack-1.22/lib
make -j
make install
```

#### Configurations

&emsp;&emsp;F-Stack has a global config file at `/etc/f-stack.conf`. We need to make a few changes before running it.

1. Change `pkt_tx_delay=100` to `pkt_tx_delay=0`. So it will send packets immediately, rather than wait for a while.
2. Modify the `[port0]` section, including `addr`, `netmask`, `broadcast` and `gateway`. Keep the same to your 
test machine, because our DPDK app only needs to have a unique TCP port.
3. Add `pci_whitelist=03:00.0,03:00.2`. As explained above, the first one is your PF with main IP, the other is one of
its idle VFs. The Flow Bifurcation will forward specific TCP flow to VF, while leaving the rest traffic to the PF,
for the Linux kernel.

### Run Photon

&emsp;&emsp;We have provided a new [example](../examples/fstack-dpdk/fstack-dpdk-demo.cpp).
It looks quite alike the old echo server example, only a few lines of changes, but now the backend becomes DPDK.

```shell
cd PhotonLibOS
cmake -B build -D BUILD_TESTING=1 -D FETCH_GTEST_GFLAGS_SOURCE=1 -D ENABLE_FSTACK_DPDK=1 -D CMAKE_BUILD_TYPE=Release
cmake --build build -j -t fstack-dpdk-demo

./build/output/fstack-dpdk-demo
```