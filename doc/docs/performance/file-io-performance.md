---
sidebar_position: 1
toc_max_heading_level: 4
---

# File IO Performance

Compare Photon with `fio` when reading an 3.5TB NVMe raw device.

### Photon

#### Code

https://github.com/alibaba/PhotonLibOS/blob/main/examples/perf/io-perf.cpp

#### Command

The test program will read-only open the SSD device, and run random reads on that.

```bash
./io-perf --disk_path=/dev/nvme0n1 --disk_size=3000000000000 --io_depth=128 --io_size=4096 --io_uring 
```

#### Parameters

- Since we are not able to get disk size without third-party libs, we need to specify the disk_size. Could be any approximate number.
- The default IO engine is libaio. --io_uring means to use io_uring instead. We need to upgrade kernel to 6.x to get the best performance.


### Test cmd

```bash
fio --filename=/dev/nvme0n1p1 --direct=1 --ioengine=libaio --iodepth=128 --rw=randread --bs=4k --size=100% --group_reporting --name=randread --numjobs=1
```

### Results

|        | IO Engine |  IO Type  | IO Size | IO Depth | DirectIO |  QPS  | Throughput | CPU util |
| :----: | :-------: | :-------: | :-----: | :------: | :------: | :---: | :--------: | :------: |
| Photon | io_uring  | Rand-read |   4KB   |   128    |   Yes    | 433K  |   1.73GB   |   100%   |
| Photon |  libaio   | Rand-read |   4KB   |   128    |   Yes    | 346K  |   1.38GB   |   100%   |
|  fio   |  libaio   | Rand-read |   4KB   |   128    |   Yes    | 279K  |   1.11GB   |   100%   |

:::note

fio only enables 1 job (process).

:::

### Conclusion

- Photon could outperform `fio` by 50%, under this circumastance.
- Even if switching the IO engine from `io_uring` to `libaio`, Photon could still surpass.