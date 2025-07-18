# how to compile
photon spdk bdev wrapper rely on some spdk dynamic libraries, and these libraries rely on some dpdk libraries.
- please first install dependience libraries according to CMakeLists.txt, such as openssl, zlib, curl, isal, dpdk, spdk, etc.
- cd /path/to/photon
- cmake -B build -DPHOTON_ENABLE_SPDK=ON -DPHOTON_BUILD_TESTING=ON
- make -C build -j 8

You can also choose to build dependencies from source by open the cmake option: "-DPHOTON_BUILD_DEPENDENCIES=ON"

# how to run
example
``` shell
# run bdev example
sudo ./build/examples-output/bdev-example --json ./examples/spdk/bdev.json
# run nvme example
# change trid to yours
sudo ./build/examples-output/nvme-example "trtype:pcie traddr:0000:86:00.0"
sudo ./build/examples-output/nvme-example "trtype:tcp adrfam:ipv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1"  # for nvmf
```
tests
``` shell
sudo ./build/output/test-spdkbdev
sudo ./build/output/test-spdknvme
```