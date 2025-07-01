# how to compile
(1) compile spdk
- Download spdk source code in /path/to/spdk
- cd /path/to/spdk
- ./configure --with-shared
- make -j 8

(2) compile photon
photon spdk bdev wrapper rely on some spdk dynamic libraries, and these libraries rely on some dpdk libraries and the isal library.
- please change the ${SPDK_ROOT} in CMake/Findspdk.cmake to yours
- cd /path/to/photon
- cmake -B build -DPHOTON_ENABLE_SPDK=ON -DPHOTON_BUILD_TESTING=ON
- make -C build -j 8

# how to run
example
``` shell
sudo ./build/examples-output/bdev-example --json ./examples/spdk/bdev.json
sudo ./build/examples-output/nvme-example
```
tests
``` shell
sudo ./build/output/test-spdkbdev
sudo ./build/output/test-spdknvme
```