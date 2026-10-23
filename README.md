# qbook

qbook 是基于 C++23 的限价订单簿采集服务项目，按外部行情更新状态，不主动撮合。
V0 提供公共数据契约、配置加载、故障控制和原子指标基础。
V1 已实现独立的 `Orderbook`：新增、修改、成交、撤单、L2 绝对量更新与同步最优一档快照。
V2 已完成纯协议、Feed 接收、Book 多品种处理、周期快照和原事件转交，全部 115 项 CTest 验收通过。
正式服务入口、Parquet、指标发布、UI 和 benchmark 留待后续版本。

需求、规格、任务和实现计划均位于 [docs](docs/)。
V0 已通过 30 项自动化测试。完整验收记录位于本地 `docs/releases/V0.md`；
`docs/releases/` 按项目所有者要求保留在本地，不上传 GitHub。
V1 的完整验收记录保存到本地 `docs/releases/V1.md`；V2 的实际命令、输出核对和限制集中记录在本地 `docs/releases/V2.md`。
V1 历史 Debug 与 ASan/UBSan 结果均为 60/60；V2 开发前已重新运行基线，60/60 通过。

## 构建和测试

目标平台为 Linux / WSL2、x86-64、小端、8 字节指针。
需要 C/C++ 编译器、所用 C++23 特性、CMake 3.28 以上和 Ninja。
固定依赖源码随工程保留，配置和构建无需联网。默认从仓库源码构建 libzmq/cppzmq，不要求系统预装 ZMQ，也不需要 Arrow 或 Node。

从仓库根目录执行：

```bash
cmake -S . -B build/debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DQBOOK_BUILD_PIPELINE=ON \
  -DQBOOK_BUILD_SERVICE=OFF -DQBOOK_BUILD_BENCHMARKS=OFF \
  -DQLIB_BUILD_TESTS=OFF
cmake --build build/debug --parallel 4
ctest --test-dir build/debug --show-only
ctest --test-dir build/debug --output-on-failure --no-tests=error
```

日常开发复用 `build/debug`。`--show-only` 只列出用例，最后一条命令才运行测试。
`qbook_tests` 包含 69 项测试：V0/V1 普通回归 54 项、纯协议 15 项，CTest 前缀为 `unit::`。
`qbook_allocation_tests` 保留分配辅助自测和 F01～F05 共 6 项，前缀为 `allocation::`；
全局分配替换仅链接到这个专用程序。
`qbook_pipeline_tests` 包含 Feed 9 项、Book 15 项和链路 16 项，共 40 项，前缀为 `pipeline::`，
使用本机 TCP 临时端口与 XPUB 订阅通知建立就绪条件。
所有测试程序使用同一份生产库。

| 开关 | 实际构建范围 |
|---|---|
| 默认 `QBOOK_BUILD_PIPELINE=ON` | `qbook_support`、`qbook_core`、`qbook_protocol`、`qbook_pipeline` 和固定通信依赖。 |
| `QBOOK_BUILD_PIPELINE=OFF` | 支持库、核心库、纯协议库及其测试；不查找或构建 ZMQ。 |
| `BUILD_TESTING=OFF` | 按流水线开关构建生产库；不引入 Catch2、测试发布者、收集器或分配替换。 |

V2 最终 Debug 结果为 **115/115 通过，0 失败（8.03 秒）**。
关闭流水线的独立配置通过 75/75 测试，编译命令不含 ZMQ；关闭测试、启用流水线的配置成功构建四个生产库，未引入 Catch2 或测试辅助。
本轮网络测试在允许创建本机 socket 的授权环境执行；受限沙箱中的 EPERM 失败及重跑详情保留于本地 V2 记录。
V1 的历史 sanitizer 检查覆盖全部 60 项测试，包含分配故障用例并保持泄漏检查开启；V2 不重复这一矩阵。
`qbook_core` 仅链接 `qlib::qlib`，独立于支持库、日志、网络和文件依赖。
当前将 `QBOOK_BUILD_SERVICE=ON` 或 `QBOOK_BUILD_BENCHMARKS=ON` 视为配置错误。
当前没有 `qbook` 服务可执行文件。

## 固定依赖

| 依赖 | 内容版本与来源 | 接入与许可证 |
|---|---|---|
| qlib | 用户提供 `/mnt/d/Desktop/unsw/qlib`；本地 README 标识 `qlib-v4-20261020`，无 Git 元数据。以 [逐文件 SHA-256 清单](third_party/qlib.sha256) 固定完整副本。 | `third_party/qlib`，`add_subdirectory` / `qlib::qlib`，Apache-2.0。关闭其自带测试。 |
| Catch2 | [官方 v3.8.1](https://github.com/catchorg/Catch2/releases/tag/v3.8.1)，提交 `2b60af89e23d28eefc081bc930831ee9d45ea58b`。 | `third_party/Catch2`，`Catch2::Catch2WithMain`，BSL-1.0。 |
| fkYAML | 0.4.4；从本地 nqbook 提交 `daf66944cffbcac2772e213a95db6b21d7775a75` 的 `third_party/fkYAML` 提取。 | `third_party/fkYAML/include/fkYAML/node.hpp`，`qbook_yaml`，MIT。 |
| spdlog | 1.17.0，bundled fmt 12.1.0；从同一 nqbook 提交的 `third_party/spdlog` 提取。 | `third_party/spdlog/include/spdlog`，`qbook_logging`，MIT；默认 header-only，使用 bundled fmt。 |
| libzmq | [官方 v4.3.5 源码](https://codeload.github.com/zeromq/libzmq/tar.gz/refs/tags/v4.3.5)，完整 811 个文件；[内容清单](third_party/libzmq.sha256)。 | `third_party/libzmq`，官方 `libzmq` 动态库目标，MPL-2.0。 |
| cppzmq | [官方 v4.10.0 源码](https://codeload.github.com/zeromq/cppzmq/tar.gz/refs/tags/v4.10.0)，完整 34 个文件；[内容清单](third_party/cppzmq.sha256)。 | `third_party/cppzmq`，官方 `cppzmq` 接口目标传递链接 `libzmq`，MIT。 |

nqbook 的已核实远程为 `git@github.com:Zhou-London/nqbook.git`。
提取使用该提交的 Git 对象，原 nqbook 工作区未改动。
qlib 原目录只读，构建使用仓库内副本；更新依赖时须重新核对契约和内容清单。
不得单独启用 `SPDLOG_COMPILED_LIB`、`SPDLOG_FMT_EXTERNAL` 或 `SPDLOG_USE_STD_FORMAT`，
所有调用方使用同一 `qbook_logging` 配置。

六份清单采用标准 `sha256sum` 格式，相对路径以各自依赖目录为基准：

```bash
(cd third_party/qlib && sha256sum --check ../qlib.sha256)
(cd third_party/Catch2 && sha256sum --check ../Catch2.sha256)
(cd third_party/fkYAML && sha256sum --check ../fkYAML.sha256)
(cd third_party/spdlog && sha256sum --check ../spdlog.sha256)
(cd third_party/libzmq && sha256sum --check ../libzmq.sha256)
(cd third_party/cppzmq && sha256sum --check ../cppzmq.sha256)
```

恢复依赖使用随工程保存的固定源码与清单。
Catch2 也可从其固定提交的
[源码归档](https://codeload.github.com/catchorg/Catch2/tar.gz/2b60af89e23d28eefc081bc930831ee9d45ea58b)
恢复；归档 SHA-256 为
`553e6c94498c42edaecc9c8459f1a320a7e4cf49155635cf7123212bb9f09cf2`。
qlib 没有可用 Git 提交，不能通过拉取某个上游分支替代当前快照。

本机接入前没有可用的 libzmq/cppzmq 开发包，因此 V2 固定官方源码归档并保留许可证。
libzmq 归档 SHA-256 为 `6c972d1e6a91a0ecd79c3236f04cf0126f2f4dfbbad407d72b4606a7ba93f9c6`；
cppzmq 归档 SHA-256 为 `c81c81bba8a7644c84932225f018b5088743a22999c6d82a2b5f5cd1e6942b74`。
CMake 通过 `add_subdirectory` 使用原项目目标，不自动下载依赖。
通信库的测试、文档、性能工具、draft、CURVE、WebSocket 关闭；不引入 libbsd、libsodium、OpenPGM 或 NORM。
生成的 `libzmq.so.5.2.5` / SONAME 5 属于库 ABI 编号，源码版本仍是 4.3.5。

## 配置

[示例 YAML](config/config.yaml) 使用九个平坦键；两个周期单位均为整数毫秒。

| 键 | 默认值 |
|---|---|
| `feed_endpoint` | `"tcp://127.0.0.1:5555"` |
| `l2` | `false` |
| `feed_queue_capacity` | `16384` |
| `snapshot_period` | `3000` |
| `record_queue_capacity` | `16384` |
| `out_dir` | `"data_out"` |
| `batch_rows` | `1024` |
| `metrics_endpoint` | `"tcp://127.0.0.1:5556"` |
| `sample_period` | `100` |

`LoadConfig(path)` 允许缺项，`{}` 使用全部默认值。
只有明确缺失的文件才记录 warn 并使用默认配置。
空文档、多个文档、非映射根、未知键、重复键、null、错误类型及非法数值均失败。
布尔、整数与字符串严格区分，例如容量 `"3"` 和 `3.0` 均无效。
容量保存请求值，实际队列向上取 2 的幂；请求 3 得到 4 个可用槽位。

`ValidateConfig` 在取整、字节乘法、时间换算和到期点加法前检查表示范围。
验证不分配队列，不创建输出目录，不连接端点；数值合法也不保证实际资源足够。
C++ 配置不接受命令行或环境变量覆盖；后续 UI 的构建变量不覆盖 C++ 配置。

## 公共接口与并发约定

- [PipelineTypes.h](include/PipelineTypes.h) 直接复用 qlib 事件和队列，并冻结全部协议字段类型、大小和偏移。variant、本地指针及原子对象不能直接作为通用网络格式。
- [Protocol.h](include/Protocol.h) 提供 `DecodeFrame`、`ValidateEvent` 和明确的拒绝结果；按空帧、标签、长度、模式、字段顺序校验，独立样本见 [协议 fixtures](tests/fixtures/protocol/README.md)。
- [FeedThread.h](include/Threads/FeedThread.h) 在 `Run` 线程持有 SUB/context；区分收到消息、有效解码和成功入队，只有成功入队表示接纳，multipart 整体拒绝。
- [BookThread.h](include/Threads/BookThread.h) 独占多品种订单簿；应用或业务忽略后均转交原事件，按单调时钟输出已知品种快照。
- [Config.h](include/Config.h) 提供默认参数、`LoadConfig` 和 `ValidateConfig`，公共头不传播 YAML 类型。
- [Orderbook.h](include/Orderbook.h) 提供 `OnOrder`、`OnTrade`、`OnCancel`、`OnLevel`、`OnSnapshot`、`size` 和 `MemoryBytes`，构造时绑定品种和 `BookMode::Mbo` 或 `BookMode::L2`。
- [Runtime.h](include/Runtime.h) 的 `RunControl` 保留第一个原子交换获胜者的故障。运行线程只读 `Failed()`；所有潜在报告者 join 后才能读 `Error()` 和 `Stage()`。
- [MetricsThread.h](include/Threads/MetricsThread.h) 当前只提供 `Metric`、`Metrics` 和 `SnapshotMetrics`。每个指标仅有一个写者；溢出保持旧值并抛错，快照逐字段读取，不保证整组来自同一瞬间。

`Orderbook` 的更新和读取均由同一个所有者串行执行，`const` 快照也不能与写入并发。
入口接受已通过协议校验的事件。事件通过 `const&` 借用，输入字段与链接保持原值；订单簿只改变自己的副本。
`ApplyResult` 区分实际更新和业务忽略，两种正常结果均保存本次事件时间，允许 0 和时间回退。
品种或模式误用抛 `std::invalid_argument`；分配异常向上传播，局部回滚保留已有业务状态。
同价改单保序，改价进入目标末尾；成交按两个已知 ID 独立扣减，L2 成交只更新时间。
`OnSnapshot()` 返回独立值，空侧为 0/0；`size()` 是本地逐笔挂单数，L2 为 0。

`MemoryBytes()` 按订单与 ID 容量、价位个数估计存储，超界饱和为 `SIZE_MAX`。
估计不含对象本体、分配器、hive 管理信息及 ID 控制区额外哨兵字节，不能当作 RSS 或精确分配量。
固定公式见 [Orderbook 规格](docs/specs.md#4321-orderbook)。
`Orderbook` 禁止复制，支持不抛异常的移动；活订单地址随拥有关系转移，容器迭代器须重取。
移动后的来源只保证可析构或重新移动赋值；外层品种表插入后也须重新查找订单簿。

价格单位为 `1/10^10`，数量单位为 `1/10^8`；时间为 Unix epoch 纳秒。
周期与耗时使用单调时钟。`QLIB_CACHE_LINE` 在本项目固定为 64，
它是对齐配置，不代表检测了硬件缓存行或保证整个服务无锁。

Feed 只有 `try_push` 成功才接纳事件；普通停止可以放弃尚未入队的消息。
正常关闭按 Feed stop/join → Book 排空/join → 下游排空/join 顺序执行；Fatal 解除队列等待，不保证剩余输出完整。
Book 在第 1、1025、2049……条完成应用事件上抽样，计时只包围分发与 Orderbook 调用。
周期快照保留最近事件时间，暂停行情仍可输出已知空簿，普通停止不补最终快照。
V2 的有限发布者与 `RecordCollector` 仅供测试，记录在 join 后逐字段核对，`writer_*` 指标保持原有含义。
