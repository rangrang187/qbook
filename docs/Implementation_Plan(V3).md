# qbook Implementation Plan（V3 实现计划）

文档版本：0.2｜日期：2026-10-23｜对应版本：V3 持久化与核心服务

状态：**V3-01～04 已完成，完整 Debug 154/154、关闭链路的核心检查 75/75、Release 文件与服务检查 39/39 通过。** 关闭测试的生产构建及错误开关组合也已核对；本地版本记录和本轮新建产物清理完成。V0～V2 历史验收保留，本轮实施前另执行前置基线 115/115，通过后开始 V3。

本文保留七章实施方案，第三章按文件写入、Writer 阶段、正式运行和测试工具展开。接口、伪代码与验收要求继续作为维护依据；实际执行结果见第 5.5 节及本地 `docs/releases/V3.md`。示例命令本身不构成运行证据。

## 1. Implementation Overview（实现概述）

### 1.1 本次实现目标与范围

V3 让正常结束的 qbook 运行留下能读回的数据。V2 已实现 Feed 接收、Book 建簿以及测试消费者；本版把真实 Writer 接到 RecordQueue 后面，分别写订单、成交、撤单、L2 更新和快照文件，并由正式程序管理三个工作线程的启动、停止和错误退出。

| 任务 | 要实现什么 | 本版交付与证据 |
|---|---|---|
| V3-01 五类 Parquet 写入与显式关闭 | 按固定字段组成列，逐类积累批次，正常 Close 保存尾批次。 | ParquetWriter、五个 Sink、明确状态；读回准确 schema、整数值、顺序和元数据。 |
| V3-02 Writer 阶段与独占输出目录 | 在一个线程内持续消费，并为每次运行取得新目录。 | WriterThread；目录与文件不覆盖、真实追加指标、正常排空和错误清理。 |
| V3-03 正式入口与运行生命周期 | 让 `qbook` 读取配置并运行真实三阶段服务。 | main/RunService、SIGINT/SIGTERM、异常边界、部分启动失败处理和正确退出码。 |
| V3-04 文件与全链路验收 | 核对筛选、业务、原始记录、快照和文件关闭共同成立。 | 独立读回、网络场景、精确接纳账本、子进程信号测试和失败证据。 |

本版不实现 MetricsThread 网络发布、浏览器 UI 或性能实验框架；阶段内部已有的 16 个指标继续维护，Writer 的四项开始记录真实 Add 成功次数。不新增撮合、交易指令、历史补发、崩溃恢复、按品种分文件或跨五个文件的全局回放序号。

**本版必须分清四个事实。** Feed 成功把有效事件放进 FeedQueue，才算已接纳；Writer 的 Add 返回成功，说明记录已追加到文件组件，可能仍在列缓冲；所有文件显式 Close 成功并在测试中读回，才形成正常完成证据；以上均不承诺断电后的持久性。故障运行即使留下部分可读文件，也不能标为完整采集。

### 1.2 参考的 Requirement 与 Spec

| 依据 | 本版采用的约定 | 验证落点 |
|---|---|---|
| `AGENTS.md` | 中文注释、英文标识符；资源所有权、阶段分工、改动与交付规则。 | 实施前读实际版本，保留用户已有改动；本版落地后更新真实路径和构建命令。 |
| `requirement.md` FR-01、FR-02 | 配置校验及正常/失败运行。 | 非法配置在线程前失败；真实进程信号、退出码和清理。 |
| FR-03～FR-10；UC-02/03 | Feed 筛选和 Mbo/L2 业务、周期快照。 | 回归 V1/V2；本版补核对原事件与快照文件。 |
| FR-11、FR-12；AC-09～AC-12 | 原始记录、五类 Parquet、尾批次、运行隔离和写入失败。 | 文件字段、同类型顺序、不覆盖、Close 与错误状态。 |
| FR-13；AC-13 的 Writer 部分 | 追加计数的真实含义。 | orders 包含 cancel；成功 Add 后计数，不能称为 fsync 次数。 |
| BR-12/13/15；NFR-02/03/06/07/08 | 业务忽略仍记录、数据格式、停止边界和受控使用。 | 精确接纳对照、错误传播、资源寿命和格式兼容。 |
| `specs.md` 4.4.2.1～4.4.3 | WriterThread/ParquetWriter 接口、Sink、状态及五个 schema。 | 第 3.1、3.2 节逐项实现。 |
| `specs.md` 4.1、5.1～5.4、6.1～6.3 | RunControl、正式服务、信号、线程与依赖组织。 | 第 3.3、3.4、5 节。 |
| `Sprint_Backlog.md` 3.4；V0–V2 实现计划 | 四个任务及其前置能力和范围。 | 保留既有回归；完成状态以实际实现和验证为依据。 |

需求与规格使用实际文件名 `requirement.md`、`specs.md`；AGENTS、README 和版本记录按实际工程结构维护。

### 1.3 现有代码与可复用组件

本计划核对了参考 nqbook 的 ParquetWriter、WriterThread、main 与构建组织。参考代码提供了按类分文件、Arrow builders 和分阶段停止的起点；qbook 需要按照既定规格实现异常传播、独占创建、显式关闭和完整验证，不能只替换项目名。

| 来源 | 复用内容 | V3 的具体处理 |
|---|---|---|
| V0 | Config、PipelineTypes、Metric/Metrics、RunControl、CMake 和测试基础。 | 不复制另一套失败状态；Runtime.cpp 保留为轻量故障控制实现。 |
| V1 | Orderbook、固定业务输入与独立期望。 | 文件层接收原记录，不重新判断交易规则；全部业务回归保留。 |
| V2 | Protocol、FeedThread、BookThread、有限发布端、接纳账本思路。 | 把 RecordQueue 的消费者换为真实 Writer；原有 Collector 仍用于 V2 测试。 |
| qlib | 五种记录、record 变体、SPSC、精度与快照深度。 | 保持输入 const；保存业务字段，排除 prev/next；两条队列各一个生产者、一个消费者。 |
| nqbook 文件实现 | 五个具名 Sink、Open*/Append*、Reserve/Commit/Flush。 | 去掉 abort 式报错及依靠析构保存的路径；增加 Open/Closed/Failed 和首错保护。 |
| nqbook 运行实现 | Writer→Book→Feed 启动、Feed→Book→Writer 正常停止。 | 为每个线程加异常边界，处理启动一半失败；V3 不创建尚未实现的 MetricsThread。 |

实现中先检查真实 V0–V2 API 与通过证据，尤其是 Failed 载荷必须 join 后读、满队列重试可被故障取消、先观察停止再确认队列为空这三项约定。它们关系到文件关闭前是否真的没有剩余生产者。

### 1.4 开发环境、依赖与工具链

沿用 Linux / WSL2、x86-64、小端、8 字节指针、C++23、CMake 3.28 或以上、Ninja、Catch2 v3，以及 V0–V2 已固定的 qlib/fkYAML/spdlog/cppzmq/libzmq。Arrow/Parquet 使用 Apache 官方 Ubuntu 24.04 amd64 的 26.0.0-1 二进制包，提取到 `.deps/arrow-26.0.0/usr`，不修改系统包数据库。

`cmake/ArrowDependency.cmake` 通过 `find_package(Arrow 26.0.0 EXACT CONFIG REQUIRED)` 和 Parquet 的等价调用查找同一固定安装，链接真实 `Arrow::arrow_shared`、`Parquet::parquet_shared`。ZSTD 已经探针及文件读回确认可用。37 个固定开发和运行包的 URL、版本、SHA-256、大小及许可证路径记录在 `cmake/arrow-dependencies.lock.json`；`scripts/install_arrow_dependencies.py` 校验全部归档后提取，支持缓存齐全时 `--offline`。头文件与运行时库必须相容；Python 的 pyarrow 不能代替 C++ 包。

配置阶段只在启用持久化时查找 Arrow/Parquet。核心和纯协议验证保持独立；文件读取器使用同一 C++ 依赖。安装脚本需要 Python 3.11 以上和 dpkg-deb，CMake 配置和构建不联网。真实库为 `libarrow.so.2600.0.0` 与 `libparquet.so.2600.0.0`；本地 ELF RPATH 解析间接依赖，移动仓库后须重新配置并链接。固定安装仍依赖主机基础运行库，锁文件不是操作系统镜像。依赖来源、包路径和验证结果见 README 与本地 V3 记录。

官方 API 核对入口：[Arrow CMake 接入](https://arrow.apache.org/docs/cpp/build_system.html)、[Parquet 文件读写接口](https://arrow.apache.org/docs/cpp/api/formats.html)、[Arrow I/O 与文件描述符所有权](https://arrow.apache.org/docs/cpp/api/io.html)。这些资料用于解释选择；实施仍以固定版本头文件、实际编译和读回结果确认，不能把在线文档版本当作工程版本锁。

### 1.5 实现约束与完成条件

**格式与保存。** 五类文件固定创建，零行文件也要有可读取 schema。每类自己积累到 batch_rows；没有跨类型事务。任何追加、刷新或关闭失败均禁止继续业务写入和重试未知结果的批次。正常关闭应显式报告结果，析构仅负责不抛异常的资源释放。

**并发与退出。** Writer 的构造不打开文件，Run 的所有路径都在 Writer 线程清空 writer_。主线程不得代替工作线程关闭其文件或 socket。正常停止依次结束 Feed、Book、Writer；任何阶段失败优先切换为故障清理，所有潜在 Fail 调用者 join 后才读取 Stage/Error。

**测试边界。** 观察真实追加后的记录和真实读回结果，不用日志、发送数或多个 relaxed 指标的即时组合冒充保存证明。少量内部测试接入仅负责流故障、成功追加观察和启动时机，公共 API、产品配置及线上协议不增加测试选项；第 3.4 节说明其寿命和限制。

**完成条件。** 四项任务均有实际实现、必要单元与集成测试、Debug/Release 相关检查以及文件读回证据；前置回归未退步；README、配置、构建和版本记录一致。成功记录保留实际命令、数量、结果及必要环境信息，不保存可再生成的完整成功日志。清理本次新建的临时构建和测试产物，保留用户已有目录、源码、测试、固定依赖和必要失败证据。

## 2. Implementation Structure（实现结构）

### 2.1 技术模块与代码文件的对应关系

| 模块 | 计划文件 | 职责 |
|---|---|---|
| 同步文件组件 | `include/ParquetWriter.h`、`src/ParquetWriter.cpp` | 五个 schema、Sink、追加、批次、状态和关闭。 |
| 输出文件适配 | `src/detail/OutputStreams.h/.cpp` | 排他创建文件并把所有权交给 Arrow；定义流工厂类型。 |
| Writer 阶段 | `include/Threads/WriterThread.h`、`src/Threads/WriterThread.cpp` | 取记录、分发、指标、正常排空和线程内释放。 |
| 运行目录 | `src/detail/RunDirectory.h/.cpp` | 候选命名与排他目录创建，允许确定性碰撞测试。 |
| 正式入口 | `src/main.cpp`、`src/Service.cpp`、`include/Runtime.h` | main 读配置；RunService 组装、处理信号并回收阶段。 |
| 私有协作声明 | `src/detail/WriterHooks.h`、`src/detail/ServiceInternal.h` | 窄范围的测试注入和协调声明，不作为可安装公共接口。 |
| 文件/阶段测试 | `tests/parquet_writer_tests.cpp`、`tests/writer_thread_tests.cpp` | 精确读回、批次边界、目录隔离、错误和所属线程。 |
| 服务测试 | `tests/service_lifecycle_tests.cpp`、`tests/service_integration_tests.cpp` | 进程控制、三阶段输出、接纳边界、积压及失败传播。 |
| 测试辅助 | `tests/support/ParquetReadback.*`、`FailingOutputStream.*`、`WriterGoalObserver.h`、`ProcessHarness.*` | 独立读取、可控 I/O 故障、同步完成证据、子进程寿命。 |
| 测试访问/驱动 | `tests/support/ParquetWriterTestAccess.h`、`WriterThreadInspector.h`、`tests/service_test_driver.cpp` | 私有构造与 join 后观察；隔离子进程内运行带受控条件的同一服务实现。 |
| 工程与交付 | `CMakeLists.txt`、`config/config.yaml`、`README.md`、`docs/releases/V3.md` | 真实目标与命令、格式说明、验证及限制。 |

所有路径相对 qbook 仓库根。Service.cpp 承担服务组装；V0 的 Runtime.cpp 只实现 RunControl，qbook_support 保持独立于 Arrow/ZMQ。规格与 AGENTS 的文件表已按此组织同步。

### 2.2 各模块需要实现的类与组件

| 组件 | 自己持有什么 | 对外或内部主要行为 |
|---|---|---|
| ParquetWriter | 五个拥有型 Sink 指针和整体状态。 | 五个 const Add 重载、显式 Close；不可复制。 |
| Sink | schema、同序 builders、输出流、FileWriter、批量与当前行数。 | 仅在所属 ParquetWriter 内部管理一个文件。 |
| WriterThread | 根路径、运行路径、批量、一次运行状态、writer_；借用队列。 | Run 内打开、消费、关闭；成功后统计追加。 |
| ClaimRunDirectory / OpenExclusiveOutput | 局部路径、错误码和短期 fd 所有者。 | 取得全新目录/文件；不截断已有结果。 |
| main / RunService | 配置、两条队列、三个阶段、Metrics、RunControl、线程句柄和完成位。 | 分配寿命、信号转普通停止、故障广播与 join。 |
| 信号守卫 | 原处理器、原掩码和恢复状态。 | 限定主线程处理停止信号，所有工作线程继承屏蔽。 |
| 测试读取/故障/观察组件 | 有限输入、独立期望、受控状态和错误轨迹。 | 读取真实文件或观察实际步骤，不替代被测业务。 |

Parquet 的 schema 就是“每列叫什么、用哪种类型、按什么顺序保存”的表结构。Sink 是这一张表对应的文件与列缓冲集合；一个 Sink 不等于一个工作线程，五个 Sink 都由同一个 Writer 执行者使用。

### 2.3 类与源文件、头文件的对应关系

公共头文件保留 Spec 的接口。ParquetWriter 的 Sink 在 `.cpp` 内定义，使使用方不需要直接管理 Arrow builders；Writer 通过 `PipelineTypes.h` 获取 RecordQueue，不包含整个 BookThread。测试友元声明在所有构建中一致，不能用宏让库与测试看到不同类布局。

| 目标 | 源码与主要依赖 | 开关 |
|---|---|---|
| `qbook_support`、`qbook_core`、`qbook_protocol` | 保持 V0–V2 的真实基础、业务与纯解码实现。 | 与既有基础目标一致。 |
| `qbook_pipeline` | V2 FeedThread、BookThread，cppzmq/libzmq 与基础库。 | `QBOOK_BUILD_PIPELINE=ON`，默认仍 ON。 |
| `qbook_storage`（STATIC） | ParquetWriter.cpp、detail/OutputStreams.cpp；公开 qlib/include，PRIVATE 链接 Arrow/Parquet 及诊断所用 qbook_logging。 | 新增 `QBOOK_BUILD_PERSISTENCE=ON`，默认 OFF。 |
| `qbook_writer`（STATIC） | WriterThread.cpp、detail/RunDirectory.cpp；链接 storage、support、Threads，PRIVATE 链接 qbook_logging，不假定 support 传播日志头路径。 | 持久化开关 ON；本目标不需要 ZMQ。 |
| `qbook_service`（STATIC） | Service.cpp；链接 pipeline、writer、support、Threads 和既有日志目标。 | `QBOOK_BUILD_SERVICE=ON`，默认仍 OFF。 |
| `qbook` | 真实 main.cpp，链接 service。 | 服务开关 ON。 |
| 既有三个测试目标 | `qbook_tests`、`qbook_allocation_tests`、`qbook_pipeline_tests`。 | 保留 V2 条件和测试前缀。 |
| `qbook_storage_tests` | 文件与 Writer 测试，读回/故障流辅助；显式链接读取所需 Arrow/Parquet。 | 测试及持久化 ON；CTest 前缀 `storage::`。 |
| `qbook_service_tests` | 生命周期/完整链路测试，ProcessHarness、有限发布端与所需辅助；链接 service，并显式链接辅助源码所用 Arrow/Parquet/cppzmq。 | 测试及服务 ON；CTest 前缀 `service::`。 |
| `qbook_service_test_driver` | 独立子进程测试入口，链接同一 service 库及所需测试辅助；使用故障流时显式链接 Arrow/Parquet。 | 测试及服务 ON；不是生产命令，不单独当成通过的 Catch2 用例。 |

服务 ON 必须同时显式启用 PIPELINE 与 PERSISTENCE，否则 CMake 给出清晰配置错误，不暗中改用户选项。持久化默认 OFF，使 V0–V2 已有命令继续不要求 Arrow；V3 的完整命令在第 5.2 节显式开启三个能力。benchmark 仍 OFF，提前开启尚未实现的目标仍报错。

只有 BUILD_TESTING=ON 才引入 Catch2、读取器和测试驱动；私有 hook 的空默认路径属于同一份库实现，其非空实现和故障流只由测试提供。每个直接使用日志的实现目标显式链接 qbook_logging；main.cpp 自己调用日志时，qbook 也直接链接它。公共头必须自包含，内部 src 路径仅按需要 PRIVATE 提供给实现及测试目标。已有全局 new 替换仅留在 V1 专用分配测试，不能链接进 Arrow 或网络测试程序。

服务测试通过 `$<TARGET_FILE:qbook>` 和 `$<TARGET_FILE:qbook_service_test_driver>` 获得准确可执行路径，并显式建立构建依赖；fixture 路径同样由 CMake 传递。子进程使用自己的临时工作目录及 config/config.yaml，不改仓库配置，也不依赖 CTest 恰好从哪个目录执行。

### 2.4 模块与类之间的实现依赖

| 对象/资源 | 创建与操作方 | 其他线程允许做什么 | 销毁前提 |
|---|---|---|---|
| FeedQueue | 主线程创建，Feed 写、Book 读。 | 仅请求停止/读专用进度，不并发 peek 队列。 | Feed、Book 已 join。 |
| RecordQueue | 主线程创建，Book 写、Writer 读。 | 不增加偷取记录的观察消费者。 | Book、Writer 已 join。 |
| ParquetWriter、Sink、流与 fd | Writer 的 Run/Open 创建并独占。 | 主线程只在 Writer join 后读运行路径与已关闭文件。 | 在 Writer 线程已释放，writer_ 为空。 |
| Metrics | 主线程持有，各阶段分别单写。 | 可读原子指标，不推导一组值是同时刻快照。 | 全阶段结束。 |
| RunControl | 主线程持有，各包装器可报告首错。 | 运行中查 Failed；所有写者 join 后读 Stage/Error。 | 所有阶段和相关测试线程结束。 |
| 测试 hooks / 观察器 | 场景线程启动前准备，内容冻结。 | 约定的线程执行回调，协调者只读同步信号。 | 最后一个可能调用者 join，随后再销毁。 |

写入器不得查询 Orderbook 以“修正”原事件。比如一笔 qty=10 的新增后来剩余 4，orders.parquet 的该新增行仍保存 10；原撤单 qty=2 也仍是 2，不替换成撤完后的剩余量。

### 2.5 整体编码顺序及其原因

1. 核对 V0–V2 真实能力，固定 Arrow/Parquet 依赖与文件创建语义，登记已有构建目录。
2. 先实现单线程 ParquetWriter 与独立读取器：从零行和一行开始，再做批次、列表、Close 和失败清理。
3. 实现独占运行目录和 WriterThread，让真实 RecordQueue 的输入成为可读文件，验证追加指标和所有权。
4. 实现 main/RunService 和信号守卫，先验证无行情的正常启动/关闭，再加入阶段错误及部分启动失败。
5. 用冻结的 Mbo/L2 场景、明确完成信号和接纳账本接通完整链路；核对文件后完成必要构建分支和回归，再更新实际验收记录。

## 3. Module Implementation（模块实现方案）

以下按四个模块展开实现。每个模块说明目标与依赖、类和重要函数的具体实现、协作流程、算法、并发、错误处理、编码顺序与完成条件。

### 3.1 五类 Parquet 文件与显式关闭

这一模块对应 V3-01。它把一条条已经决定保留的记录变成五个可读的数据文件。Arrow 的 builder 是“按列积累值”的缓冲器，RecordBatch 是各列行数相同的一批数据，Parquet 文件中的 row group 是文件内部的一组行；这三个概念相关，但一次 API 调用不一定对应一个已经写出的 row group。

#### 3.1.1 模块实现概述

1. **实现目标。** 同步接收 order、trade、cancel、level 和 book，保持字段原值与各类型顺序，满批写出、正常 Close 保存尾批；打开、追加、刷新或关闭失败都明确传播。
2. **规格依据。** `specs.md` 4.4.2.2、4.4.3；FR-11、FR-12、BR-12/13/15、AC-09/10/12；Sprint V3-01。接纳边界来自 V2，文件组件不再次判断订单是否存在、是否真正改簿。
3. **类与文件。** `include/ParquetWriter.h` 保留公开重载和不完整 Sink 声明；`src/ParquetWriter.cpp` 定义 Sink、schema、Arrow 检查、批次和资源清理；测试位于 `tests/parquet_writer_tests.cpp`，读取与故障辅助只放在 `tests/support/`。
4. **复用与依赖。** 复用固定 qlib 记录和精度常量，接入实际锁定的 Arrow、Parquet 与 ZSTD 支持。参考 nqbook 的五个 Sink 和 schema/Append 配对结构可以复用思路，其 `abort()`、析构执行业务 Close、未显式保留输出流及路径截断行为不能直接沿用。

#### 3.1.2 Class Implementation（ParquetWriter 与私有 Sink）

##### 3.1.2.1 类的实现目标

ParquetWriter 只处理文件格式与保存结果。WriterThread 传入什么原始字段，它就保存什么字段：例如撤单 qty=2 即使簿上只剩 1，也保存输入的 2；市价单、重复新增或未知撤单已经在上游决定保留时，不能在这里丢掉。它不把整数价格除以倍率，不在保存时修改时间，也不把 order 的 prev/next 地址写入文件。

##### 3.1.2.2 声明与文件组织

```cpp
namespace arrow::io { class OutputStream; }
namespace qbook {
class WriterThread;
namespace testing { struct ParquetWriterTestAccess; }
class ParquetWriter {
 public:
  ParquetWriter(const std::filesystem::path& run_dir,
                std::int64_t batch_rows);
  ~ParquetWriter() noexcept;
  ParquetWriter(const ParquetWriter&) = delete;
  ParquetWriter& operator=(const ParquetWriter&) = delete;
  void Add(const qlib::order& value);
  void Add(const qlib::trade& value);
  void Add(const qlib::cancel& value);
  void Add(const qlib::level& value);
  void Add(const qlib::book& value);
  void Close();
 private:
  ParquetWriter(const std::filesystem::path& run_dir,
                std::int64_t batch_rows,
                const std::function<std::shared_ptr<arrow::io::OutputStream>(
                    const std::filesystem::path&)>& output_factory);
  enum class State { Open, Closed, Failed };
  struct Sink;
  static void Reserve(Sink& sink);
  static void Commit(Sink& sink);
  static void Flush(Sink& sink);
  // OpenOrders/OpenTrades/OpenCancels/OpenLevels/OpenBooks
  // 与 AppendOrder/AppendTrade/AppendCancel/AppendLevel/AppendBook 成对实现。
  std::unique_ptr<Sink> orders_, trades_, cancels_, levels_, books_;
  State state_ = State::Open;
  friend class WriterThread;
  friend struct testing::ParquetWriterTestAccess;
};
}
```

析构定义放到 `.cpp`，此处能看见完整 Sink 类型。头文件不暴露 builders、文件 writer 或输出流给调用者。私有通用 Open、状态检查、Arrow Status/Result 转异常和不抛清理辅助都在实现侧定义；每个失败信息附带文件类型及操作，例如“orders / WriteTable”，而不是只打印一段无法定位的库错误。函数在返回 Status/Result 时逐个检查；只有确认 Result 成功之后才能提取值。

测试需要的内部扩展使用 `src/detail/OutputStreams.h/.cpp` 中的 `OutputStreamFactory`：它是接收 `const filesystem::path&`、返回 `shared_ptr<arrow::io::OutputStream>` 的 `std::function`，该源文件也定义生产排他打开辅助并编译进 qbook_storage。`src/detail/WriterHooks.h` 引用此定义，不重复声明另一套别名。公共头中的私有构造直接写等价 std::function 类型，并前向声明 OutputStream、包含必要标准头；不依赖 src/detail 中不可公开获取的别名，也不把整个 src 加入 PUBLIC include 路径。实现文件使用统一工厂别名，公开构造选择默认排他真实文件工厂；WriterThread 和 ParquetWriterTestAccess 的友元声明在两种构建中保持一致。工厂只在构造/Open过程中使用，无须保存为热路径成员。WriterHooks 的 after_add 回调由 WriterThread 使用，ParquetWriter 不持有它。私有构造从友元直接创建拥有指针，不能误以为 friend 权限会传给 std::make_unique 模板。

##### 3.1.2.3 成员变量的具体实现

| 成员 | 数据、初始化及所有权 | 不变量与释放方式 |
|---|---|---|
| 五个具名 Sink 指针 | 各自独占一个类型的文件；构造时逐个取得。 | 类型与文件一一对应，不按品种创建更多文件；部分构造成功的指针也能独立清理。 |
| state_ | 私有 State；对象完整构造后可用状态为 Open。 | Closed 永远不能重新 Add；任何工作错误进入 Failed，Failed 不恢复为 Open/Closed。 |
| Sink::schema | `shared_ptr<arrow::Schema>`，在对应 Open* 中构造一次。 | 列名、顺序、类型和元数据固定。 |
| Sink::builders | 与 schema 同序的 `vector<shared_ptr<arrow::ArrayBuilder>>`。 | 可用状态下每个顶层 builder 的长度等于 rows；列表子长度等于 rows×D。 |
| Sink::output | `shared_ptr<arrow::io::OutputStream>`。 | 保持底层输出资源，显式关闭并检查；流的寿命覆盖 FileWriter。 |
| Sink::writer | `unique_ptr<parquet::arrow::FileWriter>`。 | 只有所属执行线程调用；正常关闭先完成文件结束信息，再关闭输出流。 |
| Sink::batch_rows / rows | `int64_t`，B>0，rows 初始 0。 | 正常 Add 返回后 0≤rows<B；完整追加后允许临时 rows=B，随后立即 Flush。 |

Sink 在 `.cpp` 提供 `noexcept` 清理析构。原因是第五个文件打开失败时，整个 ParquetWriter 尚未构造成功，其析构函数不会执行，前四个 Sink 仍必须释放资源。这个析构不调用业务 Flush，不写尚未提交的尾批；正常保存必须走公开 Close。

**五类文件的固定映射。** 表内顺序就是 schema 与 Append 的顺序。i64/u32/u8 分别指 Arrow int64/uint32/uint8；不会因为某个样本值很小而换成更窄的类型。

| 文件及追加入口 | 依次保存的列与类型 | 数据来源 |
|---|---|---|
| orders.parquet / AppendOrder | seq:i64、order_id:i64、price:i64、qty:i64、new_qty:i64、event_ns:i64、recv_ns:i64、instrument_id:u32、side:u8、type:u8、action:u8。 | 同名 order 标量；三个枚举使用其固定底层值。prev/next 完全不参与。 |
| trades.parquet / AppendTrade | seq:i64、buy_order_id:i64、sell_order_id:i64、price:i64、qty:i64、event_ns:i64、recv_ns:i64、instrument_id:u32、side:u8。 | 同名 trade 标量；缺失订单 ID 的 0 原样保留。 |
| cancels.parquet / AppendCancel | seq:i64、order_id:i64、qty:i64、event_ns:i64、recv_ns:i64、instrument_id:u32、side:u8。 | 同名 cancel 标量；qty 是输入撤销量。 |
| levels.parquet / AppendLevel | seq:i64、price:i64、qty:i64、event_ns:i64、recv_ns:i64、instrument_id:u32、side:u8。 | 同名 level 标量；删除使用的非正 qty 原样保留。 |
| books.parquet / AppendBook | event_ns:i64、recv_ns:i64、instrument_id:u32、bid_price、bid_qty、ask_price、ask_qty；后四列均为 `fixed_size_list<int64>[D]`，本版 D=1。 | 同名快照数组逐元素复制，同侧同下标价格/数量配对。 |

本版把各列和列表子值定义为非空字段，写入过程中不追加 null；这是对既有值类型的文件格式细化，应写入格式说明和读取测试。未知时间 0、空侧 0/0 都是实际整数，不能改成 null。即使某类没有记录，也创建并正常关闭该类的零行文件。

schema 元数据采用下面的固定字符串键值；`ARROW:schema` 由 Arrow 自己维护，不能覆盖。文件格式版本 1 是 qbook 格式版本，不是 V3 或 Arrow 软件版本。

| 元数据键 | 值或含义 |
|---|---|
| qbook.format_version | `1` |
| qbook.price_scale / qbook.qty_scale | `10000000000` / `100000000` |
| qbook.time_unit | `unix_epoch_ns`；event_ns 是事件时间，recv_ns 是接收时间，0 表示未知。 |
| qbook.snapshot_depth | `1` |
| qbook.side | `0:buy,1:sell` |
| qbook.order_type | `0:limit,1:market` |
| qbook.order_action | `0:add,1:modify` |

##### 3.1.2.4 成员函数的具体实现

###### A. 构造与 Open*：先准备独立文件，再接收业务记录

| 实现维度 | 具体方案 |
|---|---|
| 函数声明与职责 | 公开构造 `(const path& run_dir, int64_t batch_rows)`；各私有 Open* 返回 `unique_ptr<Sink>`，接收该类型完整路径和批量。 |
| 输入、输出与契约 | 目录由调用者排他取得且可写；B>0。构造全部成功后才允许 Add，五个文件都存在且相应资源归本对象。 |
| 核心算法与数据结构 | 五个固定 schema、对应 builders、独立输出流及 FileWriter；不建立运行时插件表，也不创建第二份全事件缓冲。 |
| 具体步骤 | ①校验 B、D 与容量运算；②依次 OpenOrders/Trades/Cancels/Levels/Books；③创建 schema/元数据和匹配 builders；④排他创建文件并取得输出流；⑤建立 ZSTD、store_schema 的 FileWriter，明确关闭列级并行；⑥Reserve；⑦成功指针交给对应成员。 |
| 成员读写 | 每个 Open* 只组装自己的临时 Sink，成功后移动给成员；不读取其他类型的 rows，不改输入目录中的旧文件。 |
| 对其他函数的调用 | 受检文件打开适配、MakeBuilder、FileWriter::Open、Reserve、Status/Result 检查；采用锁定版本提供的实际重载。 |
| 边界与错误处理 | 非正批量、容量不可表示、无 ZSTD 支持、目录不可写或目标文件已存在都失败。不存在的父目录由 WriterThread 的目录流程处理，本组件不偷偷换目录。 |
| 异常安全与状态一致性 | 局部和成员 Sink 都拥有不抛清理；中途失败不产生可使用对象，不保证目录为空；保留原异常，不删除已有文件。 |
| 时间与空间复杂度 | 固定五个文件的启动开销，加 O(B×各列宽度) 的列容量；实际 Arrow 压缩/字典和文件元数据另有空间。 |
| 并发、同步与线程安全 | 构造在 Writer 工作线程中发生；同步单元测试可在自身唯一线程中直接调用，不能另一个线程同时打开同一路径。 |
| 单元测试与验证方法 | 五个零行文件可读；已有哨兵文件不变；第 k 个打开失败释放前面资源；不支持压缩及过大容量明确失败。 |

Linux/WSL 的生产文件适配使用 `open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode)` 取得新文件，再交给 Arrow 的 fd 重载。`exists()` 后再用可截断的路径 Open 不是排他创建；append 模式也不是新运行。文件描述符在移交之前由局部 RAII 管理，移交时严格按锁定 Arrow 版本的成功/失败所有权规则实现，专门核对移交失败不泄漏、不二次 close。此处不改输出文件的命名规则，也不向外增加 fd API。

###### B. 四类标量 Add 与 Append*：完整一行之后才计数

| 实现维度 | 具体方案 |
|---|---|
| 函数声明与职责 | 四个 `void Add(const qlib::order/trade/cancel/level&)` 分别调用对应 `Append*(Sink&, const T&)`，随后 Commit。 |
| 输入、输出与契约 | 当前必须 Open，输入按上游契约有效且不可变；成功表示这一条追加及必要的满批处理成功，不等价于文件已关闭或断电安全。 |
| 核心算法与数据结构 | 使用上表确定的列下标和精确 builder 类型，追加对应标量；不按结构体内存、variant 下标或文件行号推导字段。 |
| 具体步骤 | ①检查状态；②确认该 Sink 有当前行容量；③Append* 按 schema 顺序填写全部字段；④Commit 增加 rows，必要时 Flush；⑤返回。任何工作步骤异常都设 Failed 后原样抛出。 |
| 成员读写 | 仅读输入值，修改对应列缓冲及 rows；发生工作错误修改 state_；不触碰另一个 Sink 的业务缓冲。 |
| 对其他函数的调用 | 类型正确的 Int64/UInt32/UInt8 builder UnsafeAppend、枚举底层值转换、Commit；容量来自已成功的 Reserve。 |
| 边界与错误处理 | 删除修改的非正 new_qty、level 删除量、未知 ID、市价和业务忽略记录照存；保存组件不添加另一套协议或业务筛选。Closed/Failed 调用直接拒绝，Closed 仍保持 Closed。 |
| 异常安全与状态一致性 | 不尝试把已经追加的半行逐列撤回；若库/实现异常发生，整个对象 Failed，不再读这些列作为完整批次。输入始终不改。 |
| 时间与空间复杂度 | 未满批的标量追加为固定字段数工作；满批时包含 Flush 的转换、压缩和 I/O，不能将 Add 一概称作 O(1) 延迟。 |
| 并发、同步与线程安全 | 所有列只有调用线程写入，没有锁，也不能被测试协调者并发读取；没有额外原子字段。 |
| 单元测试与验证方法 | 四类分别独立固定输入，跨批读回全部字段和顺序；输入前后逐字段相等；故意使满批写入失败，Add 抛且后续 Add 被拒绝。 |

order 的 11 列、trade 的 9 列、cancel/level 的 7 列各自有明确 Append*，不能用“共同前缀”跨类型错写 qty 或时间位置。builder 下转型必须与创建它的 schema 对应；在构造验证类型/列数，测试独立检查映射，不能将 unchecked cast 当作运行时类型识别。

###### C. Add(book) 与 AppendBook：一条快照就是四组完整列表

| 实现维度 | 具体方案 |
|---|---|
| 函数声明与职责 | `void Add(const qlib::book&)` 调用 `AppendBook(Sink&, const qlib::book&)` 后 Commit。 |
| 输入、输出与契约 | Open；输入数组长度来自同一 qlib::snapshot_depth，本版1。返回后保存完整快照行，不保存任何借用指针。 |
| 核心算法与数据结构 | 三个标量 builder 加四个 FixedSizeListBuilder；每个列表拥有 Int64 子 builder。 |
| 具体步骤 | ①状态检查；②追加 event_ns/recv_ns/instrument_id；③依次为四个列表 Append 一个有效槽；④每个槽向子 builder 追加 D 个对应值；⑤所有列完成后 Commit。 |
| 成员读写 | 读取快照副本；写 books_ 的顶层有效位、四个子数组、rows；失败写 state_。 |
| 对其他函数的调用 | FixedSizeListBuilder::Append 的 Status 必须检查；已 Reserve 的 Int64 子列使用 UnsafeAppend；最后调用 Commit。 |
| 边界与错误处理 | 空簿和单边空簿仍是四个有效列表，每项0/0；不追加 null 列表，不把长度1退化为标量列。 |
| 异常安全与状态一致性 | 第三个列表失败时也不能 Commit 前两个；部分列改变后整体 Failed，不能重试相同快照补齐。 |
| 时间与空间复杂度 | 每条追加 O(D)，D=1；容量 O(B×D)，满批另计 Flush。 |
| 并发、同步与线程安全 | 只接收值快照，不向 Book 借用活容器；全部 builder 仍是单线程。 |
| 单元测试与验证方法 | 双边、单边、空簿；四列恢复为长度1 fixed_size_list<int64>，元素、列表有效性和时间均独立核对。 |

###### D. Reserve：为下一批准备顶层与子列容量

| 实现维度 | 具体方案 |
|---|---|
| 函数声明与职责 | `static void Reserve(Sink&)`，供新建 Sink 和成功 Flush 后准备下一批。 |
| 输入、输出与契约 | rows=0，B>0，各 builder 已创建；返回后每个顶层列至少可放 B 个值，列表子列至少可放 B×D 个值。 |
| 核心算法与数据结构 | 遍历固定数量 builders，分别 Reserve 顶层与列表的子值容量，不预填虚假记录。 |
| 具体步骤 | ①在乘法前检查 B≤INT64_MAX/D；②按 V0 的可表示容量规则检查字节计算；③Reserve(B)；④列表 value_builder Reserve(B×D)；⑤逐个检查 Status。 |
| 成员读写 | 改变容量，不改变有效行数，不增加 rows，也不改变文件中的内容。 |
| 对其他函数的调用 | builder::Reserve、FixedSizeListBuilder::value_builder、受检整数转换和统一 Arrow 检查。 |
| 边界与错误处理 | 大批量不可表示直接失败；可表示但实际分配失败也正常传播，不能假定启动校验代表机器内存足够。 |
| 异常安全与状态一致性 | 多列可能已有部分新容量，但失败后不开始业务追加；构造失败释放 Sink，运行中失败经 Add/Close 进入 Failed。 |
| 时间与空间复杂度 | 预留成本随列容量增长；不是每条行情都预留整批；Arrow实际分配策略须以固定依赖为准。 |
| 并发、同步与线程安全 | 只由同一文件拥有线程调用，不与 Append/Finish 并发。 |
| 单元测试与验证方法 | B=1、小批量和过大边界；列表子列长度/容量及跨批读取正确，不为失败测试耗尽整机内存。 |

###### E. Commit：完成一行，必要时写出该类型的一批

| 实现维度 | 具体方案 |
|---|---|
| 函数声明与职责 | `static void Commit(Sink&)`，只在 Append* 全部完成后调用一次。 |
| 输入、输出与契约 | 调用前旧 rows<B，所有列刚追加一行；返回后0≤rows<B，满批已交给写入路径。 |
| 核心算法与数据结构 | 单个受约束计数增加；达到 B 调用 Flush，不访问其他类型。 |
| 具体步骤 | ①检查 rows<B；②增加1；③若等于B则Flush，否则返回。 |
| 成员读写 | 读 B，写 rows；Flush 会进一步改变缓冲与文件；不增加 Writer 指标。 |
| 对其他函数的调用 | Flush；在调试/定向验证中检查列长，但不在每行遍历整个历史数据。 |
| 边界与错误处理 | B=1 每条都触发；rows+1≤B 已保证整数可表示。Commit 被重复调用属于内部实现错误。 |
| 异常安全与状态一致性 | Flush 抛出时不退回 rows 再尝试；外层设置 Failed，已写前缀无法回滚。 |
| 时间与空间复杂度 | 未满批O(1)；满批成本继承Flush；无额外事件列表。 |
| 并发、同步与线程安全 | 同一线程独占 rows，不能拿它做对外的磁盘进度信号。 |
| 单元测试与验证方法 | 0、B−1、B、B+1、2B+1 行，Close 后逐行检查；不同类型独立触发，不把总记录数当每类批量。 |

###### F. Flush：完成列并执行一次实际文件写入

| 实现维度 | 具体方案 |
|---|---|
| 函数声明与职责 | `static void Flush(Sink&)`，把非空列缓冲组成一批并交给既有 FileWriter。 |
| 输入、输出与契约 | 仅在可写路径调用；各列完整且同长，0≤rows≤B。空批立即返回；非空成功后 rows=0，下一批容量可用。 |
| 核心算法与数据结构 | 固定长度列数组 vector、RecordBatch，再用 Table::FromRecordBatches 包装这一批，调用同一个 `FileWriter::WriteTable(table, batch_row_count)`。不另建文件 writer。 |
| 具体步骤 | ①保存本批行数n；②逐列Finish并检查；③用schema/n/arrays建立RecordBatch并验证形状；④转换为Table并检查；⑤调用WriteTable并检查；⑥成功后rows归0；⑦Reserve下一批。 |
| 成员读写 | Finish移走并重置列缓冲；writer推进文件状态；rows仅在写入成功后归0，最后准备新容量。 |
| 对其他函数的调用 | ArrayBuilder::Finish、RecordBatch::Make及验证、Table::FromRecordBatches、FileWriter::WriteTable、Reserve；检查每项Status/Result。 |
| 边界与错误处理 | 第k列Finish失败、Table创建失败、文件只写了前缀、写成功后下一批Reserve失败，都使调用失败；不能据此重放本批。 |
| 异常安全与状态一致性 | Finish已经成功的列无法靠改rows恢复；任何中途错误都让对象Failed。写成功但Reserve失败时，文件可能已有本条而Add未成功返回；指标仍按成功返回定义，不承诺错误路径账目事务一致。 |
| 时间与空间复杂度 | 与n、列数、D及压缩/I/O有关；数组/表包装通常共享已完成缓冲，不能宣称库内部零复制或固定内存。 |
| 并发、同步与线程安全 | 同一拥有线程顺序调用；不在独立线程异步Flush，不让Close与本函数并发。 |
| 单元测试与验证方法 | 跨批文件逐行读回；实际注入满批Write错误后验证Failed与无重试；Finish/分配错误的检查和catch范围作代码审阅，不能用流错误冒充其实际触发证据。 |

本方案特意不把参考实现的 `WriteRecordBatch()` 等同于“本批已经从其内部缓冲写到输出流”。Arrow 允许多个 RecordBatch 留在同一缓冲 row group，直到切换组或关闭才刷出；这里改用同一 FileWriter 的 WriteTable 完成每个应用批次。物理 row group 的划分还受固定库的限制影响，验收以真实数据与调用边界为准，不擅自要求每批必有且只有一个物理 row group。该写入成功仍不是 fsync 或断电安全声明。

###### G. Close：保存尾批，所有关闭成功后才承认完成

| 实现维度 | 具体方案 |
|---|---|
| 函数声明与职责 | `void Close()`，完成正常保存；成功后重复调用无操作。 |
| 输入、输出与契约 | Open可执行正常关闭；Closed直接返回；Failed只能清理并明确拒绝“正常关闭成功”。 |
| 核心算法与数据结构 | 按固定Sink顺序逐个Flush尾批、writer.Close、output.Close；局部exception_ptr保留本次最先异常，后续清理不覆盖。 |
| 具体步骤 | ①检查state；②Open时逐个正常关闭；③某一步失败立即设Failed并保存当前异常；④对全部剩余资源做不抛清理，不再Flush任何业务缓冲；⑤有首错则重抛，否则确认全部完成并设Closed。 |
| 成员读写 | 正常清空各Sink剩余rows并释放writer/output；最后写state；没有将已关闭文件再打开的分支。 |
| 对其他函数的调用 | Flush、FileWriter::Close、OutputStream::Close、清理辅助、exception_ptr重抛；输出流已关闭时不要求重复底层Close。 |
| 边界与错误处理 | 零行也要完成文件元数据；尾批不足B仍写；第一个文件关闭失败后其余句柄仍必须释放。Failed入口不重试Flush，清理后抛明确logic_error，保持Failed。 |
| 异常安全与状态一致性 | 正常Close不承诺跨五文件事务；前几个已关闭、后几个失败时整次仍失败。原始关闭异常是报告依据，清理异常只能作为补充诊断且不能再抛出。 |
| 时间与空间复杂度 | 最多处理五个尾批，加库的文件结束信息和关闭成本；调用可能阻塞，不承诺固定截止时限。 |
| 并发、同步与线程安全 | 只由Writer线程或同步测试的唯一拥有者调用；主线程join之前不能替它Close。 |
| 单元测试与验证方法 | 空文件、尾批、完整批、重复Close；关闭后Add拒绝；两个关闭错误同时触发仍报告第一条且资源释放。 |

###### H. 析构与 Sink 清理：释放资源，不补写业务尾批

| 实现维度 | 具体方案 |
|---|---|
| 函数声明与职责 | `~ParquetWriter() noexcept`、私有 `Sink::~Sink() noexcept` 与统一不抛清理辅助。 |
| 输入、输出与契约 | 无并发使用；可能从未完整构造、仍Open、已Closed或Failed。只保证释放能释放的资源，不保证文件完整。 |
| 核心算法与数据结构 | 对仍有资源的Sink先尝试Abort输出流，再释放可能带内部关闭动作的FileWriter，最后释放流与builders。Abort成功后流已不可写；不能假定库析构内部不会再尝试Close。 |
| 具体步骤 | ①不调用业务Flush；②未关闭output尽力Abort并检查/捕获结果；③reset writer；④reset output；⑤释放列缓冲；每步均不得让清理异常逃逸。 |
| 成员读写 | 清空拥有指针；不增加rows、指标或任何成功状态。 |
| 对其他函数的调用 | OutputStream::Abort、智能指针reset；使用锁定版本的实际清理契约，不能依赖库析构把尾批保存成功。 |
| 边界与错误处理 | 未完成显式Close即析构时，尾批允许丢失；故障Sink禁止再次写入，其他文件也只做清理。清理诊断失败不能触发第二次异常。 |
| 异常安全与状态一致性 | 构造展开和Add/Close异常展开均不得由qbook清理再次抛错；失败对象不会通过析构变成Closed。Abort成功后，库析构再尝试文件结束写入也只能面对已关闭的流；Abort本身失败须保留清理失败信息，不能声称已强制消除全部底层I/O。核对固定库析构的异常契约，应用层绝不再次调用Flush或重放记录。 |
| 时间与空间复杂度 | 释放五个固定Sink及其缓冲；底层句柄释放不作硬实时承诺。 |
| 并发、同步与线程安全 | WriterThread在自己的Run清理守卫内reset本对象；主线程只销毁已经空的WriterThread成员。 |
| 单元测试与验证方法 | 在追加/关闭失败后记录清理线程ID和关闭次数；析构期间无异常越界、不重放失败批；部分构造失败也不泄漏已打开资源。 |

##### 3.1.2.5 构造、初始化与资源释放

初始化按“类型schema→列builders→排他输出→文件writer→Reserve”进行，各阶段取得资源后立刻交给拥有型对象。普通保存按“Flush→文件writer Close→输出流 Close→释放”进行；异常清理则不进入业务Flush。不能仅把默认析构顺序排好就认为错误已报告：正常Close必须检查实际返回值，Failed清理必须保留已存在错误。

##### 3.1.2.6 类内部的关键实现流程

取B=2，连续追加3条订单和1条撤单。前两条订单组成一次订单批次，第三条仍在orders列缓冲，撤单在自己的cancels缓冲；Close分别保存1条订单尾批和1条撤单尾批。其他三类仍完成零行文件的关闭。最终订单文件3行、撤单文件1行，不能用“总共4条、每2条一批”把两种schema混在一起。

若第二条订单触发Flush时发生写入错误，当前Add抛异常，WriterThread不增加这一次成功追加计数，之前已经增加的计数也不倒扣。文件可能有部分字节，整个运行失败；没有“再调用一次Add试试”的恢复分支。

##### 3.1.2.7 类的实现顺序与依赖关系

先固定schema/元数据与独立读取预期，再实现排他文件及部分构造清理；随后实现单类型Append/Reserve/Commit/Flush，扩展剩余四类；最后实现状态约束、显式Close及错误清理。错误测试随打开、写批和关闭各步骤建立，不能等正常流程完成后再用析构掩盖关闭遗漏。

#### 3.1.3 模块内部协作与调用流程

WriterThread从队列得到独立record值，按实际变体调用一次Add。ParquetWriter选择一个Sink，按该类型schema追加并必要时Flush；正常停止时WriterThread显式Close。成功返回以后WriterThread才能更新对应追加计数，文件读取验收则等整个Writer线程结束。这个分工不需要文件组件持有队列、Metrics或RunControl。

#### 3.1.4 模块关键算法的实现方案

**格式与追加同时核对。** schema与Append*使用同一固定字段说明，但测试预期独立逐列书写；不能由生产schema自动生成测试期望后宣称字段正确。读取器遍历所有chunk/row group后核对值，不能只看第一块数据或文件总行数。schema中的uint8/uint32检查Arrow逻辑类型，不能把Parquet底层可能采用的INT32误判为项目类型变化。

**没有跨文件全序。** 每个Sink保留到达本类型的顺序；不同文件行号不构成关联键。seq来源仍是上游消息，没有新增跨类型统一序列号，book也没有虚构seq。真正全序回放不属于当前持久化交付。

**批次内存。** 五类各保留最多一批的应用层列缓冲，但压缩、字典、已完成数组及文件元数据可能增加峰值；不能把B×列宽度称作全进程严格上限。V3先验证有界应用缓冲与正确性，V5再依据真实负载测量峰值和成本。

#### 3.1.5 模块并发与同步实现

本类不创建线程、不暴露可并发读取的业务状态；Arrow列写入关闭并行选项，保持文件操作与测试观测的线程归属明确。独立文件之间仍在同一Writer线程顺序操作。若外部发生Fatal，WriterThread结束Run并在本线程销毁本对象；当前I/O调用可能先返回，不能从其他线程关闭其fd强行打断，也不能因此承诺硬实时退出。

#### 3.1.6 模块错误处理与恢复方案

正常错误传播链是“Arrow Status/Result或系统错误→有操作上下文的C++异常→Add/Close置Failed→Writer线程包装器Fail→主线程join后报告”。构造失败没有可继续使用的对象；运行中Failed以后只能清理，重新采集应启动新对象、新运行目录，不能续写上次失败目录。

测试使用真实Parquet writer和可控输出流错误，不用FakeWriter替代它。公开接口保持不变，具体测试访问与输出流工厂在第3.4节统一；工厂仅在冷启动打开Sink时使用，生产始终选择排他真实文件，测试包装真实流以触发Open/Write/Close故障并记录所属线程。工厂与故障状态不做全局可变设置，不允许主线程在运行中替换已经使用的流；不向配置文件增加故障开关。可控流把Abort与指定的Close失败分开，确保故障测试仍能释放真实句柄；清理时库可能发出的失败写调用不能被误报为应用层再次Flush。

本版必须实际注入的是打开、写批和关闭边界。Reserve/Finish 的错误返回也必须被检查并进入相同 Failed 清理，但流包装器不能使内存分配失败；容量边界测试与代码审阅分别记录。除非实现时发现具体未解决风险，不为每个内部调用再引入一套 MemoryPool 参数和故障框架；未实际触发的内部失败点不能写成故障注入通过。

#### 3.1.7 模块实现顺序

1. 核对目标环境Arrow/Parquet及ZSTD实际可用性、导出目标和文件API；登记固定版本，不以在线文档版本冒充本机版本。
2. 实现固定schema、元数据与只读测试辅助，确定独立字段期望。
3. 实现排他输出和Sink清理，先验证五类零行文件与打开中途失败。
4. 实现四种标量与book列表追加，完成B=1及跨批读取。
5. 实现状态机和Close首错处理，验证写失败、关闭失败、部分构造和析构无重试。
6. 将同步文件组件交给V3-02，实际回归通过后才更新任务状态。

#### 3.1.8 模块测试与完成条件

| 检查组 | 必须看到的结果 |
|---|---|
| 五类格式 | 文件名、字段顺序、逻辑类型、元数据、压缩和列表深度正确，prev/next不存在。 |
| 行与批次 | 每类0、B−1、B、B+1、2B+1行读回准确；不同类型计数独立；所有chunk均检查。 |
| 原始语义 | 修改绝对new_qty、撤销量、成交量、L2删除量、未知时间及空快照原样保存。 |
| 正常状态 | 显式Close后全部文件可读；重复Close无操作；Closed对象Add明确拒绝。 |
| 不覆盖 | 既有同名文件保持字节不变；构造拒绝复用，不改为append或truncate。 |
| 失败状态 | 实际注入打开、WriteTable、文件Close和流Close错误，核对Failed、无业务重试、资源清理与首错；Reserve/Finish容量与异常传播另外作边界测试/代码审阅，不冒充已注入内存错误。 |
| 所属线程 | Writer集成中open/write/close/资源释放属于Writer线程，主线程析构不补写。 |

五类文件、边界和故障检查已纳入实际测试。固定 Arrow 26.0.0 源码已核对 fd 接管、Abort、Close 和析构行为；定长列表与元数据经过真实读回。fd 移交内部失败和系统 close 错误仅经源码审阅，未实际注入。API 依据可查 [Arrow Parquet C++接口](https://arrow.apache.org/docs/cpp/api/formats.html)、[Arrow输入输出接口](https://arrow.apache.org/docs/cpp/api/io.html)及[Arrow/Parquet类型往返说明](https://arrow.apache.org/docs/cpp/parquet.html)。

### 3.2 Writer 阶段与独占输出目录

#### 3.2.1 模块实现概述

**1. 模块实现目标。** 对应 V3-02：让 RecordQueue 中的每条记录由一个实际写文件的消费者接走，交给第 3.1 节的 ParquetWriter。普通停止时，Book 已经结束生产，Writer 应处理队列剩余内容、写尾批次并显式关闭；文件错误或其他阶段失败时，应结束等待并把此次运行判为失败。

**2. 对应的 Spec 章节。** 遵循 `specs.md` 4.4.2.1、4.4.3、4.5 的 Writer 指标及 5.1～5.2 的失败与寿命规则，落实 FR-11、FR-12、FR-13 的追加计数部分，BR-12/15、AC-09～AC-12。三阶段统一停止和进程退出由第 3.3 节组织，本类不接管信号处理或线程创建。

**3. 需要实现的类与文件。** `include/Threads/WriterThread.h` 声明阶段类，`src/Threads/WriterThread.cpp` 实现消费。目录命名与排他创建抽成一个窄内部函数，在 `src/detail/RunDirectory.h/.cpp` 声明和实现，便于用固定候选值验证碰撞。`src/detail/WriterHooks.h` 统一声明有限测试接入，并引用 `src/detail/OutputStreams.h` 中唯一的 OutputStreamFactory 定义。新增 `tests/writer_thread_tests.cpp`，复用第 3.4 节的 Parquet 读取与故障辅助；必要的私有观察放在 `tests/support/WriterThreadInspector.h`。

**4. 依赖与复用组件。** 使用 `PipelineTypes.h` 的 RecordQueue、V0 的 Metrics/RunControl、qlib 的五种 record 值，以及真实 ParquetWriter。Writer 头文件不通过 BookThread.h 获取队列，也不依赖 Feed 或 ZMQ。原输入值已经决定应当保留，本层不再检查它是否改变过订单簿。

#### 3.2.2 Class Implementation（WriterThread 类实现）

##### 3.2.2.1 类的实现目标

WriterThread 是一个工作流程：持续取下一条记录、选择对应文件追加方法、遇到普通停止后完成保存。ParquetWriter 才是懂得列名、批次和压缩的文件组件。例如 Writer 取到一条未知订单的合法撤单，仍调用 `Add(cancel)`；撤单文件保存原 qty，writer_orders 增加一次，不查订单是否存在。

本类不拥有操作系统线程。调用方先构造它，再在一个工作线程调用一次 Run，最后 join 后销毁对象。把“对象存在”和“线程已经启动”分开，可以在启动失败时清楚判断哪些资源需要等待。

##### 3.2.2.2 类的声明与文件组织

保持 Spec 的公开接口和六个业务成员；另加一个默认空的私有测试接入指针，生产构造不会配置它。拷贝限制用于落实唯一消费角色。ParquetWriter 与 WriterHooks 可在头文件前向声明，析构在 `.cpp` 中定义，使使用 Writer 声明的文件不必展开 Arrow 头文件。

```cpp
namespace qbook {
namespace testing { struct WriterThreadInspector; }
struct Config;
namespace detail {
struct WriterHooks;
struct ServiceHooks;
int RunServiceImpl(const Config&, const ServiceHooks*);
}
class ParquetWriter;

class WriterThread {
 public:
  WriterThread(RecordQueue& in, std::filesystem::path out_root,
               std::int64_t batch_rows);
  ~WriterThread() noexcept;
  WriterThread(const WriterThread&) = delete;
  WriterThread& operator=(const WriterThread&) = delete;
  void Run(Metrics& metrics, std::stop_token stop,
           const RunControl& control);
 private:
  WriterThread(RecordQueue& in, std::filesystem::path out_root,
               std::int64_t batch_rows, const detail::WriterHooks& hooks);
  void Open();
  void Consume(const qlib::record& record, Metrics& metrics);
  void Close();
  RecordQueue& record_queue_;
  std::filesystem::path out_root_;
  std::filesystem::path run_dir_;
  std::int64_t batch_rows_;
  std::unique_ptr<ParquetWriter> writer_;
  bool started_ = false;
  const detail::WriterHooks* hooks_ = nullptr;
  friend struct testing::WriterThreadInspector;
  friend int detail::RunServiceImpl(const Config&, const detail::ServiceHooks*);
};
}
```

友元声明和私有测试成员在所有构建中保持一致，观察器定义只进入测试目标，不能用测试宏改变 WriterThread 的布局。观察器只在所属线程独占执行时调用私有方法，或在 join 后读取 run_dir_/writer_；它不允许主线程窥视运行中的文件对象。内部 RunServiceImpl 也是明确的友元，因此测试驱动可经同一服务装配选择私有构造，不需要让生产库包含 tests 中的观察器定义。私有构造在友元上下文用直接 new 后立即交给 unique_ptr，不能误以为 std::make_unique 或 optional::emplace 会继承调用者的访问权限。

WriterHooks 使用统一的 `OutputStreamFactory` 和 `std::function<void(const qlib::record&)> after_add`。工厂类型只在 OutputStreams.h 定义，不在 WriterHooks.h 再声明另一份；它只在 Open 冷路径调用，返回真实文件流或包裹真实文件流的故障注入器。after_add 在该记录的 Add 和 Metric::Add 都成功之后调用，参数只在调用期间有效，不保留对循环局部 record 的引用。生产 hooks_ 为 nullptr，不增加用户参数或新指标。测试回调的计数状态由 Writer 独占，在“已收到目标原事件及之后的目标快照”成立时 release 发布完成标志；协调者 acquire 读取该标志，文件仍在 join 后检查。

##### 3.2.2.3 成员变量的具体实现

| 成员 | 初始化、所有权与生命周期 | 必须保持的约束 |
|---|---|---|
| record_queue_ | 构造绑定外部队列引用，不复制和释放队列。 | Run 是唯一消费者；队列活到生产者和 Writer 都 join。 |
| out_root_ | 移动取得配置路径，允许相对仓库运行目录解释。 | 构造只保存，不创建目录；执行期间路径不由其他线程修改。 |
| run_dir_ | 初始为空，Open 排他创建成功后保存本次路径。 | 只代表本次已经取得的目录；异常后仍保留路径用于诊断，不重用或删除。 |
| batch_rows_ | 从已验证配置保存，再检查正值。 | 与 ParquetWriter 批次契约一致；不从队列容量推导批量。 |
| writer_ | 初始 nullptr；仅在 Run 所属线程创建、调用、reset。 | Run 的所有退出路径都清空；主线程析构时不再执行文件关闭。 |
| started_ | 初始 false，首次 Run 开始置 true，之后不复位。 | 阻止同一实例串行重启；不是并发锁，两个线程同时 Run 本就违反契约。 |
| hooks_（私有测试细化） | 生产构造为空，私有测试构造借用固定 WriterHooks。 | hooks 与其回调捕获对象在线程启动前就绪，并活到 Writer join；运行中不替换工厂或函数。 |

不增加第二份 record 容器。取出的当前记录是局部值，列缓冲由 ParquetWriter 持有，尚未取出的记录留在有界队列。业务记录的排队量与列缓冲量应分别解释，不能把两条队列固定容量说成全进程内存固定。

##### 3.2.2.4 成员函数的具体实现

###### A. 构造函数：只绑定资源与配置

| 实现维度 | `WriterThread(RecordQueue&, path, int64_t)` |
|---|---|
| 函数声明与职责 | 保存输入队列、根路径和批量，建立尚未运行的阶段对象。 |
| 输入、输出与契约 | 有效队列引用、输出根路径、正批量；无返回值，不取得队列所有权。 |
| 核心算法与数据结构 | 引用绑定、path 移动、整数校验；writer_ 为空、run_dir_ 为空、started_ 为 false、hooks_ 为空。私有测试构造委托该构造后绑定hooks引用。 |
| 具体步骤 | 检查 batch_rows 大于 0；保存成员；返回可供 Run 使用的对象。目录有效性由真正 Open 时验证。 |
| 成员读写 | 初始化六个业务成员及空测试指针，不读取队列下标、指标或外部文件。 |
| 对其他函数的调用 | 标准 path 的拥有型操作；不调用 Open、ParquetWriter 或线程 API。 |
| 边界与错误处理 | 非正批量抛 invalid_argument；路径保存发生分配错误则构造失败。不能提前证明磁盘可写。 |
| 异常安全与状态一致性 | 构造失败自动释放已经初始化的普通成员，没有已启动线程或打开文件需要补偿。 |
| 时间与空间复杂度 | 与路径保存成本相关，除此之外固定工作和固定成员空间。 |
| 并发、同步与线程安全 | 在线程启动前完成，不与 Run 或销毁并发。 |
| 单元测试与验证方法 | 0/负批量失败；正值构造成功；构造前后临时根目录仍未被创建。 |

###### B. Run：消费、排空和线程内清理

| 实现维度 | `void Run(Metrics&, stop_token, const RunControl&)` |
|---|---|
| 函数声明与职责 | 在当前线程拥有完整写入生命周期，并在返回或抛出前清空 writer_。 |
| 输入、输出与契约 | 共享对象存活，实例只运行一次；普通 stop 由协调者在 Book join 后发出；无返回值，异常交给线程包装器。 |
| 核心算法与数据结构 | 单消费者 try_pop 循环；局部 noexcept 清理守卫；普通停止排空和 Failed 退出分开。 |
| 具体步骤 | 拒绝重复运行，置 started_；在 Open 前安装 reset 守卫；若已经 Failed 则直接结束；Open；循环先检查 Failed，再读取本轮 draining，随后 try_pop；有值只 Consume 一次；draining 为真且这次 pop 为空时再检查 Failed并显式 Close；其他空轮次短暂休眠。 |
| 成员读写 | 本线程读队列、读固定配置、写 started_/run_dir_/writer_；Consume 更新本阶段四项指标。 |
| 对其他函数的调用 | Open、Consume、Close、try_pop、Failed、stop_requested、100us 命名空闲等待，以及退出守卫中的 writer_.reset。 |
| 边界与错误处理 | Fatal 随时终止进一步消费，不等待队列变空；没有数据的正常运行仍打开并关闭五个零行文件；已经 Failed 的早退可以不创建输出。 |
| 异常安全与状态一致性 | Open、Add、指标、Close 或日志异常向外传播；reset 不抛且不重试业务批次，不能覆盖当前异常。已取出但未成功追加的记录在故障时不重新入队。 |
| 时间与空间复杂度 | N 条记录执行 N 次消费，加上批次压缩/I/O；本层只多持有一条当前记录，不复制整个队列。等待不承诺有限业务时延。 |
| 并发、同步与线程安全 | 所有文件调用及释放均在这个线程；主线程只请求停止、读原子指标并 join，不操作 writer_。 |
| 单元测试与验证方法 | 正常尾部排空、空输入、启动前 Fatal、运行中 Fatal、重复 Run、Open/Add/Close 失败；故障夹具记录资源操作线程并在 join 后核对。 |

循环中的先后关系要明确写进代码，而不只写成“停止后排空”：

```cpp
// 伪代码：Open 前已经安装 noexcept reset 守卫。
for (;;) {
  if (control.Failed()) return;
  const bool draining = stop.stop_requested();
  auto record = record_queue_.try_pop();
  if (record) {
    if (control.Failed()) return;
    Consume(*record, metrics);
    continue;
  }
  if (draining) {
    if (control.Failed()) return;
    Close();
    return;
  }
  SleepForIdlePolicy();
}
```

例如 Writer 先观察到空队列，Book 随后放入最后一条并结束，协调者再请求停止。如果 Writer 用旧的空结果加上刚读到的新 stop 就直接返回，会漏掉最后一条。上面先读 stop、后 pop 的顺序要求它在看到正式停止后再确认一次空；停止请求必须发生在 Book join 之后，二者缺一不可。

Failed 与某次 Consume/Close 的开始可能竞争，已经开始的同步库调用不一定可撤销。这里保证在检查点结束继续处理，不承诺故障标志置位的那个瞬间磁盘上绝不再出现一个字节；最终进程结果仍必须是失败。

###### C. Open：先取得目录，再创建真实文件

| 实现维度 | `void Open()` |
|---|---|
| 函数声明与职责 | 为本次运行取得独占目录，并构造五文件 ParquetWriter。 |
| 输入、输出与契约 | 仅首次 Run 的当前线程调用，writer_ 为空；使用 out_root_ 与 batch_rows_，无返回值。 |
| 核心算法与数据结构 | 生成有限候选种子，调用内部 ClaimRunDirectory；成功后构造文件组件，测试时才传入固定输出流工厂。 |
| 具体步骤 | 取得当前时钟的原生 tick 与进程标识形成可打印 seed；用 0 起始的尝试号排他创建目录；保存 run_dir_；生产路径构造 ParquetWriter(run_dir_, batch_rows_)，测试工厂存在时使用其私有构造；成功后记录实际路径及初始化完成。 |
| 成员读写 | 读 out_root_/batch_rows_，设置 run_dir_ 和 writer_；不取队列记录，不改变追加指标。 |
| 对其他函数的调用 | ClaimRunDirectory、目标 Linux 的进程标识读取、标准时钟/字符串、ParquetWriter 构造及启动日志。 |
| 边界与错误处理 | 根路径为普通文件、不可创建、候选重试耗尽、任一输出打开失败均抛错。时钟回拨或进程标识重复不能破坏排他规则。 |
| 异常安全与状态一致性 | 构造文件组件完整成功才把unique_ptr写入成员，不发布半对象；已经创建的目录与可能不完整文件保留，已打开句柄由构造清理在本线程释放。 |
| 时间与空间复杂度 | 最多固定上限的目录尝试，加五个文件的冷启动成本；无逐事件目录操作。 |
| 并发、同步与线程安全 | 不同进程可竞争同一根目录；只有 create_directory 成功者拥有候选目录，不依赖单进程互斥锁。 |
| 单元测试与验证方法 | 两次运行目录不同、第一份文件不变；固定候选冲突会换名；根为普通文件/目录创建故障/构造部分失败明确退出。 |

候选命名只用于减少碰撞，不参与行情时间计算。可选 `run-t<原生tick>-p<pid>-<attempt>`；不必把 tick 乘成纳秒或使用当地时区格式，因此不引入不必要的乘法溢出和时区依赖。使用实际整数类型格式化，尝试号有固定上限，不允许无界递增直到回绕。

ParquetWriter 将 WriterThread 声明为友元以允许测试工厂接入。调用其私有构造时应在 Writer 成员函数中直接构造并立即交给 unique_ptr，不能直接让 make_unique 模板访问私有构造；后者并不会继承调用者的友元权限。生产公共构造继续使用 make_unique。

###### D. Consume：追加成功后才更新对应指标

| 实现维度 | `void Consume(const qlib::record&, Metrics&)` |
|---|---|
| 函数声明与职责 | 对一个不可变记录做穷尽类型分发，调用一次对应 Add 并更新一次追加数。 |
| 输入、输出与契约 | writer_ 已创建且可追加；输入为五类 record 值之一；无返回值，不修改输入业务字段。 |
| 核心算法与数据结构 | std::visit 配合 qlib::overloaded 的五个 const 引用分支；按具体类型选择 Add，不使用 variant 下标猜类型。 |
| 具体步骤 | 访问具体值；writer_->Add(value)；只有正常返回后调用对应 Metric::Add(1)；visit全部成功后，若测试after_add存在再调用一次；结束这一条，不重放、不筛除业务忽略。 |
| 成员读写 | 调用本线程 writer_，写四个 writer 指标之一；不读 Book 状态、接收指标或其他线程的容器。 |
| 对其他函数的调用 | 五个 ParquetWriter::Add 重载、Metric::Add，以及仅测试启用的after_add；不调用 DecodeFrame、Orderbook 或文件读取器。 |
| 边界与错误处理 | Add 抛错则不增加该条追加数；Add或Metric失败都不执行after_add；测试回调自身失败也传播为测试故障。variant 非法状态是内部契约错误；合法重复、未知ID、市价或方向冲突记录仍按原值保存。 |
| 异常安全与状态一致性 | Add 失败可能已有部分批次工作，不能重试。Add 成功后 Metric 溢出也必须报 Fatal；已经追加的行不能回滚，所以失败时不要求指标与可读行数相等。 |
| 时间与空间复杂度 | 类型选择及字段选择为固定成本；Add 可能触发整批压缩和I/O，不能把整个 Consume 描述成严格常数耗时。 |
| 并发、同步与线程安全 | 当前 Writer 是相应指标唯一写者；其他线程可读 relaxed 值，但不能把多个指标当同步快照。 |
| 单元测试与验证方法 | 五种输入逐字段读回；订单/撤单同计数而不同文件；Add失败不加数；指标临界值在线程启动前设置，再验证真实 Add 后的溢出失败。 |

| record 具体类型 | 调用 | 目标文件 | Add 成功后的指标 |
|---|---|---|---|
| qlib::order | Add(order) | orders.parquet | writer_orders + 1 |
| qlib::trade | Add(trade) | trades.parquet | writer_trades + 1 |
| qlib::cancel | Add(cancel) | cancels.parquet | writer_orders + 1 |
| qlib::level | Add(level) | levels.parquet | writer_levels + 1 |
| qlib::book | Add(book) | books.parquet | writer_books + 1 |

指标表示写入组件接受了这条记录，可能仍在列缓冲。它不是“已经 fsync 的行数”，也不能证明异常退出后的文件一定可读；完整性仍取决于正常 Close 和后续读取核对。

###### E. Close：显式完成保存，失败必须可见

| 实现维度 | `void Close()` |
|---|---|
| 函数声明与职责 | 将本次正常结束交给 ParquetWriter 的显式关闭协议。 |
| 输入、输出与契约 | writer_ 已创建，只有当前 Writer 调用，不和 Consume 并发；无返回值，成功通过正常返回表达。 |
| 核心算法与数据结构 | 直接调用 writer_->Close，不另建尾批缓冲或逐文件重试循环。 |
| 具体步骤 | 确认内部指针满足前置条件；调用 Close；成功后才可记录“输出正常关闭”，文件对象稍后由 Run 守卫清空。 |
| 成员读写 | ParquetWriter 内部由 Open 转 Closed；Writer 保留 run_dir_ 供诊断，尚不销毁队列。 |
| 对其他函数的调用 | ParquetWriter::Close；不调用上游 stop、join 或重新执行 Add。 |
| 边界与错误处理 | 已成功 Closed 的重复 Close 无操作；Failed 不能通过再次 Close 转成成功；尾批、文件结束信息和流关闭的任何错误传播。 |
| 异常安全与状态一致性 | 具体文件组件负责尽力释放其他资源和保留首错；本层不以 catch 后正常返回掩盖它。随后 reset 不能覆盖这次异常。 |
| 时间与空间复杂度 | 至多五类剩余批次的处理与关闭成本；底层 I/O 不承诺硬性完成时限。 |
| 并发、同步与线程安全 | 仅在 Writer 线程执行；Book 已 join 后才允许普通停止进入此路径。 |
| 单元测试与验证方法 | 不足一批的记录在正常结束后可读；重复成功关闭不重复行；关闭故障传播到 RunControl 且不能报告正常保存。 |

###### F. ClaimRunDirectory：以排他创建的结果确认所有权

内部函数采用 `std::filesystem::path ClaimRunDirectory(const path& root, std::string_view seed, std::size_t max_attempts)`。它只服务本模块，放在 qbook::detail 中，不成为用户配置或安装的公共 API。Open 传入内部常量 128 作为最大尝试次数；测试可以传入较小上限和固定 seed，确定地触发冲突或耗尽。

| 实现维度 | 内部 `ClaimRunDirectory` |
|---|---|
| 函数声明与职责 | 在输出根目录中取得一个新建的运行子目录并返回路径。 |
| 输入、输出与契约 | root 为配置路径；seed 是程序生成、无目录分隔符的有限文件名片段，max_attempts 为正；返回路径只属于本次调用。 |
| 核心算法与数据结构 | create_directories 准备根目录，之后以 create_directory 的布尔值与 error_code 判断每个候选，最多尝试 max_attempts 次。 |
| 具体步骤 | 检查内部参数；创建/确认根目录；依次生成 run-seed-0、run-seed-1；先查看 ec，非零则抛 filesystem_error；ec为空且返回true就返回；返回false表示已有目录，继续下一个；耗尽抛明确错误。 |
| 成员读写 | 自由函数只使用局部 path、error_code 和尝试计数；成功由 Open 保存到 run_dir_。 |
| 对其他函数的调用 | 标准 filesystem 目录操作与有限字符串格式化；不先用 exists 决定后续能否创建。 |
| 边界与错误处理 | 已存在目录可以跳过；根或候选为普通文件、权限/空间/路径错误均传播；没有随机字符串生成成功就算独占的捷径。 |
| 异常安全与状态一致性 | 不删除冲突目录、不覆盖内容；新目录创建后若后续保存路径/文件初始化失败，保留本次残留供诊断，不尝试再次占用。 |
| 时间与空间复杂度 | 最多K次目录创建和路径格式化，K为有限上限；额外空间为一个候选路径，实际系统调用耗时由文件系统决定。 |
| 并发、同步与线程安全 | 多调用竞争由文件系统原子目录创建区分胜者；没有检查存在与创建之间的所有权窗口。输出根按受控环境使用，本版不新增恶意路径替换防护协议。 |
| 单元测试与验证方法 | 预建 -0，确认返回 -1且旧内容不变；预建全部候选后失败；候选为普通文件报错；两个线程同seed分别拿到不同目录。 |

这里区分“已存在目录”和“其他错误”依据标准 `create_directory` 语义：[C++ 工作草案目录创建规则](https://eel.is/c++draft/fs.op.create.directory)。候选文件名相同并不总是可忽略的碰撞；例如同名普通文件应按文件系统错误报告。

##### 3.2.2.5 构造、初始化与资源释放

main 构造阶段对象时 writer_ 仍为空。Run 首先建立不会抛异常的清理守卫，再调用 Open，因此五个文件中途打开失败、追加失败、指标溢出以及关闭失败都走同一线程内清理。ParquetWriter 析构不执行业务 Flush；只释放剩余资源，必要的失败清理不重新写尾批次或覆盖原始异常。

正常路径必须先让 Close 成功，随后才由守卫 reset。Fatal 路径不调用普通保存 Close，只 reset 并由全局失败结果说明不完整；可能残留的文件不自动删除。文件操作与输出流销毁线程由第 3.1/3.4 节的真实流故障包装器记录，测试在 join 后检查。

WriterThread 自身的 noexcept 析构在主线程执行时只释放已经为空的 writer_ 和路径。不能把文件关闭拖到这个析构中，否则工作线程中的关闭错误就无法准确传播，而且违反文件资源单线程所有权。

##### 3.2.2.6 类内部的关键实现流程

1. 入口把队列、配置与 Metrics/RunControl 准备好，再启动 Writer。
2. Writer 排他取得目录，完整打开五类文件，之后才开始 try_pop。
3. 每个成功 pop 只调用一次 Consume；某类型达到批量时由 ParquetWriter 自行写出该类批次。
4. 普通停止前，入口已 join Book；Writer 观察 stop 后继续取队列，直到之后一次 pop 为空。
5. Writer 显式 Close，成功后释放文件资源，再返回供入口 join；只有完整服务最终无失败时才报告正常结束。

以 batch_rows=2 为例，依次消费订单、撤单、订单：第二条虽然令 writer_orders 达到2，却不会使 orders.parquet 写一批，因为撤单有自己的 Sink；第三条才使订单的两行满批。正常关闭还要写出撤单的一行尾批。这个例子说明“合并统计”与“按文件分别批量”不能混淆。

##### 3.2.2.7 类的实现顺序与依赖关系

先完成并独立验收 ParquetWriter，再实现纯目录辅助和构造；随后写 Consume 的五分支及指标映射，最后加入 Run 的普通排空、Fatal 检查、显式 Close 与作用域清理。错误注入与资源线程验证随 Run 一起完成，不能留到正式服务已经宣布可用之后。

#### 3.2.3 模块内部协作与调用流程

Book 只向 RecordQueue 写值，Writer 只弹出值；二者没有互相调用文件对象的方法。Writer 通过一个 const record 引用分派给 ParquetWriter，后者在自己的列缓冲复制所需字段。文件正常关闭后，测试读取器再打开文件，不同时用读取器窥探正在写入的目录来推断已处理数量。

| 边界 | 可以确认的事实 | 还不能确认的事实 |
|---|---|---|
| RecordQueue push 成功 | 记录已经交给唯一消费者可读取的队列。 | Add 已返回或文件可读。 |
| Consume 内 Add 成功返回 | 对应原值已被写入组件接受，追加计数可以增加。 | 尾批完成、所有文件关闭或断电安全。 |
| 显式 Close 正常返回 | 文件组件完成尾批及规定关闭步骤。 | 外部发布端没有丢消息或具备崩溃恢复能力。 |
| 所有阶段 join 且未 Failed | 可以按正常停止边界读取本次输出并验收。 | 未接纳的网络消息也受到内部排空保证。 |

#### 3.2.4 模块关键算法的实现方案

**接收后只消费一次。** pop 成功取得一个局部 record；即使 Add 触发磁盘错误，也不重新 push、不调整输入业务量、不在下一轮重复调用。文件可能已经接受部分压缩块，错误情况下没有可靠依据决定“整行完全没写”，所以本版选择失败退出而非自动补发。

**目录竞争不靠猜测。** 同一毫秒启动两次、相同时钟值甚至同一 seed 都可以工作；每个调用用 create_directory 的成功结果取得不同候选。根目录预先存在是允许的，旧运行目录被当作已有数据保留，生产服务不承担自动清理历史输出。

**失败优先于排空。** 正常停止的前提是上游已结束且没有 Fatal；任一循环发现 Failed，立即放弃继续消费。若磁盘调用本身长期阻塞，stop_token 不能凭空中断该库调用；这里只保证控制流返回检查点后能够退出，不承诺硬实时关闭期限。

#### 3.2.5 模块并发与同步实现

RecordQueue 一端固定为 Book（或隔离测试唯一生产者），另一端固定为 Writer。主线程不调用正在运行队列的 size/empty/try_pop，不读取 run_dir_ 或 writer_ 来猜状态；初始化完成可用约定日志或测试同步信号表达，路径和最终指标在 join 后读取。

指标保持 V0 的单写者 relaxed 更新。看到 writer_orders 增长只表示某些 Add 已完成，不是读取文件或另一个普通成员的同步许可；特别不能把多项指标的即时读数拼成一份保证一致的进度快照。

所有线程异常在各自入口 catch，并以静态阶段名 `"writer"` 调用 RunControl::Fail。Writer 内部不读 Error/Stage；协调者必须 join 所有潜在 Fail 调用者之后再读首条诊断。正常停止不会一次性停止 Feed、Book、Writer，而是依次停止并 join 上游，再通知下游排空。

#### 3.2.6 模块错误处理与恢复方案

| 错误或边界 | 本模块处理 | 保留的输出解释 |
|---|---|---|
| 候选目录已存在 | 换下一个有限候选，不触碰旧内容。 | 未覆盖旧运行。 |
| 根/候选文件系统错误 | Open 抛异常，Run 守卫在本线程清理。 | 没有成功初始化，不宣称采集已经开始。 |
| 五文件构造中途失败 | 构造负责释放已取得资源，保留目录路径。 | 可能有零字节或不完整文件，不能作为正常数据。 |
| Add、批次写入失败 | 不增加该条成功追加数，不重投，不继续本次运行。 | 已有前缀不保证可读或完整。 |
| Add 成功后指标溢出 | 抛错并进入 Fatal，不撤销已追加行。 | 指标不是事务，也不能用于故障文件完整性断言。 |
| 尾批/关闭失败 | 保留首错向线程边界传播，清理不再次业务写入。 | 进程非零，不能因部分文件可读就判正常。 |
| 其他阶段 Failed | 停止继续消费，当前文件仅做失败资源清理。 | 不保证 RecordQueue 排空或尾批已保存。 |
| 普通停止且无 Fatal | 在上游 join 后排空、显式 Close、reset。 | 对正常接纳并转交的记录进行完整输出验收。 |

本版没有从失败目录恢复继续追加、覆盖重跑或自动重试批次。操作者排除磁盘/权限问题后启动新实例，新实例取得新目录；必要故障证据由测试或操作者保留，不能用成功路径的临时清理规则删除旧输出。

#### 3.2.7 模块实现顺序

V3-01 的五类同步写入先完成，V3-02 才连接真实 RecordQueue。先验证独占目录和单次类型分派，再验证小容量正常排空，随后注入打开、写批次及关闭错误，最后把这个同一 Writer 实现接入 RunService。集成时不再另写简化消费者代替它。

#### 3.2.8 模块测试与完成条件

1. **五类及尾批。** 单一测试生产者记录每次成功 push，结束并 join 后请求 Writer 普通 stop；读取五个关闭文件，按类型顺序逐字段比较，不只数行。覆盖 batch_rows=1、满批、每类不足一批和零记录文件。
2. **指标含义。** 固定订单、撤单、成交、level、book 样本核对四个 Writer 指标；Add 失败不加当前条，Close 失败不把之前追加数清零，也不把它解释成完整文件。
3. **目录隔离。** 同根连续运行两次和固定 seed 竞争，核对目录不同，旧文件字节或哈希不变；已占用候选继续尝试，非目录错误及有限尝试耗尽明确失败。
4. **停机最后一条。** 生产者完成最后一次成功 push 并 join 后才发 stop，核对最后一行仍存在；配合 Run 中先读 stop 再确认空的代码顺序，覆盖旧空结果误用风险。
5. **故障与清理。** 使用统一的真实 OutputStream 故障包装，触发打开、批次和 Close 错误，记录资源创建/调用/释放线程；Run 退出后 writer_ 为空，主线程销毁阶段对象不再发生文件操作。测试线程也必须被异常包装和 join。
6. **权限测试可重复。** 根路径为普通文件是确定的错误用例；权限错误测试需确认实际运行身份不会绕过权限，不能在具有特殊权限的环境仅 chmod 后就假定一定失败。所有测试仅操作本次排他创建的测试目录。

Writer 代码与定向测试已实现。完整 Debug 和 Release 文件检查通过；追加计数、目录隔离、所属线程清理、末条排空和正常关闭均由测试核对。实际命令与结果汇总见第 5.5 节及本地版本记录。

### 3.3 正式入口与运行生命周期

V3-03 把已有阶段装配成可以启动、接收停止请求并明确报告结果的程序。`main` 决定读哪份配置，`RunService` 负责各对象活多久、线程怎样结束；它们不计算订单簿，也不直接拼接 Parquet 列。

#### 3.3.1 模块实现概述

1. **实现目标。** 提供真实 `qbook` 可执行文件，运行 Feed → Book → Writer；SIGINT/SIGTERM 触发正常排空，初始化、线程创建或运行失败触发非零退出。退出成功必须发生在 Writer 显式关闭成功且所有阶段已经 join 之后。
2. **规格依据。** `specs.md` 4.1.2、4.1.3、5.1、5.2、5.4，Sprint V3-03；对应 FR-01/02、BR-15、AC-01/11/12。V4 的 MetricsThread 发布循环不在本版创建，既有 Metrics 仍由三阶段更新。
3. **文件与组件。** 新增 `src/main.cpp` 和 `src/Service.cpp`；`include/Runtime.h` 增加 Config 前向声明及固定的 RunService 声明。保留 `src/Runtime.cpp` 的 RunControl 实现。仅供本工程的冷启动协作声明放 `src/detail/ServiceInternal.h`，服务生命周期测试放 `tests/service_lifecycle_tests.cpp`，隔离驱动放 `tests/service_test_driver.cpp`，子进程辅助放 `tests/support/`。
4. **复用依赖。** 使用 V0 配置、RunControl、Metrics，V2 Feed/Book，V3 Writer，以及标准 jthread 和目标 Linux 的信号接口。不复制配置解析、队列实现或文件清理规则。

`Service.cpp` 属于独立的 `qbook_service`，`Runtime.cpp` 属于不依赖 ZMQ/Arrow 的支持库。纯核心测试保持独立于通信和持久化；公开 RunService 接口与规格一致。

#### 3.3.2 Class Implementation（入口、协调器与信号组件）

##### 3.3.2.1 组件的实现目标

协调器像一名负责交接的值班者：先准备所有共享资源，再请各工作阶段开始；结束时先让上游停止交货，再让下游处理完手中剩余内容。Writer 出错则说明这条正常交接链已经断开，必须转入故障退出，不能继续无限等待空位。

信号处理器只把“用户想结束”记成一个值。关闭文件、记录日志和等待线程都由普通主线程执行，不在异步处理器里执行复杂操作。

##### 3.3.2.2 声明与文件组织

```cpp
// include/Runtime.h：保留原有 RunControl 声明。
namespace qbook {
struct Config;
int RunService(const Config& config);
}

// src/detail/ServiceInternal.h：仅供服务实现和隔离测试驱动使用。
namespace qbook::detail {
struct WriterHooks;
struct ServiceHooks {
  std::function<void(const char*)> before_start;
  const WriterHooks* writer_hooks = nullptr;
  std::function<void(const Metrics&)> coordinator_poll;
};
int RunServiceImpl(const Config& config,
                   const ServiceHooks* hooks = nullptr);
}
```

公开 RunService 固定调用 `RunServiceImpl(config, nullptr)`。内层始终创建真正的 `std::jthread`；隔离测试驱动可通过 before_start 在指定阶段创建之前抛异常。writer_hooks 经已约定的私有入口传给 Writer，用来提供测试输出流和 after_add 观察；coordinator_poll 只在主线程正常等待循环调用，并传入本次只读 Metrics，让驱动在同步目标达成后一次性 raise(SIGINT/SIGTERM)。必要时还核对有限计划尾部拒绝对应的 feed_messages/feed_dropped，不能合法输出先达标就提前停止。它们不加入配置、环境变量或主程序参数，也不能替换生产阶段为假阶段。空 hooks 不创建回调状态；这三个定向入口不扩展成插件、通用调度或故障框架，内部头文件不随公共接口安装。

信号处理器和 `SignalScope` 放在 Service.cpp 的内部作用域。后者保存旧屏蔽集合、两个旧 action 和逐步安装状态，提供构造、`EnableCoordinator()`、`Restore()` 与 noexcept 析构回退；不公开信号对象给工作阶段。main.cpp 只包含配置/运行入口和必要日志头文件，不包含 Arrow builders 或订单容器实现。

##### 3.3.2.3 局部数据、成员与所有权

| 对象/状态 | 类型及初始状态 | 生命周期与约束 |
|---|---|---|
| 配置 | `const Config&` | main 持有至 RunService 返回；RunService 不热更新。 |
| 共享运行对象 | `Metrics`、`RunControl`、FeedQueue、RecordQueue | 先构造，所有使用者 join 后才销毁。每次运行创建新的 RunControl。 |
| 阶段对象 | FeedThread、BookThread、WriterThread | 主线程保存对象，Run 在各自线程执行；网络和文件资源由对应 Run 创建和销毁。 |
| 阶段句柄 | 三个初始不可 join 的 `std::jthread` | 只有协调者改写句柄、request_stop 和 join；失败时只处理实际已创建者。 |
| 完成位 | 三个 `std::atomic<bool>{false}` | 各包装器在离开前 store-release，主线程 load-acquire；只用于等待完成，不替代 join 或 RunControl 载荷读取前提。 |
| 用户停止标志 | 静态 `volatile std::sig_atomic_t`，安装期间设为 0 | 仅主线程及在同一主线程执行的 SIGINT/SIGTERM 处理器访问；工作线程不读取它。 |
| SignalScope 保存状态 | 原 `sigset_t`、两个 `sigaction`、安装/恢复标记 | 只由主线程操作；逐步记录成功步骤，部分安装失败也能回退。 |
| 受限测试入口 | 借用的 `const ServiceHooks*`，正式入口为 nullptr | 驱动在调用前构造Hooks及其引用对象，服务所有阶段join后才销毁；before_start只在创建前执行，coordinator_poll只在主线程正常等待时执行，参数只开放Metrics原子读取。 |

完成位解决一个具体问题：主线程正常等待 Feed 结束时，也应能看见 Writer 已经失败，从而给所有已启动阶段请求停止。它先短步等待完成位并检查 Failed，再对已完成线程调用 join；不用在一个长时间阻塞的 join 中停止观察其他阶段。三个完成位不是新增业务指标，不发布到网络。

##### 3.3.2.4 重要函数的具体实现

###### A. main：加载配置并提供唯一的程序退出结果

| 实现维度 | `int main()` |
|---|---|
| 声明与职责 | 从固定相对路径加载配置，调用真正的三阶段服务并返回其退出码。 |
| 输入、输出与契约 | 无业务命令行参数；从运行工作目录读取 `config/config.yaml`；0 表示本次正常关闭成功，非零表示启动或运行失败。 |
| 算法与数据结构 | 一份 const Config，外层异常边界；不持有业务事件缓存或文件 writer。 |
| 执行步骤 | 初始化同步日志 → LoadConfig → 输出实际配置摘要 → RunService → 返回结果。配置不存在的默认策略和存在但不可读的错误策略完全复用 V0。 |
| 状态读写 | 只创建并读取 Config；不直接修改队列、阶段状态或各指标。 |
| 调用关系 | LoadConfig/ValidateConfig、RunService 和普通上下文中的日志。 |
| 边界与错误 | 捕获标准与未知异常，给出明确失败结果；日志失败不能使失败变成返回 0。不能在错误路径改成默认“成功运行”。 |
| 异常安全 | 配置失败发生在阶段创建之前；RunService 一旦启动工作线程，其内部必须先完成清理才能返回或传播异常。 |
| 复杂度 | 配置解析按文件长度；装配为冷路径；运行时间由服务寿命决定。 |
| 并发与同步 | main 是唯一协调线程，所有复杂操作位于普通执行上下文。 |
| 验证 | 非法配置子进程非零退出且没有工作阶段；有效配置进入真实服务；SIGINT/SIGTERM 后读取退出码和关闭后的文件。 |

###### B. RunService：创建资源、运行和顺序退出

| 实现维度 | `int RunService(const Config&)` / 内部 `RunServiceImpl` |
|---|---|
| 声明与职责 | 前者保留固定公开签名并传入空hooks；后者承载同一份真实生命周期代码。 |
| 输入、输出与契约 | Config 已验证；主线程调用，同一进程同时只运行一个实例。正常返回 0；启动、阶段或清理失败返回非零。 |
| 算法与数据结构 | 固定三个阶段、两条队列、三个完成位；信号停止与 RunControl 故障分别判断。 |
| 执行步骤 | 建立信号屏蔽/处理守卫 → 准备共享对象和阶段 → 建立异常清理守卫 → 启动 Writer、Book、Feed → 主线程启用信号接收 → 等停止或 Failed（隔离驱动的coordinator_poll可在此发起一次信号） → 正常按 Feed/Book/Writer 逐个停止等待；Failed 则向全部已启动者请求停止 → 全部 join → 恢复信号设置 → 检查首错并返回。 |
| 状态读写 | 只由主线程管理句柄和停止请求；工作线程各自更新其完成位及所属状态。主线程不查看活跃队列 size/empty 或订单簿。 |
| 调用关系 | 阶段构造和 Run、SignalScope、线程包装器、StopAndJoinStage、RunControl；不调用 ParquetWriter 的 Add/Close。 |
| 边界与错误 | 任意构造、启动、测试回调或监控异常进入统一 catch；启动到一半只 join 实际句柄。即使用户停止已经到达，只要稍后发生关闭错误，最终结果仍为失败。 |
| 异常安全 | 清理守卫先广播首错，再停止并等待所有已创建线程；保留共享对象至此结束。后续诊断错误不覆盖先前业务/写入错误。 |
| 复杂度 | 固定阶段协调成本；普通排空成本随已接纳剩余记录增长；系统调用/压缩等待没有硬性上限承诺。 |
| 并发与同步 | 每阶段唯一 Run；完成位用于等待，join 确保其执行和释放已结束；只有全部潜在 Fail 写者结束后读 Stage/Error。 |
| 验证 | 零、一个、两个阶段成功创建后的失败，真实阶段初始化失败，正常信号、积压停止和 Writer 关闭失败；确认没有悬空借用或错误的成功退出。 |

若提供before_start，先以静态阶段名调用它，再创建对应真实线程；它不能先创建隐藏线程再抛异常。每次成功创建线程后先保存其句柄，再尝试下一个阶段。开始下一个阶段之前观察 Failed；如已有失败就停止扩大启动范围。不能以“消费者线程已经创建”说明文件已经打开；真实初始化完成日志由对应 Run 在成功点记录。

###### C. 阶段包装器与 StopAndJoinStage

| 实现维度 | 内部线程入口包装与 `StopAndJoinStage` |
|---|---|
| 声明与职责 | 包装器捕获阶段异常并发布完成；协调函数请求停止、持续观察故障并等待真正结束。 |
| 输入、输出与契约 | 阶段名使用静态字面量 `"feed"`、`"book"`、`"writer"`；共享对象和完成位活到 join 后。 |
| 算法与数据结构 | 入口 try/catch，退出守卫写完成位；主线程短步轮询完成位，不建立额外信号线程。 |
| 执行步骤 | 包装器调用 Run；若 Run 返回时本阶段无 stop 且 RunControl 仍健康，则在同一 try 内抛“阶段提前结束”错误；catch(...) 调 Fail，最后发布 done。协调者 request_stop 目标，等待 done，同时检查 Failed；若 Failed，给所有 joinable 阶段 request_stop；目标完成后 join，再处理下一个尚未 join 的句柄。 |
| 状态读写 | 包装器只写自己的 done 和允许竞争的 RunControl；协调者写停止状态，读取 done/Failed。 |
| 调用关系 | Run、current_exception、Fail、stop_source 请求和 jthread::join；等待使用命名的内部短间隔。 |
| 边界与错误 | 已 join 或未创建的句柄跳过；普通停止中发生下游异常立即转为故障取消。某阶段在既无 stop 又无 Failed 时意外返回，应报告阶段提前结束，不能让主循环永远等用户信号。 |
| 异常安全 | 线程入口不让异常越过边界；退出完成位无论正常或异常均发布。清理中不 detach 尚在借用共享对象的线程。合法独占、非自 join 的调用由结构保证。 |
| 复杂度 | 固定阶段数；等待开销由短步轮询决定，不忙等，不添加新的可配置参数。 |
| 并发与同步 | done.store(release)/load(acquire)，最后仍 join；Fail 的 payload 不在观察 Failed/done 时提前读取。 |
| 验证 | 阶段抛错、正常返回、启动中止、下游失败解除上游满队列等待；所有已创建句柄均不再 joinable，资源随后销毁。 |

主循环和顺序收尾可复用例如 10ms 的协调检查间隔；这是内部策略值，实施时记录并测试，不能把它写成 10ms 停止 SLA。Book 和 Writer 仍使用自己的队列等待策略，主线程不承担消费角色。

###### D. HandleStopSignal：异步上下文只记录请求

| 实现维度 | `void HandleStopSignal(int) noexcept` |
|---|---|
| 声明与职责 | 接到 SIGINT 或 SIGTERM 时把用户停止标志设为 1。 |
| 输入、输出与契约 | 只注册到上述两种信号；处理器必须在协调主线程执行。没有返回数据。 |
| 算法与数据结构 | 对一个静态 volatile sig_atomic_t 进行简单赋值，不累加次数。 |
| 执行步骤 | `stop_requested = 1;` 然后返回。 |
| 状态读写 | 只写停止标志；不碰 RunControl、日志、队列、异常或线程句柄。 |
| 调用关系 | 不调用业务函数，也不调用 request_stop、join、分配或 mutex。 |
| 边界与错误 | 重复信号仍为 1，不把第二次信号定义为强杀或丢弃排空；SIGKILL 不属于可处理的正常停止。 |
| 异常安全 | 函数 noexcept，无异常、动态初始化和资源操作。 |
| 复杂度 | 固定一次赋值，常量额外空间。 |
| 并发与同步 | 工作线程屏蔽两种信号且不读此标志；volatile 不是一般跨线程同步工具。 |
| 验证 | 隔离子进程分别发送 SIGINT/SIGTERM及停止过程中的重复信号；检查后续普通主线程执行清理，处理器内无复杂调用。 |

###### E. SignalScope：建立并恢复可解释的信号所有权

| 实现维度 | 构造、`EnableCoordinator()`、`Restore()` 及析构回退 |
|---|---|
| 声明与职责 | 在启动前保存信号环境，确保工作线程继承屏蔽；开始协调时仅主线程接收请求；结束后恢复原状态。 |
| 输入、输出与契约 | 主线程独占，同一进程没有并行的服务实例或其他代码修改这两个信号处理器；调用前尚未创建应用后台线程。 |
| 算法与数据结构 | sigset_t、两份旧 sigaction、逐步完成标记；处理器 action 的 mask 同时包含两种停止信号，避免彼此嵌套。 |
| 执行步骤 | 构造时 pthread_sigmask 阻塞两信号并保存旧集合，清零停止标志，依次保存/安装两action；工作线程全部创建后 EnableCoordinator 仅在主线程解除这两信号的屏蔽，即使进入时原mask已屏蔽它们；所有工作线程 join 后 Restore 恢复主线程原mask及原action，标记已恢复。 |
| 成员读写 | 只改内部恢复信息、主线程mask和进程action；不保存指向临时对象的信号回调上下文。 |
| 调用关系 | sigemptyset/sigaddset、pthread_sigmask、sigaction；返回值检查位于普通主线程，不在处理器里格式化错误。 |
| 边界与错误 | pthread_sigmask 直接返回错误码，sigaction 失败返回-1并设置errno；分别转换，不能统一误读errno。安装第二项失败时回退已完成项。启动期间挂起的停止信号在主线程解屏蔽后形成停止请求。 |
| 异常安全 | 每成功一步才登记恢复责任；显式Restore失败使运行结果非零，已有首错保留；析构只能尽力回退且不抛，不能用析构吞错后宣称恢复成功。 |
| 复杂度 | 固定两种信号的冷路径操作；无额外工作线程或业务缓冲。 |
| 并发与同步 | POSIX新线程继承创建者mask；Feed内创建的ZMQ内部线程也从已屏蔽的Feed线程继承。主线程启用后不再创建未屏蔽的应用后台线程。 |
| 验证 | 在隔离驱动核对worker中两信号被屏蔽、主线程可接收，部分安装/启动失败后按已完成步骤恢复；禁止向Catch2父进程发送服务停止信号。 |

这里选用目标 Linux 的明确方案，不把一个全局 volatile 值宣称为任意平台的线程安全开关。依据是 Linux man-pages 项目的 [pthread_sigmask](https://man7.org/linux/man-pages/man3/pthread_sigmask.3.html)、[signal](https://man7.org/linux/man-pages/man7/signal.7.html) 和 [sigaction](https://man7.org/linux/man-pages/man2/sigaction.2.html)，以及 C++ 工作草案的 [信号处理限制](https://eel.is/c++draft/support.signal)与[同线程 sig_atomic_t 规则](https://eel.is/c++draft/intro.races)。这是依据接口契约作出的项目实现选择；所选编译器、标准库和实际链接库仍须通过子进程用例验证。

恢复阶段三个应用工作线程均已结束，信号设置仍由主线程独占处理。先在自有处理器仍安装时恢复主线程原 mask，使原 mask 允许的已挂起请求仍由本处理器接收，再恢复旧 action；如原 mask 本来屏蔽该信号，则保留其原有屏蔽语义。恢复完成后的新信号由调用前规则处理，不再把它解释为尚在运行的 qbook 停止请求。失败清理也遵守同一顺序，且不重置已经观察到的失败结果。

##### 3.3.2.5 构造、初始化与资源释放

主线程先安装信号环境，再构造 Metrics、RunControl、队列和阶段对象，最后才创建线程。阶段对象构造不打开网络或文件；任何构造失败都不会留下已启动的业务线程。异常清理守卫必须在第一条线程创建之前就有效，不能等三条都成功后才补上。

先启动 Writer，再启动 Book，最后 Feed，目的是先提供下游消费角色。此顺序不意味着 Writer 完成初始化早于 Feed；若 Writer 初始化失败，RunControl 会让其余阶段退出。生产入口无需增加“全阶段同时就绪”屏障，不把初始化次序与数据完整性混为一谈。

正常返回前 Writer 已在自身线程执行显式 Close 并清空 writer_，Feed 已在自身线程销毁 socket/context。主线程销毁的阶段外壳此时不再执行文件或网络操作。信号环境最后恢复；Config 在 RunService 返回后销毁。

##### 3.3.2.6 关键实现流程

正常停止以实际交接顺序推进：主线程看见停止标志，先给 Feed 普通 stop；Feed 放弃尚未接纳的等待事件并结束，已成功入 FeedQueue 的仍由 Book 接收。Feed join 后才 stop Book，Book 排空并结束；Book join 后才 stop Writer，Writer 排空 RecordQueue、写尾批次、Close，随后 join。整个过程若出现 Failed，立即按故障规则取消，最终非零。

例如 Writer 写第二批时失败，Book 可能正等 RecordQueue 空位，Feed 又可能等 FeedQueue 空位。Writer 包装器设置 Failed 后，两处重试都能退出。主线程不再要求失败文件满足接纳清单，不等一条已经停止消费的链路自行排空；它等待所有已启动线程结束，再报告首错及失败输出目录。

##### 3.3.2.7 实现顺序与依赖

先落实 SignalScope 的单线程安装/回退和线程屏蔽继承，再实现真实线程创建、包装器与完成位；随后组装正常服务及有序退出，最后加入启动失败与运行失败清理。所有路径共用真实生命周期代码，测试不得另抄一份“看起来相同”的装配算法作为被测服务。

#### 3.3.3 模块内部协作与调用流程

main 加载配置后调用 RunService；协调器创建三个阶段，阶段之间只通过两条 SPSC 队列传值。主线程的控制操作只有停止请求、故障观察和 join，不越过阶段调用其底层网络/文件对象。

启动日志说明实际配置、模式、队列请求及实际容量、快照周期、输出根目录和批量；V3 中配置的 metrics 端点尚未启用，不能打印成已绑定。阶段日志分别以明确标识 `feed_connected` 说明 Feed connect 配置完成、以 `writer_opened` 说明 Writer 五个文件成功打开及真实运行目录；`service_control_ready` 只说明信号处理与协调等待已经启用。连接调用返回不等于订阅完成，文件打开也不等于已有行情。

最终正常日志只在所有线程 join、Writer Close 无异常后输出；失败日志注明首错阶段与异常，不输出“完整采集成功”。日志不是接纳账本，精确完整性仍由受控输入和关闭后的文件比较证明。

#### 3.3.4 模块关键算法的实现方案

**正常与失败优先级。** 每次主循环先观察 Failed，再观察用户停止。进入正常收尾之后仍观察 Failed；用户先发 SIGTERM、Writer 后报 Close 失败时，退出结果必须为非零。不能在接到信号时就提前固定返回值为 0。

**末条排空。** 主线程只在上游 join 后请求下游 stop；Book/Writer 的循环先观察 stop，再在这个观察之后确认输入为空。这样“最后一次成功 push”发生在上游结束前，不能被下游一次过早的空检查漏掉。协调顺序和消费循环的空确认顺序缺一不可。

**首错保留。** 任何启动/阶段异常先进入现有 RunControl，清理失败只能作为附加诊断，不能覆盖获胜的首错。所有阶段 join 后才读 Stage/Error；三个 done 位不改变这一明确前提。普通停止、有效输入拒绝和业务忽略不调用 Fail。

#### 3.3.5 模块并发与同步实现

每条队列保持单生产者、单消费者。主线程运行期间只读原子指标、阶段完成位和 Failed；不读取 Writer 私有 run_dir_、writer_、Book 内部品种表或正在增长的测试结果。运行目录由 Writer 初始化日志报告；组件测试如需直接观察，必须在其线程 join 后通过受限测试观察器读取。

两个停止信号只交由协调主线程处理，且处理器只改自己的 sig_atomic_t 标志。工作线程继续通过各自 stop_token 和 Failed 决定退出；不能让它们直接读取主线程的 volatile 变量，也不能从处理器调用 RunControl::Fail。依赖库若在别处创建未屏蔽的后台线程，需要在接入时明确调整创建顺序和屏蔽继承，不能保留当前假设后仍宣称验证通过。

#### 3.3.6 模块错误处理与恢复方案

| 发生位置 | 处理与后续行为 |
|---|---|
| 配置、信号安装或队列/阶段构造 | 未启动业务线程时释放已构造资源，恢复已改信号设置，返回非零。 |
| 第 k 条线程创建 | 保存当前异常、取消并 join 已创建者，再释放依赖；不依赖 jthread 逆序析构碰巧排空。 |
| Feed/Book/Writer Run | 包装器捕获并调用 Fail，发布完成；其他等待观察 Failed 退出。 |
| 普通收尾中的批次/Close | 转为失败退出；先前 Add 指标和部分文件不回滚，不标记为完整采集。 |
| 最终信号恢复或诊断 | 不覆盖已有首错；原本健康但恢复失败时也不能返回 0。析构回退不抛异常。 |

本版不在同一运行对象上自动重启三阶段，不把失败输出目录复用为下一轮。失败之后可以由操作者重新运行，新运行创建新对象和独占目录。强杀、进程崩溃或无法中断的底层系统调用不属于正常排空保证；测试超时应如实失败，不能把强制杀死视作正常清理。

#### 3.3.7 模块实现顺序

1. 在 Runtime.h 添加固定运行声明，将真实装配放 Service.cpp 并接入服务目标，保持支持库依赖不变。
2. 实现 SignalScope、简单处理器、真实线程创建和异常包装，检查信号与完成位的所有权。
3. 创建真实三阶段并完成普通停止；先用空闲进程验证五文件打开及显式关闭。
4. 加入 Failed 优先、部分启动清理和首错报告，在隔离测试驱动中复用同一内层函数和before_start注入创建失败。
5. 对正式 qbook 进程发送两种停止信号，读取退出状态及文件；更新 README 的工作目录、启动命令、输出位置和退出说明。

#### 3.3.8 模块测试与完成条件

测试将真正的 qbook 放在专属工作目录下运行，预先写入该目录的 `config/config.yaml`；正常 SIGINT/SIGTERM 场景等待 `service_control_ready`、Writer 成功初始化和 Feed 连接配置完成的真实日志，再发送信号。子进程应正常返回 0，五类空文件都可读取、schema 正确；这只能证明空闲生命周期，不代替带数据的完整链路验收。

有行情的精确接纳、尾批次、顺序和业务忽略保存由第 3.4 与第 5 章的受控阶段链路核对。子进程工具采用适合多线程测试进程的 spawn/exec 方式；如用 fork，子进程在 exec 前仅做允许的底层操作，不能复制父进程后继续使用 C++ 日志/锁。所有 kill 都针对已保存的子进程身份，不向 Catch2 父进程发服务停止信号。

部分启动失败使用单独测试驱动通过before_start注入第 k 次创建前异常，驱动调用同一 RunServiceImpl，正式 qbook 不增加测试命令行或隐藏环境变量。实际测试核对已启动阶段结束、文件资源归属及非零退出；外部期限只作防挂起保护，超时属于失败。信号屏蔽继承、挂起信号、原环境恢复、SIGINT/SIGTERM 和正常停止中的尾批/Close 故障均已有子进程测试。

有数据的驱动把 Writer after_add 发布的目标信号经 release/acquire 交给主线程 coordinator_poll(const Metrics&)；若有限计划含尾部非法帧或模式拒绝，还须等本次 feed_messages/feed_dropped 达到明确目标，再一次性 raise。只读 Metrics 不开放队列、业务对象或输入接纳集合；其多个 relaxed 值不能当作一致同步快照，Writer观察结果的同步证据仍是独立目标信号的release/acquire。回调一次性 raise 后不再发送信号，不在工作线程安装或触发主线程处理逻辑。发布者放在父测试进程或独立进程，使子进程在 SignalScope 建立前没有未屏蔽停止信号的后台线程。上述目标只能证明对应 Add 已返回，完整文件仍须等服务正常结束后读取；没有新生产接纳接口。

### 3.4 文件读回、故障夹具与全链路测试工具

本模块对应 V3-04，并为前三项任务提供检查工具。V2 的 Collector 已能证明记录到达下游；V3 改为让真实 Writer 保存文件，测试必须在正常关闭后重新打开文件、读取字段并比较，不能只检查路径存在或追加计数增长。

#### 3.4.1 模块实现概述

1. **模块目标。** 建立独立文件读回、可定位的 I/O 故障、明确的网络场景完成信号和受控子进程生命周期，使正常保存与失败退出都有可复验结果。
2. **规格依据。** `specs.md` 4.4、5.1～5.4、6.1～6.3；Sprint V3-04；FR-02、FR-11、FR-12，BR-12/15。测试工具不增加线上记录字段、配置项或外部查询接口。
3. **组件与文件。** `tests/support/ParquetReadback.h/.cpp`、`FailingOutputStream.h/.cpp`、`WriterGoalObserver.h`、`ProcessHarness.h/.cpp` 和 `tests/service_test_driver.cpp`；具体场景位于 `parquet_writer_tests.cpp`、`writer_thread_tests.cpp`、`service_lifecycle_tests.cpp`、`service_integration_tests.cpp`。
4. **依赖与复用。** 复用 V1 固定业务输入、V2 的 FeedEncoder/FeedPublisher 和成功入队账本方法；使用真实 qlib 队列、生产阶段、Arrow/Parquet Reader 和 Linux 进程控制。期望字段独立写在夹具中，不调用生产写入器的 schema/Append 方法产生答案。

#### 3.4.2 Class Implementation（测试组件实现）

##### 3.4.2.1 ParquetReadback：重新读取关闭后的五类文件

**1. 组件的实现目标。** 把五个已经关闭的文件读成可逐字段核对的数据，发现列错位、类型变化、尾行丢失和错误元数据；它不重建订单簿，也不根据写入计数填补缺行。

**2. 声明与文件组织。** 采用自由函数 `ReadRunDirectory(const std::filesystem::path&, std::int64_t max_rows_per_file)`，返回测试专用 `ReadbackResult`；该结果拥有五个 Arrow Table 及必要文件元数据。另用 `CheckExpectedFiles(const ReadbackResult&, const ExpectedRun&)` 比较独立预期。接口只在 tests 下，主程序不链接它。

**3. 数据的具体实现。** 五类文件使用固定顺序的数组或具名字段，不扫描任意 `.parquet` 文件来猜测类型。Reader、输入流是函数局部拥有对象，Table 在返回值中保留独立读取的数据。max_rows_per_file 来自冻结用例；先检查文件元数据行数再申请整体读取，防止夹具错误使测试意外读取大量数据。ExpectedRun 保存明确列名/类型、每类有限预期行和需要寻找的快照条件。

**4. 重要函数的具体实现。**

| 实现维度 | `ReadRunDirectory` 与 `CheckExpectedFiles` |
|---|---|
| 函数声明与职责 | 前者打开五个固定文件并读取真实数据，后者将实际 schema、元数据和行值与独立期望比较。 |
| 输入、输出与契约 | Writer 已 join 或子进程已正常退出，目录属于本用例；输出不引用运行中的 Writer。读取失败或差异必须使测试失败。 |
| 核心算法与数据结构 | 使用 `arrow::io::ReadableFile`、`parquet::arrow::FileReader` 及 Arrow Table；Parquet footer 用于核对行数和列压缩信息。 |
| 具体执行步骤 | 按五个固定名称逐个打开；核对 footer 行数上限；读 schema 和全部行；检查列名、顺序、类型、null及元数据；按同类型行序与预期逐行比较；另查满足最终条件的快照。 |
| 数据读取与修改 | 只读关闭后的文件；填充本次 ReadbackResult；不改写文件、不调用生产 Add，也不修改原始输入期望。 |
| 对其他函数的调用 | 检查每次 Arrow Status/Result；调用 Reader 的读取和 metadata 接口。具体签名以锁定版本为准，不忽略返回值。 |
| 边界与错误处理 | 零行也检查完整 schema；缺文件、缺 footer、错误类型、null、超限或缺字段均失败；不把读取异常转换成空表。 |
| 异常安全与状态一致性 | 任一读取失败释放局部 Reader/输入流，保留已有文件供定位；不返回一份被误认为完整的部分结果。 |
| 时间与空间复杂度 | 有限数据的读取/比较为 O(R×C)，R 为总行数、C 为固定列数；整体读取内存受用例行数上限约束。 |
| 并发、同步与线程安全 | 读回发生在 Writer 完成和 join 之后；不在写入尚未结束时轮询半成品文件作为完成信号。 |
| 单元测试与验证 | 用手工确定的字段、多个批次、零行、非零列表和空侧验证；构造错列/错类型的小文件证明检查器能拒绝错误。 |

Arrow Table 的一列可能分成多个 `ChunkedArray` 分块，而且不同列的分块边界未必相同。比较器为各列维护自己的块索引与块内偏移，按统一行号读取；不能只取 `chunk(0)`，也不能假定同一块号代表相同行范围。整数直接按 int64、uint32、uint8 读取，不经过 double 或 JSON 浮点转换。

快照四列必须恢复为长度 1 的 fixed_size_list，列表本身与子元素均无 null，再按元素偏移读取 int64。空侧是实际的 `[0]`，不能替换为 null。格式元数据检查第 3.1 节规定的版本、倍率、时间、深度和枚举含义；保留 Arrow 的 `ARROW:schema`，不要求其序列化字节跨库版本完全相同。ZSTD 检查针对实际存在的列块；零行文件没有列块时，不能凭空断言已经测到压缩数据。批次正确性由行数、顺序和边界用例证明，不要求物理 row group 数量固定等于应用批次数。

**5. 构造、初始化与释放。** 准备输入与 ExpectedRun 时即固定期望，不在读到输出后修改答案。文件句柄只在读回期间存在，异常由局部资源对象清理；结果比较完成后释放 Table。成功运行数据是否保留按第 5.2 节的最小证据规则处理。

**6. 关键流程。** 例如批量为 2，连续写入三条订单，显式 Close 后读回三个实际订单行；第二条的 qty 即使已在簿中被成交扣减，文件仍应保持它最初的事件值。这个用例同时核对完整批次与不足一批的尾行。

**7. 实现顺序。** 先读单个零行/单行文件，再加入五种类型和元数据，随后处理多块与列表，最后复用于完整服务输出。读取器先能发现人工构造的差异，再用来判断生产写入器正确。

##### 3.4.2.2 FailingOutputStream：让真实写入路径遇到确定错误

**1. 类的实现目标。** 在指定的打开、写入或关闭阶段产生可重复的错误，验证生产代码是否传播首错、停止继续追加并在 Writer 所属线程释放资源；不用填满真实磁盘或修改用户目录权限来碰运气。

**2. 声明与文件组织。** `FailingOutputStream` 在 tests 下继承当前 Arrow 版本的 `arrow::io::OutputStream`，包装一个真正排他创建的本地文件流。实现所需 Write 重载、Tell、closed、Close 和 Abort；通过 `src/detail/OutputStreams.h/.cpp` 的私有 `OutputStreamFactory` 接入 ParquetWriter，`WriterHooks` 引用该类型，工厂打开错误在返回流之前注入。它只模拟声明的 I/O 失败，不自动覆盖 Reserve/Finish 的内存分配失败。

**3. 成员变量的具体实现。** 包装器拥有底层流、冻结故障计划和 Writer 线程独占的有限操作计数；独立 `StreamProbe` 保存预留容量的调用轨迹、所属线程 ID 和必要原子闸门。测试主线程可以保留 Probe，但不能额外持有流的最后一个 shared_ptr，否则对象析构可能错误地延迟到主线程。操作轨迹只在 join 后读取，跨线程控制只通过原子或有锁条件变量。

**4. 重要函数的具体实现。**

| 实现维度 | 工厂打开、`Write`、`Close` 与 `Abort` |
|---|---|
| 函数声明与职责 | 工厂创建真实流；Write 在选中时返回 I/O 错误；Close 验证关闭错误传播；Abort/析构只完成资源清理。 |
| 输入、输出与契约 | 路径是本次独占目录下的文件；调用来自唯一 Writer 线程；故障方式和触发条件事先冻结。工厂按统一签名返回 shared_ptr，打开失败抛异常；流的虚函数按 Arrow 接口返回 Status/Result。 |
| 核心算法与数据结构 | 对正常操作委托底层真实文件；选中故障时记录一次触发，不自行重发数据；计数和轨迹均有上限。 |
| 具体执行步骤 | 记录文件角色与操作；判断故障是否已武装；必要时进入有限可取消闸门；正常则委托；选中错误则返回指定 Status；关闭失败仍经后续 Abort/资源释放结束句柄寿命。 |
| 成员变量的读取与修改 | Writer 更新操作数、状态和轨迹；协调者仅设置原子武装/放行/取消信号，不直接操作底层文件。 |
| 对其他函数的调用 | 使用生产的排他打开规则，调用底层流对应操作；进程内夹具可借用本场景 RunControl 查询 Failed，隔离驱动使用自己的取消信号和绝对期限，不假定能直接访问服务内部控制对象；不重试被测批次。 |
| 边界情况与错误处理 | FileWriter 创建时也可能 Write 文件头，故障不能误在该阶段触发后又声称验证了运行中背压；追加成功观察点出现后才武装所需下一次 Write。 |
| 异常安全与状态一致性 | 第一个注入错误与后续清理错误分开记录；Abort 后拒绝新的业务 Write，释放底层资源；不让清理异常盖过首错。 |
| 时间复杂度与空间复杂度 | 正常委托增加固定检查，轨迹空间有限；故障闸门等待受测试期限、取消和 Failed 控制。此夹具不用于性能成绩。 |
| 并发、同步与线程安全 | 包装器与底层流始终由 Writer 调用和最终释放；闸门不压过故障/取消；Probe 的普通数据在 join 后检查。 |
| 单元测试与验证方法 | 分别证明 Open、后续 Write、尾批次/关闭失败真的触发；核对触发操作、传播错误、没有重试批次及资源释放线程。 |

Write 的第一种确定故障可在提交给底层前返回错误；更贴近“可能部分写入”的用例可先向底层写入一个受控前缀再返回错误，触发后不继续重试原批次。两者分别登记，不能把一种方式说成覆盖所有磁盘故障。Close 注入要明确是 footer 写入失败还是流 Close 返回失败：前者通过对应 Write 触发，后者可在底层关闭后返回指定错误，使生产代码必须报告失败，同时资源确实可释放。

若要制造“Writer 写入暂停、上游两条队列积压”，在指定记录的 after_add 回调内同步武装特定文件的下一次批次 Write，再返回回调；也可让唯一生产者在发送触发记录前等待明确的武装确认。不能由协调者读到首条计数后才异步武装，却假定下一次 Write 尚未发生。Probe 发布“已进入闸门”后，再推进有限输入形成积压。正常测试在等待 Book 排空前必须放开闸门；故障测试让闸门返回错误。超时清理总能取消闸门，闸门自身也检查绝对期限，不得靠不可取消的测试锁造成无限 join。

**5. 构造、初始化与释放。** 计划、Probe 容量及被借用的控制对象在线程启动前准备，流在 Writer 的 Open 路径创建。析构不抛出，不补写未完成业务批次；库自身析构是否尝试关闭仍按固定版本验证，不能据此声明失败文件已完整。join 后再释放 Probe 并读取轨迹。

**6. 关键流程。** 批量设为 2，在第一条记录的 after_add 回调中同步武装第二条导致的 Write，然后返回。第二条 Add 抛出时，Writer 不为第二条增加成功追加数，线程包装设置 Failed，Book/Feed 的等待退出。即使文件已有第一段字节，运行仍为失败，不重放第二条“补齐”。

**7. 实现顺序。** 先无故障透明委托并正常读回，再做打开失败，随后加入武装后的写入和关闭失败，最后加入可取消闸门与线程轨迹。不要先搭建通用 I/O 模拟框架。

##### 3.4.2.3 WriterGoalObserver：给网络用例一个明确结束点

**1. 类的实现目标。** 由实际追加记录的 Writer 判断“目标原事件已经到齐，而且此后出现了相应快照”，协调者据此开始正常停止。它避免把多个 relaxed 指标混读成同步快照，也不把 PUB 发送完成当作已经保存。

**2. 声明与文件组织。** 在 `tests/support/WriterGoalObserver.h` 定义小型测试类，提供 `void Observe(const qlib::record&)` 和 `bool Reached() const noexcept`。通过 `WriterHooks::after_add` 调用 Observe；回调只在真实 Add 与对应 Metric::Add 均成功后执行。正式 Writer 的 hooks 为空，公开构造签名不变。

**3. 成员变量的具体实现。** 保存冻结的原事件目标数、目标品种、预期买卖一档条件；Writer 独占 raw_seen_、各类计数和目标品种最近事件时间。仅 `goal_reached_` 跨线程，初始 false、成功时 release store，协调者 acquire load。目标一旦发布不撤销；它只授权开始结束流程，后续失败仍必须使场景失败。

**4. 重要函数的具体实现。**

| 实现维度 | `Observe` 与 `Reached` |
|---|---|
| 函数声明与职责 | Observe 在 Writer 已成功追加后检查实际值；Reached 让协调者查询同步后的目标信号。 |
| 输入、输出与契约 | 输入是只读 record，回调由一个 Writer 调用；目标和对象寿命覆盖整个运行。Reached 不提供文件关闭承诺。 |
| 核心算法与数据结构 | 固定类别计数加简单阶段状态；先收齐目标原事件，再接受下一张指定品种且字段正确的快照。 |
| 具体执行步骤 | 原事件分支增加受检计数并记目标品种时间；book 分支只在原事件目标已达成后检查品种、深度、价格数量与时间；匹配则 release 发布 true；协调者 acquire 读取。 |
| 成员变量的读取与修改 | 仅 Writer 修改普通字段；协调者只读 goal_reached，不并发读取这些计数或快照缓存。 |
| 对其他函数的调用 | 使用 variant 的实际类型分派，不调用 Orderbook 重新生成答案；不修改 record、队列或生产指标。 |
| 边界情况与错误处理 | 目标前的快照不能提前满足目标；其他品种不算；超出预期、超时或最终未匹配均失败。可无快照目标的场景只要求原事件完成。 |
| 异常安全与状态一致性 | 正常 Observe 不分配、不调用 Catch2 断言；意外异常由 Writer 包装转 Fatal；即使已发布 goal，随后 Close 出错也不能判成功。 |
| 时间复杂度与空间复杂度 | 每条记录只做固定字段检查，O(1) 时间和固定内存；不收集无限完整事件副本。 |
| 并发、同步与线程安全 | release/acquire 明确连接完成信号；其他共享指标仍各自独立，不能反过来拿它们证明非原子数据已同步。 |
| 单元测试与验证方法 | 先快照后原事件不会提前完成；目标后错误品种/错误值不完成；正确快照完成；尾部拒绝计数与最终文件检查另行执行。 |

**5. 构造、初始化与释放。** 在线程启动前构造 Observer 与 WriterHooks，回调捕获稳定引用；不得运行中修改回调或目标。所有使用者 join 后才析构。对网络输入，recv_ns 由 Feed 覆盖，观察器从真实原事件记下该时间，再核对后续快照，而不是要求远端哨兵值保留。

**6. 关键流程。** UC-02 的六条原事件依次被 Writer 追加后，观察器等待下一张该品种快照，并核对买一 100/7、卖一 101/6。协调者见到 goal 后仍要满足本用例的完整接收/拒绝目标，随后正常停止。全部 join 或子进程退出后，独立 Reader 再证明这些真实记录最终在关闭的文件中。

**7. 实现顺序。** 先用同步 record 序列证明目标状态机，再接 Writer 私有测试构造；最后用于网络集成与测试子进程。不用它替代受控生产者的成功入队账本。

##### 3.4.2.4 ProcessHarness：隔离真实程序与信号测试

**1. 类的实现目标。** 在独立子进程中运行正式 qbook 或测试专用 driver，精确控制工作目录、信号、退出期限和文件检查，避免在多线程 Catch2 宿主内临时安装进程级处理器。

**2. 声明与文件组织。** `ProcessHarness.h/.cpp` 提供一次性启动、向本次子进程发送信号、限时等待与结果读取；内部保存 `pid_t`、管道和终止状态。`tests/service_test_driver.cpp` 链接与正式程序相同的 Service 实现，只负责建立私有 hooks 和冻结用例；它不是用户运行入口，不向正式 qbook 增加测试命令行或环境变量。

**3. 成员变量的具体实现。** Harness 拥有独占临时工作目录、明确的 executable 绝对路径、已冻结 argv/env、stdout/stderr 管道以及本次子进程 PID。准备 `config/config.yaml` 后再启动；不在父进程调用 `chdir`。输出保存有固定字节上限，达到上限后继续排空管道但只保留有限诊断并将超限记为测试问题，不能停止读导致子进程阻塞在日志写入。

**4. 重要函数的具体实现。**

| 实现维度 | `Start`、`SendSignal`、`Wait` 与失败清理 |
|---|---|
| 函数声明与职责 | 启动指定子程序，发送一次受控信号，回收退出结果；异常时只清理本实例创建的子进程。 |
| 输入、输出与契约 | 可执行路径和临时目录已准备；只接受有效且尚未回收的子 PID；返回退出码、是否被信号终止及有限诊断。 |
| 核心算法与数据结构 | Linux 进程创建、pipe/poll/read、kill 和 waitpid；子进程状态由单个协调者维护，不创建后台无限读取任务。 |
| 具体执行步骤 | 准备管道/启动属性；启动后关闭父端无用句柄；持续排空输出并检查期限；收到必要就绪证据才发目标信号；等待退出、读完管道并回收；最后读文件。 |
| 成员变量的读取与修改 | 只修改本实例 PID/管道/结果；不更改宿主进程 cwd，不向所有同名 qbook 进程发送信号。 |
| 对其他函数的调用 | 调用明确的 POSIX 进程 API；错误按系统返回值处理，EINTR 可继续有限等待；使用独立 Reader 检查输出。 |
| 边界情况与错误处理 | 启动失败、提前非零退出、未就绪、超时、管道EOF和信号发送失败分别记录；reap 后立即撤销 PID 操作权限，避免误操作复用编号。 |
| 异常安全与状态一致性 | 所有父端句柄用资源对象管理；已有子进程时失败先发TERM并等待有限宽限，必要时KILL后reap；走过强制清理的用例始终失败。 |
| 时间复杂度与空间复杂度 | 等待时间受测试期限保护，保存日志空间有上限；不承诺生产不可中断 I/O 的硬停止上限。 |
| 并发、同步与线程安全 | 信号处理仅在子进程；父进程可拥有XPUB线程，但其socket仍由该发布线程独占；子进程不借用父进程C++对象。 |
| 单元测试与验证方法 | 正式qbook分别SIGINT/SIGTERM正常退出；错误配置/端点非零；driver部分启动和写入失败非零；超时失败能回收本次子进程。 |

本版 Linux 测试采用局部 `fork`/`execve` 方案。所有字符串、参数数组和句柄计划必须在 fork 前准备；多线程父进程 fork 后的子分支只做允许的低层操作，如 chdir、dup2、close、execve 和失败时 `_exit`，不运行 C++分配、日志、析构或测试断言。需要报告 exec 错误时，用事先创建的错误管道写入固定大小错误码，由父进程生成说明；不能在子分支拼接错误字符串。不能为了方便在 fork 后调用一段普通 C++ 配置代码。进程与信号约束可核对 [Linux fork](https://man7.org/linux/man-pages/man2/fork.2.html) 与 [signal-safety](https://man7.org/linux/man-pages/man7/signal-safety.7.html) 文档，实际接口仍由目标工具链编译验证。

正式 qbook 的测试使用父进程向精确子 PID 发送 SIGINT/SIGTERM，依据阶段初始化日志确认该信号不会只命中尚未安装处理器的启动窗口；“线程已创建”不等于文件已打开。测试 driver 的完整已知事件场景由 WriterGoalObserver 发布目标，`ServiceHooks::coordinator_poll(const Metrics&)` 在子进程主线程 acquire 观察目标并核对必要的接收/拒绝计数后，一次性 `raise(SIGINT/SIGTERM)`，实际走同一处理器和正常停止逻辑。父进程的 XPUB 先绑定端点，配置子进程地址，收到订阅通知后才发冻结计划，并保持 socket 到场景结束。

driver 不在安装 SignalScope 前自行创建发布线程；发布者留在父进程，避免额外的未屏蔽线程接收子进程的停止信号。Hooks、Observer 和它们引用的数据在 RunServiceImpl 调用前创建，所有工作线程结束后才销毁。运行时屏蔽、主线程解除屏蔽及恢复原 mask/action 的约定沿第 3.3 节执行。

**5. 构造、初始化与释放。** 构造只准备本用例资源；Start 失败不留下可误认为有效的 PID。析构是兜底清理，不把超时终止当作正常保存。父进程发布者、管道和文件读回资源也必须在作用域离开前结束，相关错误不能被第二次清理覆盖。

**6. 关键流程。** 正式 qbook 的空闲停止用例核对真实配置加载、信号和五个零行可读文件；driver 的网络已知序列用例核对相同 Service 内部协调、实际三阶段、目标后信号和闭合文件。两类证据分别登记，不能把带私有观察的测试 driver 称为未修改入口的黑盒测试。

**7. 实现顺序。** 先让极小子程序正常退出和返回非零，验证管道与reap；再接正式qbook空闲信号，随后接带目标观察的driver，最后加入启动/写入故障与期限清理。场景不依赖固定sleep猜测准备完成。

#### 3.4.3 模块内部协作与调用流程

| 测试层次 | 实际执行路径 | 独立的完成与正确性证据 |
|---|---|---|
| 文件组件 | 固定输入 → ParquetWriter → 显式Close。 | Reader检查五schema、所有行、列表与元数据。 |
| Writer组件 | 唯一测试生产者 → RecordQueue → Writer。 | 成功push账本、Writer join、Reader；流Probe检查资源线程。 |
| 进程内真实网络 | XPUB → Feed → Book → Writer。 | 订阅ready、Writer目标观察、完整接收/拒绝条件、顺序join和读回。 |
| 精确排空 | 唯一受控生产者 → FeedQueue → Book → Writer。 | 成功push后记账，普通停止后按类型与文件逐条比对。 |
| 隔离进程 | 正式qbook或私有driver，外部XPUB仅位于父进程。 | 实际信号、退出码、已回收子进程及独立文件读回。 |

V2 Collector 不与真实 Writer 同时消费 RecordQueue。V3 的观察点位于 Writer 已取得并追加记录之后，不增加第二个队列消费者，也不在测试主线程调用队列 size/empty 来猜测积压。

#### 3.4.4 模块关键算法的实现方案

**接纳账本与五文件比较。** 受控生产者在线程启动前预留有限账本，仅在 FeedQueue.try_push 成功后记录事件；每次重试观察自身普通stop和Failed。结束生产者并join后，账本不再变化。Writer关闭后把账本按四种原事件类型稳定分组，与各文件逐行比较；快照单独核对。这样保持同类型顺序，不人为给五个文件增加原本不存在的全局序号。

**输出积压。** 先用 Writer 的成功追加观察点与流闸门确定文件处于可写运行阶段，再让实际Write暂停，使用小容量两条队列和有限输入形成背压。正常结束先让输入生产者取消未接纳事件并join，再释放写入闸门，随后Book、Writer按依赖顺序排空。Fatal场景让流返回错误，所有等待观察Failed后结束；不对失败文件提出完整记录断言。

**完成信号与文件事实分开。** WriterGoalObserver只说明某些Add已成功；即使目标已经发布，尾批次或Close仍可失败。最终成功必须同时满足正常退出、显式Close成功及Reader逐字段结果。网络计划尾部的非法帧还要确认接收和拒绝目标，不能因为合法记录已齐就立即停Feed。

#### 3.4.5 模块并发与同步实现

普通结果容器、调用轨迹和接纳账本都只有一个写者，并在join后读。目标观察使用专门的release/acquire信号；各Metrics字段仍保持原有relaxed语义。流控制只能触发预先限定的故障或闸门，不允许协调者越过Writer直接Close文件。任何用例的清理路径都要解除测试闸门，并让所有可能调用Fail的生产者、发布者和阶段结束后再读取RunControl载荷。

私有hooks的类型与friend声明在所有编译单元保持相同；不通过测试宏改变生产类布局。正式构造不接入回调，公开配置无测试选项。观察器和故障代码只编译进测试目标，生产库只保留必要的窄接入位置；这些是实现可验证性的细化，不是线上插件机制。

#### 3.4.6 模块错误处理与恢复方案

任何缺失目标、Reader差异、注入点未触发、资源释放线程不符或异常退出都使场景失败。保存首条相关错误、触发位置和最小输入，修正后重跑；不通过重发消息、跳过差异行或延长无限等待来制造成功。发生错误后允许文件不完整，仍须确认没有覆盖旧运行目录、没有残留未回收子进程、共享对象寿命正确。

Open/Write/Close故障夹具不等于真实断电、文件系统全面故障或全部分配失败覆盖。Reserve、Finish、构造中间步骤等另按第 3.1 节的对应注入或受控资源检查验证；没有执行的分支写入未验证项。生产库不为了这些测试引入自动批次重试或输出修复行为。

#### 3.4.7 模块实现顺序

先做独立读取器和最小文件用例，再做透明流包装与故障，随后接真实Writer和目标观察。复用V2发布者完成进程内网络链路，再加入受控账本与双队列积压。最后建立ProcessHarness，验证正式入口和私有driver信号场景，完成第5章规定的全套回归及证据记录。

#### 3.4.8 模块测试与完成条件

五类输出由独立 Reader 核对；成功入队账本与正常关闭后的原事件逐条对应。网络场景检查 Mbo/L2、业务忽略、输入拒绝、周期快照和时间；受控写入故障检查背压取消与资源释放。相关用例已在完整 Debug 和 Release 文件/服务检查中通过，验证边界见第 5.5 节。

## 4. Implementation Sequence（编码顺序）

### 4.1 模块实现先后顺序

| 顺序 | 进入条件 | 实施动作 | 可检查的阶段结果 |
|---|---|---|---|
| 前置核对 | 真实仓库与固定依赖可访问。 | 核对 V0–V2 代码/基线，确认 Arrow/Parquet/ZSTD 接入，登记已有目录。 | 依赖和前置缺口被明确识别，没有把计划当完成证据。 |
| V3-01 | 共享记录契约稳定。 | 固定 schema/元数据及独立读回 → 排他文件与部分构造清理 → Add/批次 → Close/Failed。 | 同步写入五类记录，正常关闭后逐字段读回；打开、写批、关闭故障可报告。 |
| V3-02 | 文件组件通过自己的必要检查。 | 独占目录 → Writer Open/Consume → 计数 → 正常排空 → Fatal及线程内清理。 | 实际 RecordQueue 输入保存为文件；旧运行不变，主线程不替 Writer 关文件。 |
| V3-03 | V2阶段和V3 Writer可用。 | 私有服务目标 → 信号守卫 → 真实线程包装 → 正常顺序结束 → 部分启动/运行失败。 | 正式 qbook 可从固定配置启动，用两种信号正常结束，失败非零。 |
| V3-04 | 组件边界已有证据。 | 有限网络与同步观察 → 接纳账本 → 业务/原事件/快照对照 → 子进程信号 → 全套与必要Release回归。 | 从输入到关闭文件的结果可复验，失败运行不会混入正常完成记录。 |

读取器、故障流和寿命清理随需要它们的任务一起实现。独立 ParquetWriter 可以提前研究，但 V3 整版仍依赖 V2 的真实接纳与失败退出能力；不要因一个文件组件通过就跳过完整链路。

### 4.2 类与关键函数的实现依赖

| 实现点 | 必须先具备 | 为什么按这个顺序 |
|---|---|---|
| OpenOrders 等 | schema、元数据、排他输出和 Sink 清理。 | 构造失败发生在对象可用之前，资源不能靠尚未完成对象的析构补救。 |
| Append* / Commit | 对应类型 builder、Reserve 和确定的列顺序。 | 只有一整行完成后才能推进 rows，避免半行被当作完整记录。 |
| Flush | Finish 检查、RecordBatch/Table、真实 FileWriter 与失败状态。 | 写入可能已接受一部分数据，失败后不能重新尝试同一批。 |
| Close | 所有正常 Flush 路径及不抛资源清理。 | 正常保存和失败释放是两条不同路径；前几文件成功不代表全成功。 |
| Writer Run | ClaimRunDirectory、Open/Consume/Close、RunControl。 | 开始取记录前必须有完整输出组件；退出前必须处理所属线程资源。 |
| RunService | 三个真实阶段、包装器、完成位、信号守卫。 | 主线程负责依赖寿命与退出次序，不能依赖对象逆序析构碰巧正确。 |
| 网络完整验收 | XPUB 就绪、WriterGoalObserver、独立读取器。 | 先知道已发生的业务处理步骤，结束后再检验实际文件；两项证据不互相替代。 |
| 信号与启动故障验收 | ProcessHarness、同一 ServiceImpl 的私有驱动。 | 不让测试污染父进程信号，也不另写一套假服务证明自己正确。 |

实现代码应在当前职责处完成校验：协议问题在 Feed，业务忽略在 Orderbook，列形状和文件错误在 ParquetWriter，进程结果在 RunService。不要把所有检查移到 main，也不要让每层重复过滤一次业务事件。

### 4.3 分阶段可运行结果

1. **V3-01 后：** 可在独占临时目录直接创建 ParquetWriter，追加固定记录、显式 Close，再运行读取器核对五个文件；无需启动 Feed。
2. **V3-02 后：** 唯一测试生产者向 RecordQueue 放入有限记录，真实 Writer 排空保存；运行两次得到不同目录，第一份数据保持不变。
3. **V3-03 后：** 构建产生真实 `build/debug/qbook`，从自己的工作目录读 config/config.yaml；没有行情时也可用 SIGINT/SIGTERM 得到五个正确零行文件。
4. **V3-04 后：** Mbo 和 L2 的真实三阶段网络链路均可关闭并读回；精确接纳和积压通过受控账本核对，写入故障使全链路退出。正式主程序信号与带数据私有驱动信号分别有明确结果。

上述能力已由本轮代码和对应测试落实。普通用户运行正式 qbook；测试驱动只用于复用同一服务实现的受控验证。示例发布与读回使用现有测试命令，具体入口见 README。

### 4.4 实现中的主要风险与处理方案

| 风险 | 实现处理 | 验证方式 |
|---|---|---|
| 目录名看似唯一，文件却被截断。 | 目录排他创建，文件 O_EXCL 创建，禁止 exists 后覆盖式打开。 | 确定性候选碰撞、同名哨兵文件及连续运行。 |
| 列顺序错位但总行数正确。 | 每类明确 schema/Append 配对，独立读回预期。 | 为相邻字段设置不同值，逐列逐行比较。 |
| 固定列表退化或整数变浮点。 | 保存 Arrow schema，固定类型和子值，保留倍率元数据。 | 跨批/多块读取，检查逻辑类型、深度、null 和64位精确值。 |
| 满批只进入库缓冲，却误称已写文件。 | 明确 WriteTable 调用边界，仍要求显式 Close。 | 实际写调用/读回与关闭结果；不把 row group 数等同应用批次数。 |
| 部分 Finish/Write 后重试导致重复。 | 工作异常使整体 Failed；无补半行或重播批次。 | 实际 I/O 故障轨迹与异常分支审阅分别记录。 |
| 文件在主线程析构才被关闭。 | Open前安装清理守卫，Run的所有路径在Writer线程reset。 | 独立Probe记录线程，阶段join后再销毁外壳。 |
| 下游失败导致上游永久等空位。 | Failed优先，所有入队重试可取消；故障不承诺排空。 | 真正写入被闸门阻塞后返回错误，同时核对全阶段结束。 |
| 普通stop漏掉最后一条记录。 | 上游join之后才停下游；先读stop再确认空。 | 最后一次成功push交接与积压账本逐行比较。 |
| 信号处理器操作复杂对象。 | 只有协调主线程接收，处理器仅设sig_atomic_t。 | 隔离进程kill/raise场景，核对线程屏蔽与普通清理路径。 |
| 部分线程创建失败提前销毁共享资源。 | 第一条线程前建立清理守卫，所有实际句柄join后释放。 | before_start在不同位置抛错，复用正式协调逻辑。 |
| relaxed指标混读误判最终快照。 | Writer同线程观察实际record，release/acquire发布测试目标。 | 精确目标之后的快照再与关闭文件及独立业务期望比较。 |
| 测试管道或暂停闸门自己造成死锁。 | 父持续读管道，闸门有取消/失败出口，期限后回收自己的子进程。 | 超时与失败清理场景明确失败，不算正常关闭通过。 |

## 5. Verification（验证与验收）

### 5.1 单元测试

验证覆盖单条文件读写、队列消费、线程资源和正式进程。下表编号表示行为检查组，与 Catch2 用例数量不同。完整构建实际注册 154 项，其中前置回归 115 项、storage 18 项、service 21 项；执行结果见第 5.5 节。

| 编号 | 任务与计划测试文件 | 输入或操作 | 必须确认的结果 |
|---|---|---|---|
| V3-UT-01 | V3-01；`tests/parquet_writer_tests.cpp` | 五类固定记录，各字段使用能区分相邻列的值。 | 读回列名、顺序、类型及每个值符合独立预期；价格、数量、ID和时间保持整数，不经过 double。 |
| V3-UT-02 | V3-01；同上 | 带非空本机 prev/next 的独立订单输入。 | 文件没有指针列；Add 不修改原输入的任何字段；不能用结构体整体 memcmp 比较 padding。 |
| V3-UT-03 | V3-01；同上 | 空侧、双侧和不同品种的 book。 | 四个价格/数量列读回为长度 1 的 fixed_size_list<int64>；空侧保持 0/0，父列和子值无意外 null。 |
| V3-UT-04 | V3-01；同上 | 每类文件打开后不 Add，显式 Close。 | 固定五文件均存在、能正常读出零行与准确 schema，不能只检查文件大小或文件名。 |
| V3-UT-05 | V3-01；同上 | batch_rows=3，各类型分别写 1、2、3、4、7 行；另测 batch_rows=1。 | 少于一批、恰好一批和跨批次均逐行准确；尾批不丢、不重复，各 Sink 的批量互不干扰。 |
| V3-UT-06 | V3-01；同上 | 交错追加五类记录，同类记录使用严格递增的测试标识。 | 每个文件保留其类型的消费顺序；不要求从五文件恢复原全局交错顺序。 |
| V3-UT-07 | V3-01；同上 | 检查关闭文件的 schema 元数据和 Parquet footer。 | 格式版本、price_scale、qty_scale、snapshot_depth、枚举含义可识别；Arrow schema 已保存；有数据列块采用 ZSTD。 |
| V3-UT-08 | V3-01；同上 | Close 成功后再次 Close，再尝试 Add。 | 第二次 Close 无操作且文件不改变；Add 被明确拒绝，Closed 不重新变成 Open。 |
| V3-UT-09 | V3-01；同上 | 非正批量、容量乘法越界、输出目录无效、预先存在同名文件。 | 在不安全运算或覆盖发生前失败；既有文件内容不变，不用先 exists 再截断来假装排他创建。 |
| V3-UT-10 | V3-01；同上 | 在第几个文件打开时注入失败。 | 构造失败，先前资源在构造执行线程清理，未留下可继续 Add 的半成品；允许保留失败目录用于诊断。 |
| V3-UT-11 | V3-01；同上 | 实测容量数值边界、受控 WriteTable 底层流错误，并审阅 Append/Reserve/Finish 的状态检查路径。 | 写入错误实际转入 Failed并传播；所有Arrow失败分支均禁止继续追加/重试。内部真实内存耗尽未单独注入时明确记录证据限制，不把它算作已测通过。 |
| V3-UT-12 | V3-01；同上 | 第一个尾批写入或文件结束/流关闭失败，其他资源也需释放。 | Close 尽力清理所有已打开资源并保留首错；不能因其余文件可读而宣称整次成功。 |
| V3-UT-13 | V3-01；同上 | Open、Closed、Failed 对象的析构；外层正在展开另一个异常。 | 析构不抛第二个异常，不调用失败批次重试；只有显式 Close 成功才是正常完成证据。 |
| V3-UT-14 | V3-02；`tests/writer_thread_tests.cpp` | 小容量 RecordQueue 中放入五类有限记录，生产者结束后普通停止。 | 唯一 Writer 消费全部记录一次，尾批写完，join 后五文件可读。 |
| V3-UT-15 | V3-02；同上 | 连续两次运行；固定候选名发生碰撞；输出根本身是普通文件。 | 成功运行目录互异，旧文件字节保持不变；真正目录冲突可换候选，其他文件系统错误不能无限重试。 |
| V3-UT-16 | V3-02；同上 | 在测试流记录 Open/Write/Close/析构调用的线程标识。 | 文件调用都属于 Writer；Run 返回时 writer_ 已空，主线程随后析构阶段对象不再关闭文件。 |
| V3-UT-17 | V3-02；同上 | Add 成功与失败分别触发；订单、撤单、成交、价位、快照混合。 | 只有 Add 成功才增加追加指标；orders 包含撤单但两个文件分开；计数不等于 fsync 或断电安全。 |
| V3-UT-18 | V3-02；同上 | 先完成最后一次 push 并 join 生产者，再请求 Writer stop。 | Writer 在观察 stop 后再确认空，不漏掉末条；不能先 pop 得空再读新 stop 就返回。 |
| V3-UT-19 | V3-02；同上 | Writer 空等、批量错误、普通排空期间 Close 错误。 | Fatal 可结束等待；错误路径清空资源并上报失败，不把普通停止请求当作返回 0 的充分条件。 |
| V3-UT-20 | V3-03；`tests/service_lifecycle_tests.cpp` | 配置文件缺失，以及存在但不可读、错误 YAML、零容量或非法字段类型。 | 缺失文件按 V0 约定警告并采用已验证默认值；存在但错误的配置在线程前返回非零，不能把读取/类型错误静默当作缺失。 |
| V3-UT-21 | V3-03；同上 | 指定第 k 次线程创建前受控抛 system_error。 | 已创建线程全部取消并 join；没有创建的线程不 join；共享资源最后销毁，错误详情在所有潜在 Fail 写者结束后读取。 |
| V3-UT-22 | V3-03；同上 | 独立真实 qbook 子进程分别收到 SIGINT、SIGTERM。 | 走主线程有序停止，正常 exit 0，输出显式关闭可读；不能仅以“进程消失”判成功。 |
| V3-UT-23 | V3-03/04；`tests/service_integration_tests.cpp` | 网络初始化失败、真实文件打开/写入/关闭失败。 | 所有阶段结束，结果非零；首错未被清理异常覆盖，不把失败目录标记为完整采集。 |
| V3-UT-24 | V3-04；同上 | Mbo/L2 网络与受控队列完整场景。 | 原事件、最终/周期快照、业务忽略、接纳账本、批量边界和正常停止分别有可读文件证据。 |

**读取结果必须独立于写入映射。** 读取辅助在关闭后使用 Arrow/Parquet reader 取得真实 schema、表及 footer；预期列名、整数类型、列序、列表长度和业务值由固定测试常量独立列出，不调用生产 Open*/Append* 来生成期望。按列名和预期位置交叉检查，然后按行比较；chunked array 可能包含多个块，读取辅助应正确遍历或显式 CombineChunks 并检查状态，不能只读第一块。订单应有 11 列、成交 9 列、撤单和 L2 各 7 列、快照 7 列；五文件中都不出现 prev/next。

保存 Arrow schema 后，读取器需要按选定依赖版本启用对应恢复路径；同时核对 Arrow 类型和 Parquet 物理/逻辑类型。仅看到 Parquet 中有 LIST 注解不能证明固定长度 1 已保留。零行文件没有数据列块时不强行断言不存在的压缩字段；ZSTD 检查使用至少含一行的样本。批量分组检查依据本版实际 WriteTable 调用与 row-group 配置，不把文件总行数正确当作列类型正确，也不以全部文件都存在替代可读性。

**失败注入不替代真实文件路径。** 复用第 3 章的私有测试接缝，在真实文件输出流外包一层有限计数的故障流；在指定 Open/Write/Close 调用返回 Arrow I/O 错误，并记录已执行动作。生产公开构造与运行参数保持不变。构建器容量乘法与表示边界实际测试；Append/Reserve/Finish 的 Status 检查、catch后Failed和禁止重试路径需代码复核。当前基线不要求逐个内部分配点制造内存耗尽，未单独注入者明确记为未验证，不以流错误冒充。只有实际暴露相关风险时才加入最小私有 MemoryPool 测试接缝；不将 V1 的全局 new 替换链接进整个网络/Arrow 测试程序，也不建立通用故障框架。

测试友元只在单所有者操作或 join 后查看状态；不在工作线程运行时读取 writer_、run_dir_、builders 或普通 trace vector。运行中的观察使用明确的测试同步信号，最后通过 join 读取详细结果。阶段中的 Catch2 断言不得越过线程边界；包装器保存失败，由协调线程在完成清理后统一断言。

### 5.2 集成测试

**常规构建。** 正常修改与回归复用已有 `build/debug`。本轮已核对 V0～V2 源码和固定依赖，前置基线 115/115 通过。下列命令从仓库根执行；首次完整构建前先运行 `python3 scripts/install_arrow_dependencies.py` 准备固定安装。实际执行结果见第 5.5 节。

```bash
c++ --version
cmake --version
ninja --version

cmake -S . -B build/debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DQBOOK_BUILD_PIPELINE=ON \
  -DQBOOK_BUILD_PERSISTENCE=ON \
  -DQBOOK_BUILD_SERVICE=ON \
  -DQBOOK_BUILD_BENCHMARKS=OFF \
  -DQLIB_BUILD_TESTS=OFF
cmake --build build/debug --parallel
ctest --test-dir build/debug --show-only
ctest --test-dir build/debug --output-on-failure --no-tests=error
```

测试发现列表必须同时包含前置核心/协议/阶段回归、本版 `qbook_storage_tests` 的 `storage::` 和 `qbook_service_tests` 的 `service::` 场景；`qbook_service_test_driver` 是场景使用的子进程驱动，不是生产入口的替代品。只生成 qbook、只执行旧测试、CTest 无用例或仅运行 show-only 都不算 V3 验收通过。记录实际包版本、CMake 导出目标和动态/静态链接方式；不能从文档示例推断目标机器已经安装对应依赖。

**三层集成各自提供证据。**

| 层次 | 实际运行路径 | 主要证据 | 不应推导出的结论 |
|---|---|---|---|
| 网络文件链路 | 有限 XPUB → 真实 Feed → Book → Writer → Parquet。 | 订阅就绪后冻结输入的接收、筛选、原事件保存及业务快照。 | PUB 发送完成或解码完成不等于任意输入已接纳。 |
| 受控队列文件链路 | 唯一测试生产者 → FeedQueue → 真实 Book → Writer → Parquet。 | 每次成功 push 的账本与正常关闭文件逐项对应，覆盖积压排空。 | 该场景不证明网络可靠投递或真实行情源兼容。 |
| 正式进程生命周期 | 独立 cwd/config 下真实 qbook，父测试进程只发送信号并读结果。 | 配置入口、信号处理、退出码、阶段清理与关闭文件可读。 | 启动日志和 exit 0 不能独自证明某个网络样本集合已完整保存。 |

网络发布者继续使用 V2 的 XPUB 订阅通知及临时 TCP 端点：先发布实际地址，再启动 Feed，等空主题订阅通知到达才发送有限唯一消息。socket 在全部发送调用结束后仍保留，直到协调者确认必要完成条件并请求停止。所有 future 等待都有期限和失败检查；不使用固定 sleep 猜测订阅建立，不重复发送业务事件来掩盖偶发缺失。

网络正常场景以 Writer 私有测试 hooks 的 `after_add(const record&)` 观察为结束条件。真实 Add 与对应 Metric Add 均成功之后，Writer 所有者在回调中累计各类原事件；到达目标后，只有随后实际看见匹配品种和预期字段的快照才以 release 发布 goal，协调者 acquire 读取。这样不需要从多个 relaxed 指标猜测“这是最后一事件之后的快照”，也不增加生产审计接口。含非法帧的计划还需等待 feed_messages、feed_dropped 到达该有限计划要求。完成信号只允许开始正常结束，不是文件已落盘证明；进程内 fixture 最终依次停止并 join 发布者、Feed、Book、Writer，再读取正常关闭文件。尾部拒绝帧也必须确实经过筛选，不能合法输出先达标就停止 Feed。

**Mbo 完整场景。** UC-02 的六步固定为：买 B1 100/10，买 B2 100/5，B1 同价修改 new_qty=6，B2 撤 2，卖 S1 101/8，B1/S1 成交 2。输入使用测试原始整数单位，末态买一 100/7、卖一 101/6。用独立期望检查以下内容：

| 检查点 | 正常关闭后的预期 |
|---|---|
| 文件行数 | orders 4 行、cancels 1 行、trades 1 行、levels 0 行；books 有目标周期快照。 |
| 原始字段 | modify 的 new_qty 仍为 6，cancel 的 qty 仍为 2，trade 的 qty 仍为 2；不能写成最终挂单剩余量。 |
| 最终状态快照 | 明确在六条原事件之后观察到的同品种快照为买一 100/7、卖一 101/6；event_ns/recv_ns 对应最近处理事件。 |
| 同步后的指标 | Feed 六条消息、orders 5、trades 1；Book events 6、samples 1；Writer orders 5、trades 1、levels 0，books 与文件实际行数对应。 |
| 解释边界 | Writer orders 含撤单，但文件分别存放；指标需在 join 后联合比较，未同步的多个 relaxed 值不是一致快照。 |

**L2 完整场景。** 价格 100 的绝对总量依次设 20、8，插入成交 qty=3，再设 0。三个 level 和一个 trade 原样保存，状态依次为 20、8、8、空侧；成交不二次扣减，订单及撤单文件为零行。为避免四条连续输入太快而看不到中间周期，使用两个独立有限运行：第一个只发前三条，观察并读回成交之后总量仍为 8 的快照；第二个发送完整四条，观察并读回删除后的空侧。两次各用独占目录，不要求恰好在两条网络输入之间触发定时器。错误模式的订单/撤单和损坏帧明确被拒，不出现在五类业务行中，不创建额外品种。

业务忽略另设场景：重复新增、未知修改/撤单、市价、方向冲突和合计溢出等已通过入站筛选的事件仍进入对应原事件文件。检查原事件保留、业务状态不被错误改变、最近处理时间更新，且 feed_dropped 不因这些状态性忽略增加。多品种可使用相同 order_id；列中的 instrument_id 保持各自归属。暂停发送后继续等待已知品种的后续周期快照，核对时间没有变成生成时间；未知品种不虚构快照。

**接纳账本与积压停止。** 受控生产者事先分配有限输入及账本空间，只在 try_push 成功后记录输入副本/唯一标识；每次重试检查自己的普通 stop_token 和 Failed。此时没有 Feed 同时写队列，账本只由生产者写，join 后才能读取。生产者在成功入队后不依赖新的内存分配才能保留证据。记录队列只由 Book 写、Writer 读，协调者不调用活队列 size/empty/try_pop 来观察积压。

通过有取消出口的测试流闸门暂缓 Writer 的一次实际写入，用小队列和有限数据形成积压。普通停止先 request_stop/join 唯一生产者，使接纳集合固定；随后解除写入闸门，让 Writer 继续消费，再 request_stop/join Book，最后 request_stop/join Writer。按类型筛选账本与五文件中的原事件逐条比较，确认无丢失、无内部重复；快照单独核对，不塞进输入接纳账本。对最后一次成功 push，消费者必须先看到上游结束后的 stop，再确认队列空，防止错误的空判断遗漏末条。

**真实进程信号用例。** 父测试进程先建立本次独占的测试目录，写入它自己的 config/config.yaml，使用构建系统提供的 qbook 可执行文件绝对路径启动子进程，不修改仓库现有配置。子进程 cwd 指向该目录；父进程持续读取有上限的日志管道，避免无人读管道反过来阻塞日志。等到 `service_control_ready`、`writer_opened`、`feed_connected` 的实际条件成立后，在无业务输入的场景分别发送 SIGINT 和 SIGTERM，检查正常 exit 0、五个零行文件可读及没有未结束子进程。这三个日志分别表示协调停止已启用、文件已打开、连接配置已完成，不表示行情已到达。信号只发送给被测试的子进程 PID，不发送给 Catch2 自身或整个用户进程组。

有数据并经真实信号结束的闭环使用独立 `qbook_service_test_driver`：其内部 ServiceHooks 只供测试，WriterHooks 的 after_add 先发布上述真实目标；协调者的 `coordinator_poll(const Metrics&)` 以 acquire 观察完成信号，并只读本次原子指标确认所需接收/拒绝条件，然后在本进程主线程一次性触发指定 SIGINT 或 SIGTERM，随后由同一个正式协调逻辑处理停止。网络发布者在父进程中先绑定并把端点写入子进程配置，保持连接直至测试收尾；子进程退出后才读 Parquet 并比对固定输入。通过此方式同时验证事件→业务→Writer→信号→关闭文件，不让生产 qbook 增加测试参数。父测试程序采用 fork 后只执行预先准备好的安全设置并立即 exec；不能在已多线程的父进程 fork 后继续执行任意 C++ 测试代码。子进程 exec 后不在建立信号屏蔽前另外创建发布线程。

配置错误、非法端点和输出根为普通文件等进程场景必须非零退出，保留真实错误原因。部分启动和内部写入故障可通过专用测试驱动调用同一个内部装配逻辑；生产 qbook 不新增故障命令行参数或环境开关。测试驱动结果与真实 qbook 信号结果分别记载，不能把驱动说成生产可执行文件的实测。进程超时由测试先记录失败并尝试正常清理，最后才强制结束这个测试子进程；被强制结束的输出不进入正常可读性通过结果。

**成功标准与最小记录。** 构建成功，发现并执行本版实际用例；正常文件逐项符合独立期望，所有已启动线程/子进程结束，故障场景按约定非零且未伪装完整。`docs/releases/V3.md` 只记录必要环境与依赖标识、完整命令、实际用例数及结果、固定输入/接纳基准、各文件行数与关键字段、退出结果、未执行项和清理情况。成功编译日志、依赖下载归档和重复报告可以重新产生，不长期保存。

**只为变化的构建条件增加两次隔离检查。** V3 引入文件依赖和正式服务，除常规目录外，在最终阶段各运行一次以下配置。第一项验证关闭链路与持久化后不会查找 Arrow、Parquet、cppzmq；第二项满足本版优化构建下的文件/生命周期功能回归。它们不是额外性能实验，也不要求每次小改动都重建。

```bash
cmake -S . -B build/v3-check-core -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DQBOOK_BUILD_PIPELINE=OFF \
  -DQBOOK_BUILD_PERSISTENCE=OFF \
  -DQBOOK_BUILD_SERVICE=OFF \
  -DQBOOK_BUILD_BENCHMARKS=OFF \
  -DQLIB_BUILD_TESTS=OFF
cmake --build build/v3-check-core --parallel
ctest --test-dir build/v3-check-core --output-on-failure --no-tests=error

cmake -S . -B build/v3-check-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DQBOOK_BUILD_PIPELINE=ON \
  -DQBOOK_BUILD_PERSISTENCE=ON \
  -DQBOOK_BUILD_SERVICE=ON \
  -DQBOOK_BUILD_BENCHMARKS=OFF \
  -DQLIB_BUILD_TESTS=OFF
cmake --build build/v3-check-release --parallel
ctest --test-dir build/v3-check-release --output-on-failure --no-tests=error \
  -R '^(storage::|service::)'
```

`QBOOK_BUILD_SERVICE=ON` 必须显式同时启用 PIPELINE 与 PERSISTENCE；错误组合应给出配置错误，不能偷偷改其他开关。检查 CMake 分支确实只在需要时寻找相应依赖，`BUILD_TESTING=OFF` 不加入 Catch2/测试驱动；不通过残留的旧可执行文件判断新配置有没有目标。若实际改动导致这一分支存在具体疑问，可在已记录的 Release 检查目录内改为 `BUILD_TESTING=OFF` 重配置并检查生成目标，记录额外命令，不再建立无理由的测试矩阵。

**失败材料与清理。** 保存失败命令、用例、首错阶段和消息、最后已确认的输入/追加目标、最小重放数据及必要堆栈；文件失败保留相关目录和必要调用轨迹，未确认的行数不能补成成功。修复后复测受影响场景，再执行本版必要回归。

实施前登记本次创建的目录和输出。验证记录完成后，逐项清理本次新建的两个隔离构建目录、测试临时 cwd、成功运行数据及其他可再生中间产物；`build/debug` 若也是本次新建则最终清理，若开始前已有则保留。已有同名目录不得直接覆盖/删除，应保留并为本次检查使用另一个明确登记的临时名。保留源码、测试、固定样本与期望、版本锁和许可证、手写文档、必要失败证据及用户已有修改，不用通配符或 git clean 清理未知目录。

### 5.3 边界与异常测试

**状态与列缓冲。** 在 Open 状态中覆盖空批次和每个批量边界，确认五个 Sink 的行数分别推进；不能用一个全局 rows 让订单触发成交文件错误刷新。列完成或写入失败可能使某些 builder 已被清空、文件已接受部分字节，因此测试要求 Failed 后禁止业务追加和批次重试，不要求回滚到还能继续采集。成功 Close 幂等；失败 Close 可继续尽力释放资源，但后续调用不能重新报告“之前成功”。

**故障矩阵。**

| 场景 | 可控方式 | 检查要点 |
|---|---|---|
| 排他创建冲突 | 预建确定候选目录或同名 Parquet 文件。 | 目录层只在实际存在冲突时换名；文件不能被截断；有限候选耗尽明确失败。 |
| 文件打开失败 | 使用根路径为文件、缺失/无效父路径，或在指定第几个流打开时返回失败。 | 启动失败清理全部已有资源，不留可继续运行对象；权限测试须在真实有效权限下证明失败，不能 root 下 chmod 后假定不可写。 |
| Append/Reserve/Finish 失败路径 | 实测容量边界，复核各Status与异常分支；内部内存耗尽未实际注入则如实登记，仅遇具体风险再定向注入。 | 路径进入Failed、保留首错并禁止Add/补半行；区别代码检查与实际故障证据，不要求无依据的全局事务回滚。 |
| 批次写入失败 | 包装真实输出流，在已完成前置调用后使指定 Write 返回 I/O 错误。 | 真正经过 Arrow/Parquet 写入路径；不增加本次失败 Add 的成功计数，不自动重试不明批次。 |
| 正常停止时尾批失败 | 少于一批，然后在 Close 的 Flush 注入写入错误。 | 普通 stop 不能掩盖失败；进程/装配结果非零，目录不标为完整。 |
| 多资源关闭失败 | 一个 writer/流先失败，后续清理也失败。 | 尽力清理各资源，保留最先错误；析构 noexcept，清理不覆盖原异常。 |
| RecordQueue 满时 Writer 失败 | Writer 在真实写调用的可控闸门等待，有限上游继续推进，再由该调用返回失败。 | Writer 包装器 Fail，Book Forward 和 Feed/受控生产者的满队列等待能退出；所有实际线程 join。 |
| 指标溢出 | 启动前给 Writer 某个 Metric 设临界值，再完成真实 Add。 | Add 可能已改变文件缓冲，而 Metric Add 失败导致整体 Fatal；不假定记录与指标事务回滚。 |
| 部分线程启动失败 | 内部启动边界按指定次数抛 system_error。 | 未启动、仅下游启动、下游与 Book 启动等位置均清理正确，不为触发故障耗尽机器线程。 |
| 场景等待超时 | 故意缺少目标或保留一个可取消测试闸门。 | 登记失败，释放/取消闸门，停止并 join；测试保护期限不成为产品停止 SLA。 |

制造输出满的定向用例应排除周期快照干扰：使用长于场景期限且已安全校验的周期，或采用单所有者 Book 受控步骤。测试记录真实写调用已经进入闸门，随后用成功入队账本与阶段进度解释为何输出容量已耗尽；不能只写“等待一会儿，大概满了”。闸门自身必须检查测试取消/失败和期限；否则注入工具会制造一个生产停止逻辑无法解除的假死。普通排空前先放开闸门，Fatal 也必须能解除它。

RunControl.Failed 仅用于取消；所有可能调用 Fail 的线程，包括测试发布者、受控生产者和辅助驱动，全部 join 后才能读 Stage/Error。多个原子指标也不形成一致快照：运行期间可等待某个进度值，却不能据另一字段的新值假定普通数据、另一计数或文件已经完成。最终行数/计数联合比较在正常 Close 和 join 后进行。

本轮场景期限使用 10 秒；CTest 的既有回归为 30 秒、storage 为 45 秒、service 为 60 秒。新增测试包含多个生成参数和子进程，较长的进程保护覆盖同一用例内全部子场景。100ms 接收超时、50us 入队退避和 Writer 空闲等待均不是停止 SLA。存储系统调用可能阻塞，受控故障测试证明应用层取消与清理，不承诺抢占内核 I/O。

只有发现具体悬空引用、数据竞争或不能解释的资源错误时，再增加对应的 ASan/UBSan/TSan 隔离配置；记录外部库是否插桩及工具限制。不要为文档中的每个场景生成一套重复构建目录，也不要把诊断工具通过替代线程所有权与文件内容核对。

### 5.4 性能测试与测量方法

V3 的功能验证不设置吞吐或延迟达标数值，也不把文件大小、一次采集耗时或 writer_* 增量当作完整性能成绩。当前检查批次机制和资源边界，为 V5 的基准实验留下真实可测链路。

| 路径 | 本版检查 | 当前不能声称 |
|---|---|---|
| 普通 Add | 固定字段按列追加，满批之前没有逐条 WriteTable。 | 每条 Add 都是常数耗时或已写到稳定介质。 |
| 满批与 Close | 批量边界明确，尾批保存，ZSTD 和实际 I/O 错误被检查。 | 压缩/磁盘延迟有硬上界，Close 等同于断电安全。 |
| 内存 | 每类一份 builders、固定队列，没有额外无界 record 列表。 | 全进程内存恒定；Arrow、ZMQ和订单簿也有额外占用。 |
| 指标 | Writer 四字段在 Add 正常返回后更新，Book 抽样口径保持 V2。 | writer计数是 fsync 数、抽样均值是持久化端到端 P99。 |
| 实验边界 | 功能测试只说明行为正确；有稳定异常慢路径才记输入规模、批量、介质并定位。 | 本版通过就已经满足未定义的性能目标。 |

批量大小、压缩、文件介质和输入比例的比较，以及预热、重复、Release 基准、perf/FlameGraph、吞吐分母和延迟分布，统一留给 V5。相关 Release 功能检查只验证优化构建下行为正确，不冒充性能实验。

### 5.5 模块实现验收

| 任务 | 必须取得的实际证据 | 当前状态 |
|---|---|---|
| V3-01 | 五文件准确schema/值/元数据、定长列表、ZSTD、各批量边界、零行、Close幂等，实际Open/Write/Close故障及内部状态路径检查。 | 已实现；Debug/Release 文件检查通过。 |
| V3-02 | 唯一消费者、独占目录、追加计数、末条排空、全部路径工作线程清理、主线程析构无文件操作。 | 已实现；Debug/Release Writer 检查通过。 |
| V3-03 | 真实 qbook入口与配置、SIGINT/SIGTERM、正常exit 0和文件关闭、启动/运行故障非零及部分线程清理。 | 已实现；Debug/Release 生命周期检查通过。 |
| V3-04 | 两模式完整文件链路、独立接纳账本、业务忽略与拒绝边界、周期快照、积压停止和Writer故障。 | 已实现；Debug/Release 网络、受控队列与子进程检查通过。 |
| V3版本 | V0–V2回归、本版必要Debug/Release功能检查、依赖/命令/数据格式说明、版本记录及新建产物清理。 | 已完成；必要测试和构建分支通过，文档与本轮新建产物清理完成。 |

2026-10-23 的实际执行结果如下。Catch2 的生成参数和 SECTION 不另计为 CTest 用例，独立依赖探针也不计入测试总数。

| 检查 | 实际结果 |
|---|---|
| 实施前 V0～V2 Debug 基线 | 115/115 通过，0 失败，7.76 秒。 |
| 完整 V3 Debug CTest | 154/154 通过，0 失败，25.30 秒。 |
| 关闭 PIPELINE/PERSISTENCE/SERVICE 的核心检查 | 75/75 通过，0 失败，3.51 秒。 |
| 完整 Release 构建及 storage/service 检查 | 新构建 268 步成功；39/39 通过，0 失败，14.94 秒。 |
| BUILD_TESTING=OFF、全部生产能力启用 | 在同一 Release 目录重配置和构建成功；118 项编译命令中测试/Catch2 源码为 0，测试目标为 0。 |
| 依赖隔离与开关错误 | 核心分支 122 项编译命令无网络、存储、服务源码，Arrow/Parquet/ZMQ 缓存项为 0；SERVICE 分别缺少 PIPELINE/PERSISTENCE 均以明确错误退出。 |

**验证限制。** Arrow Reserve/Finish 内存耗尽、fd 移交内部失败、系统 close 错误及信号系统调用失败未逐点注入；本轮对这些路径核对状态检查、异常传播和固定依赖源码。实际注入覆盖打开、批次写入、部分写入、尾批、footer、流关闭及清理错误。重复信号场景连续发送同一信号，标准信号可能合并，不能据此证明“收尾期间再次收到信号”的确定覆盖。未进行真实交易所接入、断电/崩溃恢复或性能验收；这些限制不改变正常接纳与排空契约。

正式 qbook、五类文件保存和三阶段停止已经实现并经过上述验证。V4 的指标网络发布与 UI、V5 的性能目标仍待对应版本实施；最终命令、限制和产物清理集中记录在本地 `docs/releases/V3.md`。

### 5.6 与 Spec 的一致性检查

| 规格约定 | 实现和验证核对点 | 依据 |
|---|---|---|
| 五类固定文件及精确整数 | 独立读取列序/类型/值，prev/next不存在；每种文件均创建。 | specs4.4.2.2；FR-11/12 |
| snapshot_depth=1定长列表 | Arrow schema恢复、父/子长度、空侧0/0及元数据一致。 | specs4.4.2.2；AC-10 |
| 每类独立批量及尾批 | 0、少于、等于、超过批量，交错类型与显式Close。 | specs4.4.2.2；AC-09/12 |
| Open/Closed/Failed状态 | 成功Close幂等；失败不再Add或重试；首错与noexcept析构。 | specs4.4.2.2、5.1 |
| 独占运行目录、不覆盖旧文件 | 排他创建成功作为所有权证据；真实碰撞、重复运行、旧字节比较。 | specs4.4.2.1；FR-12 |
| 文件资源归属Writer | Open/Write/Close/销毁线程记录，Run退出后writer_为空。 | specs4.4.2.1、5.2 |
| Add成功才计Writer指标 | 订单与撤单共计但分文件；失败与Metric溢出边界明确。 | specs4.4.2.1、4.5.2.2 |
| 业务忽略仍写原始输入 | 重复、市价、未知ID、方向冲突与总量越界读回原始字段。 | FR-11；BR-12；specs4.3/4.4 |
| 已接纳以成功入队为准 | 唯一受控生产者账本，网络发送/解码数不替代它。 | BR-15；specs6.3 |
| 普通停止依次排空 | Feed结束→Book结束→Writer排空Close结束，最后一次push不漏读。 | specs5.2；AC-11/12 |
| Fatal解除等待且返回非零 | 队列满、写入和关闭失败、部分启动失败均有实际退出证据。 | specs5.1、6.3 |
| 首错载荷join后读取 | 所有工作及测试Fail写者结束后检查stage和exception。 | specs4.1.2.2；V0/V2计划 |
| 不承诺跨文件全局恢复 | 逐类型顺序比较，不新增全局序号或把上游seq当完整本地总序。 | specs4.4.2.2；SP-06 |
| V3三阶段范围 | 真实Feed/Book/Writer，无空MetricsThread、UI、benchmark或强杀完整性声明。 | Sprint3.4；specs6.5 |

出现依赖API差异时，修正真实接入并记录；若改变字段、追加口径、停止保证或文件语义，必须同步相关规格和验收，不能只改预期让测试通过。本轮按上述约定核对实现，具体测试结果和未注入边界见第 5.5 节。

## 6. Plan Review（实现方案复核）

### 6.1 是否覆盖 Spec 中的设计

本计划覆盖五类 schema、Sink 列缓冲、Add/Reserve/Commit/Flush/Close、Open/Closed/Failed 状态，以及 Writer 的一次运行、独占目录、追加计数和线程内释放。正式入口复用配置与 RunControl，补齐三阶段生命周期、SIGINT/SIGTERM 和部分启动失败。

V1/V2 的业务与协议不在本版重写，文件验收补上它们此前无法证明的保存结果。合法但被业务忽略的事件仍保存原输入；拒绝帧不入业务文件；快照只来自 Book 的已知品种和既定周期，正常停止不强制新增最后快照。

Service.cpp、独立持久化开关、目录/流内部辅助、格式元数据键、非空列和窄测试 hooks 已按本计划实现，公开阶段签名保持 Spec 不变。相关文件表与格式说明按真实工程同步；业务契约和验收要求保持原定义。

### 6.2 算法与数据结构是否可行

每种记录进入独立 Sink，builder 与 schema 同序，整行完成后再增加 rows。五类固定对象比通用插件注册表更直接；每类最多一批应用列缓冲，无需在 Writer 再堆一份无界记录列表。RecordBatch/Table 只是把已经完成的列交给库，必须检查每次返回结果，不能假定 Finish 可以回滚。

目录与文件的排他成功是所有权依据，时间戳、进程号和尝试号只负责产生候选名。文件层保持各类型顺序，但没有新增跨文件事务或总序号。失败后的状态策略是停止与释放，而不是把部分写入倒回到还能继续运行的状态。

正常停止以两条 SPSC 的生产者结束为前提逐段推进；故障标志解除无法继续的等待。主线程完成位与 join 管理线程寿命，WriterGoalObserver 只为测试发布已观察到的值事件，两个机制都不把业务指标变成同步屏障。

### 6.3 依赖与编码顺序是否合理

先验证同步文件组件可以把格式错误与线程错误分开；再把它放进 Writer，最终才启用正式服务和信号处理。读取器与真实 I/O 故障工具随组件实现建立，能够及时发现“文件存在但不可读”“Add成功但尾批未关”等问题。

qbook_support 保持轻量，qbook_storage/qbook_writer 可以独立于 ZMQ 构建；只有真实服务把通信、建簿和文件组合起来。新增能力受明确开关控制，不要求旧核心构建安装全部依赖。测试代码不进入生产 main，也不为观测方便增加产品参数。

常规 build/debug 负责开发及完整回归。额外两个目录分别检查无文件/网络依赖的配置和 Release 文件/生命周期行为，有明确不同目标；不会为每个小任务复制相同构建，也不在本版引入性能或诊断工具全矩阵。

### 6.4 性能、资源与线程安全风险

| 方面 | 本版处理 | 仍需准确说明的限制 |
|---|---|---|
| 批次与压缩 | 每类正批量、预留容量、固定ZSTD和明确Flush调用。 | Arrow字典/压缩/元数据会占额外内存；单批容量不是进程内存上限。 |
| 保存完整性 | 正常介质下按已接纳边界排空，尾批及所有文件显式关闭。 | 不保证未交付网络消息、强杀、断电、跨重启恢复或失败运行完整。 |
| I/O停顿 | 文件由唯一Writer线程操作，上游经固定容量队列背压。 | 同步库/系统调用未必可立刻中断，没有硬性退出时限。 |
| 状态一致性 | 正常返回时列长一致；失败后禁止继续追加及盲目重试。 | 已写字节、先前指标和部分文件不能跨层回滚。 |
| 信号与线程 | 主线程处理停止信号，工作线程继承屏蔽；所有资源寿命覆盖join。 | 当前是Linux/WSL目标方案，不能直接宣称任意平台兼容。 |
| 测试观察 | 回调状态单Writer写，完成信号release/acquire，详细结果join后读。 | after_add只证明追加成功；仍需Close及真实读回。 |
| 私有测试入口 | 默认空、固定生命周期、只有定向用途；正式入口始终不配置。 | 不把测试故障开关暴露为服务功能，也不假称额外分支完全零成本。 |

V3 不声称吞吐、平均延迟或P99已达标，也不使用“用了无锁队列/Parquet”替代测量。V5 再用真实负载、明确时间范围和环境数据评估批量、压缩及瓶颈。

### 6.5 实施问题与核对结果

| 编号 | 待核对事项 | 时点及影响 | 当前状态 |
|---|---|---|---|
| V3-OI-01 | V0–V2真实代码、接口和验收是否已具备。 | V3编码前。 | 已核对，实施前基线 115/115 通过。 |
| V3-OI-02 | Arrow/Parquet准确版本、包来源、导出目标、链接与ZSTD可用性。 | V3-01接入。 | 官方 26.0.0-1 固定安装、shared targets、动态解析及 ZSTD 已验证。 |
| V3-OI-03 | 固定版本fd移交失败的所有权、Abort/析构行为、固定列表读回与元数据保留。 | 输出适配和文件测试。 | 资源源码已核对；成功接管/关闭及列表/元数据已实测。移交内部失败和系统 close 错误未注入。 |
| V3-OI-04 | 目标Linux信号接口、实际库后台线程的创建顺序及屏蔽继承。 | 正式入口接入。 | 子进程已核对工作线程屏蔽、主线程接收及原环境恢复；系统调用失败未注入。 |
| V3-OI-05 | 实际机器允许的本地TCP、子进程操作、文件权限及测试保护期限。 | 集成验收。 | 已在允许本机 socket 的环境完成 TCP/子进程检查；确定性路径错误及故障流用于文件失败验证。 |
| 原需求 OI-02/03 | 真实行情源的单位、ID和事件映射。 | 真实接入前确认；受控本地输入仍可开发。 | 不宣称已经与真实市场兼容。 |
| 原需求 OI-04/06 | 性能门槛，以及是否新增跨文件全序恢复需求。 | 分别进入V5或新需求评审时处理。 | 当前不设虚构指标，不擅自增加序列字段。 |

以上能够通过目标源码和环境检查解决的事项，由实施者直接核对并记录，不逐个发起不必要的审批。普通适配不改变业务契约；若需要改变输入字段、文件格式、接纳边界或正常停止保证，应先同步需求/规格/验收，不能只修改测试预期让程序通过。

## 7. Change Log（变更记录）

| 日期 | 文档版本 | 内容 | 实施状态 |
|---|---|---|---|
| 2026-10-20 | 0.1 | 按本次模板建立qbook V3实现计划：五类文件、Writer、正式三阶段服务、独立读回、真实信号、故障清理和构建验收。 | 计划已编写；生产代码未实施，构建/测试未执行，版本未验收。 |
| 2026-10-23 | 0.2 | 完成 V3-01～04，同步 Service.cpp、持久化目标、官方 Arrow/Parquet 26.0.0-1 固定安装及实际验证边界。 | 完整 Debug 154/154、核心分支 75/75、Release 文件/服务 39/39 及构建分支检查通过。 |

后续变更继续记录实际偏差、原因和受影响任务。本轮未提交或推送；版本记录保留在本地，V0～V2 历史结果与 V4/V5 范围保持独立。
