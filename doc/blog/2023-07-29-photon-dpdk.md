---
slug: photon-dpdk
title: How to run Photon on top of DPDK
authors: [beef9999]
tags: [DPDK, F-Stack]
---

import Tabs from '@theme/Tabs';
import TabItem from '@theme/TabItem';

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

```mdx-code-block
<Tabs groupId="os" queryString>
  <TabItem value="CentOS 7" label="CentOS 7">
```

```bash
# Edit /etc/default/grub
# Expand GRUB_CMDLINE_LINUX with 'intel_iommu=on iommu=pt pci=realloc'
grub2-mkconfig -o /boot/grub2/grub.cfg
reboot
```

```mdx-code-block
  </TabItem>
  <TabItem value="Debian 10" label="Debian 10">
```

```bash
# Edit /etc/default/grub
# Expand GRUB_CMDLINE_LINUX with 'intel_iommu=on iommu=pt'
grub-mkconfig -o /boot/grub/grub.cfg
reboot
```

```mdx-code-block
  </TabItem>
</Tabs>
```

Note the `pci=realloc` is a work-around solution for CentOS and RHEL.
Without this, kernel would report `not enough MMIO resources for SR-IOV`,
see this [issue](https://access.redhat.com/solutions/37376).

#### 2. Set VF number

```shell
echo 4 > /sys/class/net/eth0/device/sriov_numvfs
```

&emsp;&emsp;If you are having an Intel NIC, this step is likely to succeed. However, for the Mellanox one, 
it might fail because of the lack of proper mlx driver in your kernel. 
Please check the result by typing `lspci -nn | grep Ethernet` and see if the NICs' virtual function number is correct.

&emsp;&emsp;If succeeded, please jump to the part of 'Install DPDK'.

&emsp;&emsp;If failed, you may need to download the official driver from NVidia.
There are many available releases in https://network.nvidia.com/products/infiniband-drivers/linux/mlnx_ofed/,
you should choose one that matches to your kernel version and OS version the best.
An improper version might lead to compiling error when building kernel modules later. 

- For example, for CentOS 7 and kernel 5.x, you should choose MLNX_OFED_LINUX-5.4-3.6.8.1-rhel7.2-x86_64.tgz
- For Debian 10, it is MLNX_OFED_LINUX-5.8-5.1.1.2-debian10.13-x86_64.tgz

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

# Update initramfs
dracut -f

# There will be rdma-core, rdma-core-devel, librdmacm and librdmacm-utils.
rpm -qa | grep rdma
```

&emsp;&emsp;Now we need to restart the server. Be careful, there is a possibility that the interface name
of your NIC might change, for example, from `eth0` to something like `enp3s0f0`, where 3 for Bus, 0 for Device,
and 0 for Function, represented in the `03:00.0` BDF notation. It will incur connection failure
of your server and unable to log in.

&emsp;&emsp;To solve this, your first option is to disable the Consistent Interface Device Naming in Linux,
and then persist the new names by `udev rules`. See the NVidia docs at
[1](https://docs.nvidia.com/networking/display/MLNXOFEDv541030/Changes+and+New+Features#ChangesandNewFeatures-CustomerAffectingChanges),
[2](https://enterprise-support.nvidia.com/s/article/howto-change-network-interface-name-in-linux-permanently).

1. Append `GRUB_CMDLINE_LINUX` in `/etc/default/grub` with `net.ifnames=0`
2. Create the `/etc/udev/rules.d/85-net-persistent-names.rules` with the following content

```text
# PCI device 15b3:1019 (mlx5_core)
# NAME:="some name" , := is used to make sure that device name will be persistent.
SUBSYSTEM=="net", ACTION=="add", DRIVERS=="?*", ATTR{address}=="00:02:c9:fa:c3:50", ATTR{dev_id}=="0x0", ATTR{type}=="1", KERNEL=="eth*", NAME:="eth0"
SUBSYSTEM=="net", ACTION=="add", DRIVERS=="?*", ATTR{address}=="00:02:c9:fa:c3:51", ATTR{dev_id}=="0x0", ATTR{type}=="1", KERNEL=="eth*", NAME:="eth1"
```

&emsp;&emsp;The second option, if you are OK with the new names, you can update the NIC scripts 
in `/etc/sysconfig/network-scripts/` and make them correct.

&emsp;&emsp;Finally, everything get ready, just reboot:

```shell
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

&emsp;&emsp;The F-Stack version we choose is [1.22.1](https://github.com/F-Stack/f-stack/releases/tag/v1.22.1), 
and it has a subdirectory called dpdk that contains the full DPDK 20.11 source code. 
Let's start with the DPDK install first.

```mdx-code-block
<Tabs groupId="os" queryString>
  <TabItem value="CentOS 7" label="CentOS 7">
```

```bash
cd f-stack-1.22.1/dpdk/
yum install python3-pip
yum install pkg-config numactl-devel zlib-devel ninja
pip3 install meson 
```

```mdx-code-block
  </TabItem>
  <TabItem value="Debian 10" label="Debian 10">
```

```bash
cd f-stack-1.22.1/dpdk/
pip3 install ninja meson
apt install pkg-config python3-pyelftools libnuma-dev
```

```mdx-code-block
  </TabItem>
</Tabs>
```

Build and install:

```shell
CONFIG_RTE_LIBRTE_MLX5_PMD=y meson -Denable_kmods=true -Dtests=false build
cd build
ninja
ninja install
```

Allocate 10GB huge-pages

```shell
echo 5120 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

Attach your PF (with main IP) and one of the VFs (idle) to the poll-mode-driver test

```shell
./build/app/dpdk-testpmd -l 0-3 -n 4 -a 0000:03:00.0 -a 0000:03:00.2 -- --nb-cores=2 --flow-isolate-all -i -a
```

Note: The `--flow-isolate-all` option is a MUST do. It enables Flow Bifurcation and ensures that all the
undetermined flow will be forwarded to the Linux kernel. Because the default behavior is to drop all packets, so
unless you configure the flow table or enable the `--flow-isolate-all` option, 
your network connection will be lost again ...

### Install F-Stack

Let's go back to the parent dir and install F-Stack.

#### Upgrade pkg-config

&emsp;&emsp;The `pkg-config` command in CentOS 7 is of version 0.27.1, and it has a [bug](https://bugs.freedesktop.org/show_bug.cgi?id=56699)
that does not correctly handle gcc's `--whole-archive` option.
As per F-Stack's document, we can upgrade it to [0.29.2](https://pkg-config.freedesktop.org/releases/pkg-config-0.29.2.tar.gz).

&emsp;&emsp;Debian 10 is OK.

#### Modify make scripts

1. Edit `lib/Makefile`, comment out `DEBUG=...`. We want a release build.
2. Edit `lib/Makefile`, enable `FF_FLOW_ISOLATE=1`. It is the trigger of Flow Bifurcation for TCP. The hardcoded TCP port is 80.
3. For CentOS 7, edit `mk/kern.mk`, add `-Wno-error=format-overflow` to `CWARNFLAGS`, 
in case a compiler warning being regarded as error. Debian 10 is OK.

#### Build and install

```mdx-code-block
<Tabs groupId="os" queryString>
  <TabItem value="CentOS 7" label="CentOS 7">
```

```bash
export FF_PATH=/root/f-stack-1.22.1  # Change to your own dir
export REGULAR_PKG_CONFIG_DIR=/usr/lib64/pkgconfig/
export DPDK_PKG_CONFIG_DIR=/usr/local/lib64/pkgconfig/
export PKG_CONFIG_PATH=$(pkg-config --variable=pc_path pkg-config):${REGULAR_PKG_CONFIG_DIR}:${DPDK_PKG_CONFIG_DIR}

cd f-stack-1.22/lib
make -j
make install
```

```mdx-code-block
  </TabItem>
  <TabItem value="Debian 10" label="Debian 10">
```

```bash
export FF_PATH=/root/f-stack-1.22.1  # Change to your own dir

cd f-stack-1.22/lib
make -j
make install
```

```mdx-code-block
  </TabItem>
</Tabs>
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

&emsp;&emsp;We have provided a new [example](https://github.com/alibaba/PhotonLibOS/blob/main/examples/fstack-dpdk/fstack-dpdk-demo.cpp).
It looks quite alike the old echo server example, only a few lines of changes, but now the backend becomes DPDK.

```shell
cd PhotonLibOS
git checkout release/0.8
cmake -B build -D PHOTON_BUILD_TESTING=1 -D PHOTON_ENABLE_FSTACK_DPDK=1 -D CMAKE_BUILD_TYPE=Release
cmake --build build -j 32 -t fstack-dpdk-demo

./build/output/fstack-dpdk-demo
```

Now you can set up an echo client on another host, and bench this server via port 80.
