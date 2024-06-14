---
sidebar_position: 1
toc_max_heading_level: 4
---

# IO性能

对比Photon和`fio`的读能力，测试对象是一块3.5TB的NVMe SSD

### Photon

#### 测试代码

https://github.com/alibaba/PhotonLibOS/blob/main/examples/perf/io-perf.cpp

#### 测试命令

测试程序会以只读方式打开SSD设备，并进行随机读

```bash
./io-perf --disk_path=/dev/nvme0n1 --disk_size=3000000000000 --io_depth=128 --io_size=4096 --io_uring 
```

#### 参数说明

- 由于不使用三方库无法获得磁盘大小，因此需要手动设置disk_size参数。无需精确值。如指定3TB
- 默认使用libaio作为IO引擎，--io_uring参数表示开启io_uring，需要把内核升级到6.x才能得到最好效果

### fio

```bash
fio --filename=/dev/nvme0n1 --direct=1 --ioengine=libaio --iodepth=128 --rw=randread --bs=4k --size=100% --group_reporting --name=randread --numjobs=1
```

### 测试结果

|        | IO Engine |  IO Type  | IO Size | IO Depth | DirectIO |  QPS  | Throughput | CPU util |
| :----: | :-------: | :-------: | :-----: | :------: | :------: | :---: | :--------: | :------: |
| Photon | io_uring  | Rand-read |   4KB   |   128    |   Yes    | 433K  |   1.73GB   |   100%   |
| Photon |  libaio   | Rand-read |   4KB   |   128    |   Yes    | 346K  |   1.38GB   |   100%   |
|  fio   |  libaio   | Rand-read |   4KB   |   128    |   Yes    | 279K  |   1.11GB   |   100%   |

:::note
fio只开启一个job（线程）
:::

### 结论

- Photon在上述情况下比`fio`最多可高出50%的性能
- 即使把IO引擎从`io_uring`换到`libaio`，Photon仍然胜出