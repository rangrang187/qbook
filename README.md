# qbook

qbook 是基于 C++23 的限价订单簿采集服务项目，按外部行情更新状态，不主动撮合。
V0 提供公共数据契约、配置加载、故障控制和原子指标基础。
V1 已实现独立的 `Orderbook`：新增、修改、成交、撤单、L2 绝对量更新与同步最优一档快照。
V2 已完成纯协议、Feed 接收、Book 多品种处理、周期快照和原事件转交，全部 115 项 CTest 验收通过。
V3 已实现五类 Parquet、Writer 阶段、正式三阶段服务及 SIGINT/SIGTERM 有序停止。
V3 初次验收的完整 Debug 154/154、Release 存储与服务 39/39、核心关闭分支 75/75 均通过。
V4 已实现指标发布、四阶段生命周期、Node/SSE 桥接和只读监控页面。
V4 历史完整 C++ 回归 **167/167**、UI 单元与组件测试 **153/153** 及类型检查通过；
100/2000 ms 两组真实 Chromium 验收各 **3/3** 通过，观察端离线后继续业务及文件读回通过。V5已实现四个真实实验程序与分析器，并完成本轮功能及独立干净构建复验：Debug167/167、Release201/201、Python34/34、UI153/153及两周期浏览器各3/3，最终新构建浏览器再通过3/3。53个正式case共265轮测量，264有效、1个L2稳态轮无效；OI-04绝对门槛待定，不声明V5完整验收通过。详见[结果与限制](docs/benchmarks/V5-results.md)。
V4 验收后已清理当时新建的 `build/`、`ui/node_modules/`、`ui/.next/` 和测试输出；下次按下文重新构建及 `npm ci`。

需求、规格、任务和实现计划均位于 [docs](docs/)。
V0 已通过 30 项自动化测试。完整验收记录位于本地 `docs/releases/V0.md`；
`docs/releases/` 按项目所有者要求保留在本地，不上传 GitHub。
V1 的完整验收记录保存到本地 `docs/releases/V1.md`；V2 的实际命令、输出核对和限制集中记录在本地 `docs/releases/V2.md`。
V1 历史 Debug 与 ASan/UBSan 结果均为 60/60；V2 开发前已重新运行基线，60/60 通过。
V3 的实际命令、文件读回与故障结果集中在本地 `docs/releases/V3.md`。
V4 历史环境、命令、跨语言样本与浏览器验收记录位于本地 `docs/releases/V4.md`，不覆盖前置版本结果。

## 构建和测试

目标平台为 Linux / WSL2、x86-64、小端、8 字节指针。
需要 C/C++ 编译器、所用 C++23 特性、CMake 3.28 以上和 Ninja。
基础依赖源码随工程保留。默认从仓库源码构建 libzmq/cppzmq，不要求系统预装 ZMQ。
持久化默认关闭；完整服务使用开发环境提供的 Arrow/Parquet C++ 26.0.0，准备好后 C++ 配置和构建不联网。
C++ 构建不依赖 Node；监控页面是 [ui/](ui/README.md) 下独立的 npm 工程。

当前使用 [.devcontainer/devcontainer.json](.devcontainer/devcontainer.json) 指定的 `cpp-dev:latest` 开发容器，
工作目录为 `/mnt/d/Desktop/unsw/qbook`。以下构建、测试和运行命令均在容器内、仓库根目录执行。
容器已安装 Arrow/Parquet 开发包和动态库 `26.0.0-1`，CMake 包目录位于
`/usr/lib/x86_64-linux-gnu/cmake/{Arrow,Parquet}`；无需运行项目的依赖安装脚本。
当前仓库未提供该镜像的 Dockerfile，也未固定镜像 digest；`cpp-dev:latest` 本身不足以在另一台机器复现环境。
保留的包锁文件与恢复脚本只固定历史 Arrow 安装内容，不能代替完整镜像构建资料。

从仓库根目录执行：

```bash
cmake -S . -B build/debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DQBOOK_BUILD_PIPELINE=ON \
  -DQBOOK_BUILD_PERSISTENCE=OFF \
  -DQBOOK_BUILD_SERVICE=OFF -DQBOOK_BUILD_BENCHMARKS=OFF \
  -DQLIB_BUILD_TESTS=OFF
cmake --build build/debug --parallel 4
ctest --test-dir build/debug --show-only
ctest --test-dir build/debug --output-on-failure --no-tests=error
```

日常开发复用 `build/debug`。`--show-only` 只列出用例，最后一条命令才运行测试。
VS Code 容器配置自动安装 LLVM clangd 扩展；仓库根 `.clangd` 指向
`build/debug/compile_commands.json`，由 CMake 配置阶段生成，用于头文件搜索、C++23 解析和跳转。
首次打开或清理 `build/` 后，先执行上述 `cmake -S . -B build/debug ...` 配置命令；
仅生成编译数据库无需执行完整构建。编辑持久化或服务源码时，使用下方完整功能配置启用对应目标。
若编辑器仍显示旧诊断，在命令面板执行 `clangd: Restart language server`。
`qbook_tests` 包含 69 项测试：V0/V1 普通回归 54 项、纯协议 15 项，CTest 前缀为 `unit::`。
`qbook_allocation_tests` 保留分配辅助自测和 F01～F05 共 6 项，前缀为 `allocation::`；
全局分配替换仅链接到这个专用程序。
`qbook_pipeline_tests` 包含 Feed 9 项、Book 15 项和链路 16 项，共 40 项，前缀为 `pipeline::`，
使用本机 TCP 临时端口与 XPUB 订阅通知建立就绪条件。
所有测试程序使用同一份生产库。
完整 V3 历史注册 `qbook_storage_tests` 的 18 项 `storage::` 测试和
`qbook_service_tests` 的 21 项 `service::` 测试，总计 154 项。
V4 增加 `qbook_metrics_tests` 的 11 项 `metrics::` 测试，服务测试扩展至 23 项，总计 167 项。
服务测试驱动只在测试构建中存在，链接同一份服务实现；不替代正式程序。

| 开关 | 实际构建范围 |
|---|---|
| 默认 `QBOOK_BUILD_PIPELINE=ON` | `qbook_support`、`qbook_core`、`qbook_protocol`、`qbook_pipeline`、`qbook_metrics` 和固定通信依赖。 |
| `QBOOK_BUILD_PIPELINE=OFF` | 支持库、核心库、纯协议库及其测试；不查找或构建 ZMQ。 |
| `QBOOK_BUILD_PERSISTENCE=ON` | 增加 `qbook_storage`、`qbook_writer` 和固定 Arrow/Parquet；这两个库不依赖 ZMQ。默认 OFF。 |
| `QBOOK_BUILD_SERVICE=ON` | 增加 `qbook_service` 和真实 `qbook`；必须同时显式开启 PIPELINE、PERSISTENCE。默认 OFF。 |
| `BUILD_TESTING=OFF` | 不引入 Catch2、测试驱动或分配替换；可独立构建显式开启的实验，其纯观察/文件核对支撑只链接实验。 |
| `QBOOK_BUILD_BENCHMARKS=ON` | 默认 OFF；建立独立实验及可选 `benchmark::` 正确性检查，不给普通测试设置机器速度门槛。 |

V2 最终 Debug 结果为 **115/115 通过，0 失败（8.03 秒）**。
关闭流水线的独立配置通过 75/75 测试，编译命令不含 ZMQ；关闭测试、启用流水线的配置成功构建四个生产库，未引入 Catch2 或测试辅助。
V2 网络测试在允许创建本机 socket 的授权环境执行；当时受限沙箱中的 EPERM 失败及重跑详情保留于本地 V2 记录。
V1 的历史 sanitizer 检查覆盖全部 60 项测试，包含分配故障用例并保持泄漏检查开启；V2 不重复这一矩阵。
`qbook_core` 仅链接 `qlib::qlib`，独立于支持库、日志、网络和文件依赖。
`QBOOK_BUILD_BENCHMARKS=ON` 建立真实 core/protocol 实验；PERSISTENCE 开启时增加 Writer，SERVICE 开启时增加 Pipeline，不自动打开其他能力。实验说明及完整 Release 命令见 [benchmarks/README.md](benchmarks/README.md)，冻结设计见 [V5-experiment.md](docs/benchmarks/V5-experiment.md)。

完整功能回归在开发容器中显式选择系统包；以下命令覆盖V0～V4，V5本轮已实际执行：

```bash
cmake -S . -B build/debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DQBOOK_BUILD_PIPELINE=ON -DQBOOK_BUILD_PERSISTENCE=ON \
  -DQBOOK_BUILD_SERVICE=ON -DQBOOK_BUILD_BENCHMARKS=OFF -DQLIB_BUILD_TESTS=OFF \
  -DArrow_DIR=/usr/lib/x86_64-linux-gnu/cmake/Arrow \
  -DParquet_DIR=/usr/lib/x86_64-linux-gnu/cmake/Parquet
cmake --build build/debug --parallel 4
ctest --test-dir build/debug --show-only
ctest --test-dir build/debug --output-on-failure --no-tests=error
```

跨宿主机与容器不要直接沿用含旧绝对路径的 CMake 缓存；需要时用 `cmake --fresh` 加上同样的配置参数重新生成。
`Arrow_DIR` 和 `Parquet_DIR` 指向同一环境中的安装，不会自动使用项目 `.deps`。
网络测试需要允许本机 TCP/IPC socket，进程测试需要允许 fork/exec 和向自身子进程发送信号。
V3 验收于 2026-10-23 实际执行：Debug 154/154（25.30 秒），Release 的
`storage::` / `service::` 39/39（14.94 秒），关闭三个能力开关后的核心 75/75（3.51 秒）。
关闭测试的完整服务配置可构建，编译源和目标不包含 Catch2 或测试代码；
服务缺少 PIPELINE 或 PERSISTENCE 的配置均明确失败。这些耗时仅为测试运行记录。

2026-10-25 切换开发容器系统库后，完整 Debug 重新发现并运行 **154/154（15.97 秒）**。
确认缓存、链接命令和全部七个程序的运行库路径不含 `.deps` 后，已删除项目内的旧安装。
删除后重新配置和增量构建成功，`storage::` / `service::` 再次 **39/39 通过（6.20 秒）**，
包含 ZSTD Parquet 读回、真实网络及 SIGINT/SIGTERM 停止。随后按用户要求清理了 `build/`；下次按上述命令重新构建。
该次V3容器切换未重跑Release、核心关闭分支或sanitizer，详情保留在本地 `docs/releases/V3.md`。

## 固定依赖

| 依赖 | 内容版本与来源 | 接入与许可证 |
|---|---|---|
| qlib | 用户提供 `/mnt/d/Desktop/unsw/qlib`；本地 README 标识 `qlib-v4-20261020`，无 Git 元数据。以 [逐文件 SHA-256 清单](third_party/qlib.sha256) 固定完整副本。 | `third_party/qlib`，`add_subdirectory` / `qlib::qlib`，Apache-2.0。关闭其自带测试。 |
| Catch2 | [官方 v3.8.1](https://github.com/catchorg/Catch2/releases/tag/v3.8.1)，提交 `2b60af89e23d28eefc081bc930831ee9d45ea58b`。 | `third_party/Catch2`，`Catch2::Catch2WithMain`，BSL-1.0。 |
| fkYAML | 0.4.4；从本地 nqbook 提交 `daf66944cffbcac2772e213a95db6b21d7775a75` 的 `third_party/fkYAML` 提取。 | `third_party/fkYAML/include/fkYAML/node.hpp`，`qbook_yaml`，MIT。 |
| spdlog | 1.17.0，bundled fmt 12.1.0；从同一 nqbook 提交的 `third_party/spdlog` 提取。 | `third_party/spdlog/include/spdlog`，`qbook_logging`，MIT；默认 header-only，使用 bundled fmt。 |
| libzmq | [官方 v4.3.5 源码](https://codeload.github.com/zeromq/libzmq/tar.gz/refs/tags/v4.3.5)，完整 811 个文件；[内容清单](third_party/libzmq.sha256)。 | `third_party/libzmq`，官方 `libzmq` 动态库目标，MPL-2.0。 |
| cppzmq | [官方 v4.10.0 源码](https://codeload.github.com/zeromq/cppzmq/tar.gz/refs/tags/v4.10.0)，完整 34 个文件；[内容清单](third_party/cppzmq.sha256)。 | `third_party/cppzmq`，官方 `cppzmq` 接口目标传递链接 `libzmq`，MIT。 |
| Arrow C++ / Parquet C++ | 当前开发容器的 Ubuntu noble amd64 包，均为 **26.0.0-1**；[历史固定包清单](cmake/arrow-dependencies.lock.json) 保留原安装的 URL、版本与 SHA-256。 | 容器 `/usr`；官方 `Arrow::arrow_shared`、`Parquet::parquet_shared`，Apache-2.0；使用动态链接。 |

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

当前容器由系统安装提供 Arrow/Parquet，头文件位于 `/usr/include/arrow`、`/usr/include/parquet`，
许可证位于 `/usr/share/doc/<package>/copyright`。容器现有包数据库可以确认已安装版本，
但当前可见资料不能确认镜像构建时使用的下载地址和完整安装步骤。

[历史固定安装脚本](scripts/install_arrow_dependencies.py) 和包锁文件继续保留，
因为可复现的容器构建流程尚未接替它们。脚本要求 Ubuntu 24.04 amd64、Python 3.11+ 和 `dpkg-deb`，
校验 37 个包后向 `.deps/arrow-26.0.0` 解包，不修改系统包数据库；缓存齐全时可加 `--offline`。
锁文件记录 [Apache 官方包仓库](https://packages.apache.org/artifactory/arrow/ubuntu/) 与 Ubuntu 官方仓库的历史来源。
这些文件用于恢复旧包内容，不是当前容器的构建步骤，脚本产物不会被 CMake 自动选中；
本轮不验证非容器的本地恢复构建方式。`.deps/` 仍在 Git 忽略清单中。

[CMake 接入](cmake/ArrowDependency.cmake) 只在持久化开启时查找 **26.0.0 EXACT**。
当前实际库为 `/usr/lib/x86_64-linux-gnu/libarrow.so.2600.0.0` 和
`/usr/lib/x86_64-linux-gnu/libparquet.so.2600.0.0`，ZSTD 系统包为 `1.5.5+dfsg2-2build1.1`。
依赖接入不再设置项目 `.deps` 的绝对 RPATH，也不修改 `LD_LIBRARY_PATH`；
Arrow/Parquet 的运行时依赖由容器系统库解析。项目自行构建的 libzmq 仍通过构建目录的运行库路径加载。

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
C++ 配置不接受命令行或环境变量覆盖；UI 的构建变量不覆盖 C++ 配置。

## 运行与文件

完整构建后，从需要使用的工作目录启动；程序读取该目录的 `config/config.yaml`：

```bash
./build/debug/qbook
```

前台运行按 Ctrl+C 正常停止。也可在同一 Bash 会话中明确保存进程 PID：

```bash
./build/debug/qbook &
qbook_pid=$!
```

确认日志出现 `service_control_ready` 后，在同一会话请求停止并等待退出：

```bash
kill -TERM "$qbook_pid"
wait "$qbook_pid"
```

正常退出码为 0；配置、初始化、运行或关闭失败均非零。启动按 Metrics → Writer → Book → Feed；
正常停止按 Feed stop/join → Book 排空/join → Writer 排空、Close、join → Metrics stop/join。
`writer_opened` 日志公布本次独占 `data_out/run-<标识>/` 路径，重复运行不会覆盖旧文件。
`feed_connected` 表示本地接收资源初始化，不能证明发布端在线或行情已流入。
`metrics_bound` 表示本地指标端口已绑定；没有订阅者或 Node 离线不使业务失败。
绑定及不可恢复发布错误仍触发全体取消。指标不保证在正常停止时额外发送最终帧，最终文件行数以 Close 后读回为准。

每次固定创建以下五文件；没有记录的类型仍有可读 schema。字段全部非空：

| 文件 | 列顺序 |
|---|---|
| `orders.parquet` | seq、order_id、price、qty、new_qty、event_ns、recv_ns、instrument_id、side、type、action |
| `trades.parquet` | seq、buy_order_id、sell_order_id、price、qty、event_ns、recv_ns、instrument_id、side |
| `cancels.parquet` | seq、order_id、qty、event_ns、recv_ns、instrument_id、side |
| `levels.parquet` | seq、price、qty、event_ns、recv_ns、instrument_id、side |
| `books.parquet` | event_ns、recv_ns、instrument_id、bid_price、bid_qty、ask_price、ask_qty |

instrument_id 为 uint32；side/type/action 为 uint8；其他标量为 int64。
快照四数组为 `fixed_size_list<int64>[1]`，元素也非空；保存 Arrow schema 和 ZSTD 压缩。
schema 元数据 `qbook.format_version=1`，另含 `qbook.price_scale=10000000000`、
`qbook.qty_scale=100000000`、`qbook.time_unit=unix_epoch_ns`、`qbook.snapshot_depth=1`，
以及 side、order_type、order_action 的枚举映射。原事件保持整数和删除数量，不保存本机链接指针。

五类各自按 `batch_rows` 写批次；物理 row group 数不等于应用批次数的公开契约。
只有成功入 FeedQueue 才属接纳，Add 成功只代表追加；正常 Close 成功并读回才能证明本次输出完整。
故障目录可能含部分文件，应按失败采集处理；不重试结果不明的批次，不承诺断电安全、强杀恢复或跨文件全序。
网络和文件访问权限由受控运行环境管理，本版未提供公网认证或多租户隔离。

## 监控页面

UI 使用 Next.js App Router、React、TypeScript 与 Node 版 zeromq。
本轮实际 Node **24.22.1 LTS**、npm **11.19.1**；直接依赖固定版本和唯一
`ui/package-lock.json` 一起维护，详细版本和官方来源见 [UI 说明](ui/README.md)。
当前容器原先没有 Node/npm，本轮将经官方 SHA-256 校验的固定工具安装到
`/opt/node-v24.22.1-linux-x64`，通过 `/usr/local/bin` 提供 node/npm；清理临时 build 不影响它。
新建容器仍需安装同版本工具，现有镜像名称本身不保证包含这些工具。
不把 Node、UI 或原生 zeromq 依赖加入 C++ 核心库。

先在另一个容器终端运行完整 qbook，确认 YAML 中 `sample_period: 100`。
从仓库根进入 UI 后执行：

```bash
cd ui
npm ci
npm run typecheck
npm run test:unit
NEXT_PUBLIC_METRICS_SAMPLE_PERIOD_MS=100 npm run build
QBOOK_METRICS_ENDPOINT=tcp://127.0.0.1:5556 npm start -- --hostname 0.0.0.0 --port 3000
```

通过 VS Code Ports 面板转发容器 3000 端口，在宿主机浏览器访问 `http://localhost:3000`。
浏览器只访问 HTTP/SSE，不连接指标端口；`QBOOK_METRICS_ENDPOINT` 是 Node 服务端配置，不能由请求指定。
关闭浏览器或停止 Node 只结束观察，qbook 继续处理业务；两个前台进程各自用 Ctrl+C 正常停止。

浏览器预期周期在 **build 时**固定，必须与 C++ YAML 的 `sample_period` 配对。
100 ms 对应 1000 ms 速率窗口、1500 ms 停滞阈值；2000 ms 对应 4000/6000 ms。
两组均每 500 ms 检查状态。修改 `npm start` 的 `NEXT_PUBLIC_` 环境变量不会更新已构建页面。
显式非法周期显示配置错误，不静默使用默认值。

完整监控测试默认读取 CMake 生成的 `build/debug/tests/runtime-paths.json`；测试专用 `QBOOK_TEST_BUILD_DIR` 可显式选择另一构建目录（相对仓库根），只使用其自身清单，缺失或引用旧产物明确失败，使用真实发布器、
服务、Next 生产进程和 Chromium。先完成上述完整 C++ 构建，再在 `ui/` 执行：

```bash
npx playwright install chromium
NEXT_PUBLIC_METRICS_SAMPLE_PERIOD_MS=100 npm run build
QBOOK_E2E_SAMPLE_PERIOD_MS=100 npm run test:e2e
NEXT_PUBLIC_METRICS_SAMPLE_PERIOD_MS=2000 npm run build
QBOOK_E2E_SAMPLE_PERIOD_MS=2000 npm run test:e2e
NEXT_PUBLIC_METRICS_SAMPLE_PERIOD_MS=100 npm run build
```

浏览器还需环境提供其系统共享库；安装与实际版本记录在本地 V4 验收中。
测试变量仅供编排，不增加生产 C++ 环境覆盖。编排器不替用户重新构建 UI。

页面准确显示全部 64 位累计值，13 项累计量先以 BigInt 差分，3 项状态量下降不视为重启。
速率按实际覆盖时间估计，首样本只建基线；趋势按真实 60 秒裁剪，最多 2000 个绘图点，断档不连线。
connecting、live、stalled、offline 分别表示无首样本、持续收样本、样本过期和 SSE 错误；
业务计数不变仍可 live，定时器不制造零值点。没有样本不能证明核心服务停止。
订单/撤单合并统计；L2 本地挂单数为 0，不代表真实委托数；内存是估计；Writer Add 不等于落盘。
抽样 Apply 均值来自独立原子字段的近期观察，不是 P99、端到端延迟或跨字段事务快照。

## 公共接口与并发约定

- [PipelineTypes.h](include/PipelineTypes.h) 直接复用 qlib 事件和队列，并冻结全部协议字段类型、大小和偏移。variant、本地指针及原子对象不能直接作为通用网络格式。
- [Protocol.h](include/Protocol.h) 提供 `DecodeFrame`、`ValidateEvent` 和明确的拒绝结果；按空帧、标签、长度、模式、字段顺序校验，独立样本见 [协议 fixtures](tests/fixtures/protocol/README.md)。
- [FeedThread.h](include/Threads/FeedThread.h) 在 `Run` 线程持有 SUB/context；区分收到消息、有效解码和成功入队，只有成功入队表示接纳，multipart 整体拒绝。
- [BookThread.h](include/Threads/BookThread.h) 独占多品种订单簿；应用或业务忽略后均转交原事件，按单调时钟输出已知品种快照。
- [ParquetWriter.h](include/ParquetWriter.h) 同步保存五类记录；调用者必须显式 `Close()`，失败后不可追加或重试批次，析构只释放资源。
- [WriterThread.h](include/Threads/WriterThread.h) 在所属线程取得独占目录、消费记录并关闭文件；成功 Add 后才更新 Writer 指标。
- [Service.cpp](src/Service.cpp) 实现 `RunService` 的线程装配、信号与停止协调；`Runtime.cpp` 保持轻量故障控制，不依赖 Arrow/ZMQ。
- [Config.h](include/Config.h) 提供默认参数、`LoadConfig` 和 `ValidateConfig`，公共头不传播 YAML 类型。
- [Orderbook.h](include/Orderbook.h) 提供 `OnOrder`、`OnTrade`、`OnCancel`、`OnLevel`、`OnSnapshot`、`size` 和 `MemoryBytes`，构造时绑定品种和 `BookMode::Mbo` 或 `BookMode::L2`。
- [Runtime.h](include/Runtime.h) 的 `RunControl` 保留第一个原子交换获胜者的故障。运行线程只读 `Failed()`；所有潜在报告者 join 后才能读 `Error()` 和 `Stage()`。
- [MetricsThread.h](include/Threads/MetricsThread.h) 提供基础指标与真实发布阶段。基础实现位于 [Metrics.cpp](src/Metrics.cpp)，只编入 `qbook_support`；网络实现属于 `qbook_metrics`。每个指标仅有一个写者，快照逐字段读取，不保证整组来自同一瞬间。Metrics 的 PUB/context 在 Run 线程拥有，非阻塞发送冻结的 136 字节普通值，不发送原子对象。

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


## V5实验与本轮复验

实验实现与命令见[benchmarks/README.md](benchmarks/README.md)，参数和实际初态见[实验设计](docs/benchmarks/V5-experiment.md)，全部成绩、无效轮诊断和FR/NFR/UC/AC证据映射见[结果报告](docs/benchmarks/V5-results.md)。Release打开BENCHMARKS后新增34项`benchmark::`正确性检查，共201项；没有机器速度合格线。核心/协议实验不依赖Arrow/ZMQ/Catch2，Writer需要PERSISTENCE，Pipeline需要SERVICE；BUILD_TESTING=OFF仍可构建所选实验。

本轮最终在新建`build/v5-final`完成201/201、四程序53项smoke，并用`QBOOK_TEST_BUILD_DIR=build/v5-final`通过100ms真实Chromium3/3。此前100/2000ms各自生产build的两组各3/3也已执行，默认配置恢复100ms。该测试变量只选择测试产物，不覆盖生产C++配置。三个无测试隔离配置（纯核心、持久化无网络、完整服务）均实际构建并检查依赖。

原始CSV、manifest、冻结参数、全部失败轮及文件校验摘要保留在本地`results/benchmarks/V5-20261027/`；`docs/releases/V5.md`为本地完整命令记录。系统Arrow/Parquet继续使用`/usr`26.0.0-1，Node24.22.1/npm11.19.1已固定安装在容器`/opt`并提供`/usr/local/bin`命令，UI锁文件未升级；仓库尚无可完全复现容器的Dockerfile/digest。按版本计划清理本轮新建build、UI生成目录和独立浏览器缓存后，使用上述命令重新构建，不能调用已清理的旧程序。
