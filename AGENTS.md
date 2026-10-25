# AGENTS.md

Guidance for Codex when working in this repository.

## What this is

`qbook` 是参考 `nqbook` 重新开发的 C++ 限价订单簿服务，用于接收行情事件、按交易品种维护订单簿、生成快照并记录数据；它根据外部成交事件更新订单，不主动撮合买卖订单。项目使用 C++23、CMake、ZeroMQ、Arrow/Parquet 和已有的 `qlib` 基础库，项目代码使用命名空间 `qbook`。常驻进程中的行情接收、订单簿维护、数据写入三个阶段通过 SPSC 队列传递数据。V0 支持库和 V1 订单簿核心已完成；V1 历史 Debug 与 ASan/UBSan 均为 60/60 通过。V2 已完成纯协议、Feed/Book 阶段和测试消费者，Debug 验收为 115/115 通过。V3 已完成五类 Parquet、Writer 阶段、正式入口及信号停止，完整 Debug 154/154、核心分支 75/75、Release 文件与服务 39/39 通过。独立监控发布和 UI 留待 V4，性能实验留待 V5；实际构建说明见 [README.md](README.md)。

## Layout

以下列出当前文件与后续规划位置。Orderbook、Protocol、Feed/Book、持久化和正式服务已实现；指标发布、UI 和 benchmark 仍是规划。按当前版本需要创建文件，不要一次性生成所有目录、空类或占位实现。已有文件采用不同路径时，先核对实际结构，再同步本文档。

```text
AGENTS.md                         Codex 的仓库工作规则
README.md                         项目介绍、实际可用的构建与运行说明
docs/requirement.md               已编写的需求、范围与验收目标
docs/specs.md                     已编写的模块职责、接口契约和设计约束
docs/Sprint_Backlog.md             版本目标、任务与实际完成状态
docs/Implementation_Plan(Vn).md    V0～V5 计划均已编写；实施状态分别记录
CMakeLists.txt                    构建目标、依赖与测试入口
include/PipelineTypes.h           公共队列别名、模式及 ABI 断言
include/Protocol.h                纯协议解码、字段校验和拒绝结果
src/Protocol.cpp                  固定布局的逐标量读取
include/Config.h                  配置结构与加载接口
src/Config.cpp                    配置读取和启动时校验
src/ConfigChecks.h                私有时间算术检查，供实现及边界测试使用
include/Runtime.h                 RunControl 与 RunService 声明
src/Runtime.cpp                   RunControl 实现
src/Service.cpp                   三阶段组装、信号处理和停止协调
config/config.yaml                运行参数，如端点、容量和周期
include/Orderbook.h               单个交易品种的订单簿接口与契约
src/Orderbook.cpp                 新增、修改、成交、撤单、L2 更新与快照逻辑
include/ParquetWriter.h           数据写入接口
src/ParquetWriter.cpp             Arrow 构建器、批量写入和文件关闭
include/Threads/FeedThread.h       行情接收阶段与 FeedQueue 的声明
src/Threads/FeedThread.cpp         ZMQ 接收、帧解码和入队
include/Threads/BookThread.h       订单簿阶段与 RecordQueue 的声明
src/Threads/BookThread.cpp         事件分发、订单簿更新和周期快照
include/Threads/WriterThread.h     数据写入阶段的声明
src/Threads/WriterThread.cpp       消费记录并调用 ParquetWriter
include/Threads/MetricsThread.h    V0 指标单元与集合；发布阶段留待 V4
src/Threads/MetricsThread.cpp      V0 原子操作与值快照；发布留待 V4
src/main.cpp                      配置加载、日志初始化及运行入口
src/detail/                       排他目录/文件创建和服务、Writer 私有协作声明
tests/                            订单簿及其他组件的单元测试、集成测试
tests/support/OrderbookInspector.h 测试只读观察器，检查全部存储、索引、价位和链接
tests/support/AllocationFailure.*  专用测试程序的普通与对齐分配替换
tests/support/FeedPublisher.*      有限 XPUB 测试发布者，以订阅通知确认就绪
tests/support/RecordCollector.*    有上限的测试结果收集器，join 后读取结果
tests/support/PipelineTestSupport.h 测试线程异常边界、期限及清理
tests/support/ParquetReadback.*    五类文件的独立 schema、元数据和逐行核对
tests/support/FailingOutputStream.* 真实输出流的受控错误和资源线程轨迹
tests/support/ProcessHarness.*     独立子进程、信号、期限和输出管道
tests/service_test_driver.cpp     复用真实服务的私有测试驱动
tests/fixtures/protocol/           独立 golden bytes 与字段说明
benchmarks/                       独立性能实验；按版本需要添加
third_party/qlib/                 用户本地 qlib 的固定源码快照
third_party/qlib.sha256            qlib 相对路径与 SHA-256 内容清单
third_party/libzmq/               固定 libzmq 4.3.5 源码与许可证
third_party/cppzmq/               固定 cppzmq 4.10.0 源码与许可证
third_party/                      其余固定依赖及各自 SHA-256 清单
cmake/ArrowDependency.cmake       官方 Arrow/Parquet 目标及本地运行库接入
cmake/arrow-dependencies.lock.json 固定 Ubuntu 包来源、版本、哈希与许可证路径
scripts/install_arrow_dependencies.py 校验并提取固定包，不修改系统包数据库
.deps/arrow-26.0.0/               本地固定安装、归档和许可证，不提交
ui/                              监控界面；实现该功能时再建立
docs/releases/                   本地验收记录，保留但不提交或上传 GitHub
build/                           CMake 生成的构建产物，不手工维护、不提交
data_out/                        运行生成的数据文件，不提交
```

公共行情类型、事件变体与通用容器优先复用 `qlib`。参考项目在 `nlib/common.h` 中定义这些内容，但不能据此认定 `qlib` 的头文件、类型布局、函数签名和 CMake 目标完全相同。接入前阅读实际 `qlib` 源码与契约，再确定包含路径、链接目标和必要适配；不要只做 `nlib` 到 `qlib` 的文本替换。

订单簿业务、线程阶段、配置和持久化逻辑属于 `qbook`。通用容器缺陷在 `qlib` 中修复并验证，再更新依赖版本；仅供本项目使用的内部类型留在 `qbook`。依赖版本与接入方式应记录在构建配置和 README 中，不复制一份同名实现到业务代码里。

## Build

各库在 Linux / WSL2、x86-64、小端环境构建。使用 C/C++ 编译器、C++23、CMake 3.28 以上及 Ninja。
当前建立 `qbook_support`、`qbook_core`、`qbook_protocol` 和默认启用的 `qbook_pipeline`。
`QBOOK_BUILD_PERSISTENCE=ON` 增加 `qbook_storage` 和 `qbook_writer`，两者不依赖 ZMQ；默认 OFF。
`QBOOK_BUILD_SERVICE=ON` 增加 `qbook_service` 和正式 `qbook`，要求同时显式启用 PIPELINE 与 PERSISTENCE；默认 OFF。
`qbook_core` 只包含真实的 `src/Orderbook.cpp`，仅公开链接 `qlib::qlib`，不依赖支持库或日志。
`qbook_protocol` 仅依赖公共类型；`QBOOK_BUILD_PIPELINE=OFF` 不查找或构建 ZMQ，保留纯协议测试。
`qbook_tests` 包含 V0/V1 与协议回归共 69 项；`qbook_allocation_tests` 独立链接分配替换，共 6 项。
`qbook_pipeline_tests` 包含 Feed、Book 和真实网络/受控队列场景共 40 项，使用本机 TCP 临时端口；无发布者场景使用独占 IPC 路径。
V2 已验证流水线关闭分支 75/75 通过，以及关闭测试时四个生产库可构建；网络测试需要允许本机 socket 的环境。
完整 V3 注册 `storage::` 18 项和 `service::` 21 项，加上前置 115 项共 154 项。
本轮完整 Debug 154/154、核心分支 75/75 和 Release 文件/服务 39/39 均已实际执行通过。
关闭测试的完整生产构建不含测试源码或目标；SERVICE 缺少任一前置开关时均明确拒绝配置。
实际环境、依赖版本和验收结果以 [README.md](README.md) 与本地 `docs/releases/` 对应版本记录为准。`docs/releases/` 按项目所有者要求不提交或上传 GitHub。

qlib 使用用户本地源码的固定副本，位于 `third_party/qlib/`；逐文件内容记录在
`third_party/qlib.sha256`。它没有可用 Git 提交，不能编造来源或用其他上游分支替代。
构建通过真实 `qlib::qlib` 目标接入，关闭 `QLIB_BUILD_TESTS`。
Catch2 v3、fkYAML、spdlog、libzmq 4.3.5 与 cppzmq 4.10.0 同样保留固定源码及许可证，配置时不自动下载。
流水线通过官方 `cppzmq` 接口目标链接官方 `libzmq` 动态库；归档哈希和内容清单见 README。
V3 使用 Apache 官方 Ubuntu 24.04 amd64 Arrow/Parquet 26.0.0-1 包，安装到 `.deps/arrow-26.0.0/usr`。
固定包和运行依赖由锁文件及安装脚本管理，链接真实 `Arrow::arrow_shared`、`Parquet::parquet_shared`，配置时不下载。
本地 RPATH 含仓库绝对路径；移动目录后重新配置和链接。固定安装依赖基础系统运行库，不等于完整系统镜像。

从 **qbook 仓库根目录** 执行，常规构建持续复用 `build/debug`：

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

`--show-only` 只确认注册数量；必须实际执行测试才可写通过。
`BUILD_TESTING=OFF` 按能力开关构建生产库和服务，不要求 Catch2 或测试辅助。benchmark 开关为 ON 仍明确拒绝配置。
完整 V3 先执行 `python3 scripts/install_arrow_dependencies.py`，再将上面的 PERSISTENCE 和 SERVICE 同时设为 ON。
已缓存全部固定包时，安装脚本支持 `--offline`。安装会重新提取同版本文件，应在使用这些库的程序停止后执行。
正式服务从当前工作目录读取 `config/config.yaml`，在仓库根执行 `./build/debug/qbook`；SIGINT/SIGTERM 触发有序停止。
五类输出在新建的独占运行目录中保存；成功 Add 的指标不等于已关闭文件或断电安全。

新增版本时只接入当前功能所需的依赖和实际源码，不创建空类或伪服务目标。
完整服务、信号停止和文件验收命令见 README；发布部署、UI 和性能命令随对应任务落实后补充。
构建产物按当前版本计划区分原有目录与本次新建目录后清理，固定依赖源码保留。

## Conventions

- **任务与文档。** 开始前阅读本文件、当前版本的实现计划和涉及的规格章节，必要时回查需求与任务清单；先检查实际源码和工作区状态。以用户指定的版本为执行范围，不自动扩展到后续版本。不存在的文档不能声称已经读过；影响实现方向的缺失契约先说明，普通实现细节自行选择合理方案。

- **分步交付。** 按完整行为推进，例如“新增限价单并生成正确快照”，同时完成对应的正常路径、关键边界与必要验证。不要把每个成员变量或辅助函数拆成独立审批步骤，也不要在当前行为尚未完成时先搭建后续功能。

- **名称与代码风格。** 项目和主程序使用 `qbook`，本项目类型放在命名空间 `qbook`，基础库类型保留 `qlib`。类型名使用 `PascalCase`，成员函数沿用参考项目的 `PascalCase`，变量使用 `snake_case`，私有成员增加末尾 `_`；适合容器习惯的接口可以保留 `size()` 等名称。文件名与主要类型对应，头文件和实现分离，现有一致风格优先。

- **中文说明、英文标识符。** 代码注释、项目说明和任务总结默认使用中文，标识符使用英文。公共声明说明用途、前置条件、结果和关键失效规则，实现处解释不明显的机制或设计原因；不要逐句翻译代码，不要求每行都加注释，也不添加无意义的说明段落。

- **数据契约。** 价格、数量、时间和 ID 的类型及单位以 `qlib` 和 Specs 的实际定义为准。固定点数的倍率、纳秒时间戳的含义、事件标签及序列化布局必须明确；不得擅自假定某个结构体的字节数，也不得混淆交易所事件时间与本机接收时间。修改跨进程协议时同步发送端、接收端和相关验证。

- **订单簿职责。** 一个 `Orderbook` 实例维护一个交易品种；多个品种由上层分发。它根据新增、修改、成交和撤单事件更新状态，按照规格生成快照；交叉挂单不触发主动撮合。不要把策略、下单、风控或撮合引擎功能顺手加入本项目。

- **订单簿存储。** `Orderbook` 使用 `qlib::hive` 持有地址稳定的订单、ID 索引定位订单、按价格排序的买卖价位集合，以及价位内的 FIFO 关系。新增、扣减和删除必须使订单存储、ID 索引、链表及价位总量保持一致；外部传来的指针字段不能作为本地链接使用。测试只读观察器定义在 `tests/`，生产头无条件声明友元，不增加在线查询接口。

- **订单簿观察与移动。** `OnSnapshot`、`size` 和 `MemoryBytes` 也由当前所有者串行调用；`MemoryBytes` 仅按固定公式估计，超界饱和，不能称为 RSS 或精确分配量。禁止复制订单簿；移动保留活订单地址，移动后重取容器迭代器，外层品种表插入后重查订单簿。移动来源只保证可析构和重新赋值。

- **业务边界。** 修改订单是否保留优先级、重复 ID、未知订单、非正数量和超额扣减如何处理，必须遵守规格并覆盖关键测试。尚未规定时不能仅凭参考实现中的偶然行为定义业务规则。校验应放在合适的输入边界，内部避免重复执行没有明确用途的全量检查。

- **逐笔与 L2 模式。** 每次运行选择一种建簿模式，不将逐笔订单和聚合价位更新混入同一个订单簿。L2 记录表示价位的绝对总量，具体删除语义按规格确定；成交记录是否影响 L2 深度也必须由协议约定，避免同时处理成交与价位更新造成重复扣减。

- **阶段与线程。** 阶段类负责 `Run()` 循环和流程协调，订单簿业务集中在 `Orderbook`，Arrow/Parquet 操作集中在 `ParquetWriter`。阶段对象本身不等于已经启动的线程，实际线程由入口统一创建和管理。不要把业务实现、持久化细节都堆入 `main.cpp` 或线程循环。

- **并发所有权。** 订单簿只由 Book 阶段访问；相邻阶段通过各自的一条 SPSC 队列交接事件，保持单生产者、单消费者。监控只读取明确共享的原子指标，不直接遍历订单簿或队列内部。新增跨线程访问时先明确所有权、生命周期和同步关系。

- **队列与停止。** 明确队列满时的等待策略和停止行为，不静默丢弃已承诺处理的事件。正常停止按 Feed、Book、Writer 的顺序停止并等待完成，使下游有机会排空输入。V2 独立测试链路仍使用 Collector；监控阶段接入后最后停止。Book 和 Writer 先观察停止，再确认输入为空；主线程只在上游 join 后停止下游。保证范围应区分“已从网络接收”“已成功入队”和“已写入文件”；SPSC 背压不能直接证明 ZMQ PUB/SUB 端到端无丢失。

- **配置。** C++ 运行参数集中放入 `config/config.yaml`，由配置结构承载并经构造函数传给组件，不添加命令行或环境变量覆盖机制。UI 的构建参数由后续 UI 版本管理，不覆盖 C++ 配置。对容量、批量大小、采样周期等参数在启动时做必要校验；缺失项是否采用默认值以及非法配置如何报错，按规格统一处理。

- **日志与错误。** 日志统一经过选定的日志库，使用固定 spdlog。正常启动、停止用 `info`，可恢复异常用 `warn`，无法继续服务的错误需明确记录并进入约定的失败路径。避免在每条正常行情或满队列重试循环中打印日志；持久化失败不能被当作成功，不为追求简短代码随意吞异常或调用 `abort()`。

- **资源与持久化。** 使用 RAII 管理线程、套接字、文件和拥有所有权的内存；索引或侵入式链接中的裸指针必须有明确生命周期。Parquet 按记录类型分开写入，列定义与追加顺序保持一致，批量大小来自配置。计数“已追加到缓冲区”不等于“已持久化到磁盘”，接口、指标和说明应准确区分。

- **指标。** 热路径只更新必要的指标，监控线程负责采样和发布。累计计数、瞬时状态和抽样延迟分别定义；抽样比例和计时范围应明确。若采用单写者原子的 `load` 加 `store` 更新方式，必须保持单写者前提；不能把抽样均值表述为完整延迟分布或尾延迟。

- **测试。** 业务变更验证可观察结果，重点覆盖订单与价位状态、快照顺序、关键边界及相关失败路径；并发集成验证事件顺序、队列交接和有序停止。不要仅按私有函数逐个照抄实现写测试，不为纯文案改动增加测试。完成后报告实际运行的命令、结果和未验证内容。

- **性能。** Benchmark 与功能测试分开，使用发布构建并说明工作负载、数据规模、计时边界和运行环境。没有测量结果时不宣称“更快”或引用参考项目的耗时作为本项目成绩。需要解释瓶颈时再使用 `perf` 等分析工具；不以更快为由改变业务语义，也不把队列无锁扩大描述为整个服务所有路径都无锁。

- **依赖与修改范围。** 优先复用已选依赖，不为单个简单功能引入额外框架；修改 `qlib` 或其他第三方内容前先确认问题归属。只修改与当前任务相关的文件，保留用户已有改动，不顺手重构无关代码。构建产物、运行数据和机器专属配置不提交到仓库。

- **Git 与交付。** 遵循用户当前授权和版本计划中的 Git 约定；未授权时不自动提交、推送或改写历史。每次交付说明完成的行为、涉及文件、验证结果和剩余问题。不要假定存在参考作者的自动提交任务或本机专用 skill。

- **持续更新。** 本文件记录跨任务持续有效的规则，不替代需求、规格或每个版本的实现计划。目录、构建命令或架构约定落地变化时同步更新相应内容，删除过时说明；计划内容只有在实际实现并验证后才能改成已完成，不能为了迎合当前代码而擅自改写需求。
