# qbook Implementation Plan（V0 实现计划）

文档版本：0.2｜日期：2026-10-20｜对应版本：V0 工程、配置与共享基础

状态：**实现计划已编写；V0-01～V0-04 已实现并完成自动化验收，实际结果集中记录于本地 `docs/releases/V0.md`（不上传 GitHub）。**

实际路径为 `docs/Implementation_Plan(V0).md`，与 Sprint Backlog 的版本引用一致。本文件严格针对 V0-01～V0-04，按上传模板说明实现目标、模块与类、关键函数步骤、编码顺序、验证和方案复核。正文保留实施步骤与验收约定；实际完成状态见第 5.5 节，命令、数量和限制集中记录于 releases/V0.md。

## 1. Implementation Overview（实现概述）

### 1.1 本次实现目标与范围

V0 要先解决“后面的开发能否建立在同一套可靠基础上”：固定 qlib 和测试依赖，建立能够实际执行测试的工程，将 YAML 参数转换成经过检查的 Config，并提供线程将来可以共同使用的故障标志和原子指标。

| Backlog 任务 | 本次实际要实现的能力 | 本版结束时可检查的结果 |
|---|---|---|
| V0-01 统一文档并固定 qlib 来源 | 文档名称、依赖来源与实际版本清楚。 | 文件引用可定位，选定依赖可复取，README 不混入参考作者环境。 |
| V0-02 构建、公共类型和契约测试 | 明确 BookMode 和两类队列，检查数据布局与实际容器能力。 | 当前源文件可编译，CTest 发现并执行真实测试，不需要网络或文件服务依赖。 |
| V0-03 配置默认值、加载与校验 | 默认值、文件覆盖和非法配置处理一致。 | 缺文件有提示；合法配置返回完整值；格式、类型或数值错误明确失败。 |
| V0-04 故障控制与指标基础 | RunControl 保留第一条致命错误，Metric/Metrics 支持规定的读写方式。 | 首条错误在 join 后可读；指标不静默溢出，16 项准确映射为普通值快照。 |

本版不实现 Orderbook、行情解码和网络接收、阶段 Run 循环、Parquet、MetricsThread 发布、UI 或 benchmark，也不创建 `main.cpp`、`RunService()` 或空的 `qbook_core` 来凑齐未来架构。V1 随真实 Orderbook 源码建立核心库，V3 才提供正式服务程序。

RunControl 的测试可以创建几个短期测试线程，用来验证错误报告的并发契约；这不表示 V0 已实现 Feed/Book/Writer 的运行和停止。Metrics 的全部字段先就绪，真实业务计数更新点在 V2/V3 落地，指标发布在 V4 完成。

### 1.2 参考的 Requirement 与 Spec

| 依据 | 本版读取范围与用途 |
|---|---|
| [AGENTS.md](../AGENTS.md) | 仓库规则、中文说明与英文标识符、依赖修改边界、所有权、测试和 Git 约定。 |
| [requirement.md](requirement.md) | FR-01；FR-02、FR-13 的基础部分；BR-14；第 6 章；NFR-03/05/06/07；AC-01、AC-12 的配置部分、AC-16 的当前构建部分；OI-01。 |
| [specs.md](specs.md) | 2.4 的共享类型，3.2～3.4 的数据和 ABI，4.1.2 的 Config/RunControl，4.5.2.1～4.5.2.2 的 Metric/Metrics，5.1/5.2/5.4 的错误、并发和构建。 |
| [Sprint_Backlog.md](Sprint_Backlog.md) | 3.1 的 V0 四项任务及版本验收；全局完成标准和跨版本边界。 |
| 当前固定的 qlib 源码 | `common.h`、`single_queue.h`、`CMakeLists.txt` 的实际契约。文档写作已核对所提供版本，实施时仍要确认最终接入版本相符。 |

本次模板新增的单一构建目录、最小验证记录和临时产物清理要求，在第 5.2 节具体落实。早期 AGENTS 或 Backlog 中“新构建目录”按本计划理解为首次建立标准目录；之后继续复用，不为每项任务或每次通过检查复制一份构建树。

V0-01 同步已有文档的路径和状态：根目录引用使用 `docs/requirement.md`、`docs/specs.md`；V0～V5 实现计划均已编写，代码和验收状态分别跟踪，不因计划已存在而标记实现完成。保留尚未确定的真实上游、性能目标等问题，不因计划成文就把它们关闭。

### 1.3 现有代码与可复用组件

计划编写时的依据为项目文档、nqbook 参考源码和 qlib 源码；本次 V0 已按该基线实施。开始执行时先查看实际工作区，若用户已有同等功能，应核对和补齐，保留其改动，不能用本计划覆盖已经成立的实现。

| 可复用内容 | 使用方式 | 不应做的事 |
|---|---|---|
| qlib 公共记录与 `single_queue` | 直接包含并链接固定版本；qbook 定义自己的模式与队列别名。 | 不再复制一份同名 order/record/queue，不把 nlib 替换名称就当作完成兼容。 |
| nqbook 的配置方向与默认值 | 参考其配置、日志依赖及项目用途，按 qbook 已冻结的校验规则实现。 | 不继承未验证的 Docker 地址、安装命令或宽松错误处理。 |
| Catch2 v3 | 统一提供断言、测试入口和 CTest 发现。 | 不写只有 `return 0` 的“测试”，不复刻 qlib 全部容器测试。 |
| fkYAML、spdlog | 固定来源和版本，前者解析 YAML，后者输出必要诊断。 | 不自写 YAML 解析器、不把日志存在当成功证据。 |
| 标准库 | 使用 filesystem、chrono、atomic、exception_ptr 和测试线程。 | 不为固定参数和第一条错误引入动态管理框架。 |

### 1.4 开发环境、依赖与工具链

目标环境为 Linux / WSL2、x86-64、小端、8 字节指针，C++23 与 CMake 3.28 或以上。Ninja 用于标准构建；编译器和标准库必须实际支持所用特性。本次实际工具链和编译结果见 releases/V0.md，不沿用参考项目的历史结果。

依赖按下表接入。版本号、提交和来源由实际可核对的资料填写，不能编造。默认使用仓库内固定源码，避免构建时自动拉取某个不断变化的分支；已存在且可复现的依赖登记优先保留。

| 依赖 | 本计划选择的接入位置与方式 | V0-01/02 需要记录 |
|---|---|---|
| qlib | `third_party/qlib/`；`add_subdirectory`，链接 `qlib::qlib`。 | 用户授权来源 `/mnt/d/Desktop/unsw/qlib`；固定完整源码快照与 `third_party/qlib.sha256`，原目录只读。无 Git 元数据，不编造提交。 |
| Catch2 v3 | `third_party/Catch2/`；固定源码提交，测试开启时加入构建。 | 具体补丁版本及提交，构建时不自动追踪最新分支。 |
| fkYAML | `third_party/fkYAML/include/` 的头文件及其所需配套文件；使用项目自己的头文件依赖目标。 | 实际来源、版本/提交、许可证和头文件相对路径。 |
| spdlog | `third_party/spdlog/include/` 的头文件与所需配套文件；保持选定的 header-only 配置一致。 | 实际来源、版本/提交、许可证以及 bundled/external fmt 的实际选项；不得混用不同配置。 |
| Threads | CMake 的 `Threads::Threads`。 | 在当前 Linux/WSL 工具链中的配置和链接结果。 |

本版不查找或安装 ZeroMQ、Arrow、Parquet、Node、Next.js。依赖源码是工程输入，不属于验证结束后可以随意删除的临时下载产物；可重新下载的归档缓存、解压中转目录则在使用完后清理。

### 1.5 实现约束与完成条件

**执行约束。** 先读 AGENTS、当前计划、Backlog 的 V0 和相关 specs，再检查实际文件与工作区状态。按四个任务完成代码和必要验证，普通实现细节自行处理；不要每个文件都停下来请求确认，也不要自动进入 V1。发现会改变公开契约的冲突时先定位和报告，不能偷偷修改产品规则来让测试通过。

**修改与 Git 边界。** 只改 V0 所需代码、测试、依赖登记和相关文档；英文标识符、中文说明。用户已有修改保留，不使用 `git reset --hard`、`git clean` 或整目录重建来恢复失败。提交和推送按用户实际授权执行；本次已授权实施 V0；不自动提交、推送或改写历史。

**完成条件。** 四项任务均有真实可观察结果；标准构建成功且实际发现测试；本计划列出的关键边界和并发契约通过；文档与实际接口相符。仅用 `docs/releases/V0.md` 记录必要环境、实际命令、用例数量、通过/失败和未验证项，不保存成功构建的整段日志或多份重复总结。按第 5.2 节清理本任务新建的可再生产物后，才完成交付检查。

完成 V0 只能声明“基础工程、配置和共享控制/指标已验证”，不能声明服务能接收行情、正常保存文件或性能达标。

## 2. Implementation Structure（实现结构）

### 2.1 技术模块与代码文件的对应关系

| 实现单元 | 对应任务与规格模块 | 本版文件和产物 |
|---|---|---|
| 工程与公共契约 | V0-01/02；跨模块公共基础。 | CMake、依赖登记、`include/PipelineTypes.h`、`tests/contracts_tests.cpp`、文档与忽略规则。 |
| 配置 | V0-03；T-01 的配置部分。 | `include/Config.h`、`src/Config.cpp`、`config/config.yaml`、`tests/config_tests.cpp`。 |
| 故障控制基础 | V0-04；T-01 的 RunControl 部分。 | `include/Runtime.h`、`src/Runtime.cpp`、`tests/runtime_metrics_tests.cpp` 中的错误控制场景。 |
| 原子指标基础 | V0-04；T-05 的 Metric/Metrics 部分。 | `include/Threads/MetricsThread.h`、`src/Threads/MetricsThread.cpp`、同一测试文件中的指标场景。 |
| 简明验收记录 | 四项任务的结果。 | `docs/releases/V0.md`；仅在实际执行时填写结果。 |

第 3 章将 T-01 拆为配置与故障控制两个实现单元，方便说明各自函数；这不改变 specs 的六个系统模块。指标文件沿用已定路径，但本版只声明并实现 Metric、Metrics 和 SnapshotMetrics，不放未实现的 MetricsThread 空类，也不包含 ZMQ 头文件。

### 2.2 各模块需要实现的类与组件

| 名称 | 类型与本版职责 |
|---|---|
| `BookMode`、`FeedQueue`、`RecordQueue` | 枚举与类型别名，统一模式和队列元素类型。 |
| ABI 契约检查 | 编译期断言及少量运行测试，验证类型宽度、字段偏移和选定布局。ABI 指当前编译环境采用的对象大小、对齐与字段位置。 |
| `Config` | 参数值集合，初始化为规格默认值，不启动任何资源。 |
| `LoadConfig`、`ValidateConfig` | 自由函数：读取/覆盖/验证参数，成功时返回可用配置。 |
| 配置内部检查函数 | 仅帮助安全解析整数、计算容量、校验时间/批次；不作为通用工具库导出。 |
| `RunControl` | 第一条致命错误与原子失败标志，诊断只在所有报告者 join 后读取。 |
| `Metric` | 一个单写者、可被并发读取的原子整数；提供 Add、Set、Read。 |
| `Metrics`、`SnapshotMetrics` | 16 个有名指标及从它们到 `qlib::metrics` 普通值的明确映射。 |

### 2.3 类与源文件、头文件的对应关系

公共头文件必须能自足编译：它们直接包含声明需要的标准或 qlib 头文件，不依赖调用者碰巧提前包含。所有 qbook 声明位于 `namespace qbook`；qlib 类型保持原命名空间。

| 文件 | 放入什么 | 不放入什么 |
|---|---|---|
| `PipelineTypes.h` | BookMode 和两个队列别名。 | 工作线程、配置加载、动态全局对象。 |
| `Config.h` | Config 字段及 LoadConfig/ValidateConfig 声明。 | YAML 解析细节、网络/文件服务。 |
| `Config.cpp` | YAML 类型检查、覆盖、数值验证、必要日志。 | main、阶段启动、全局配置单例。 |
| `Runtime.h/.cpp` | RunControl 声明和实现。 | RunService 空实现、信号注册、完整线程管理器。 |
| `Threads/MetricsThread.h/.cpp` | Metric、Metrics、SnapshotMetrics。 | 网络发布、定时采样循环或未实现的 MetricsThread。 |
| 三个测试源文件 | 契约、配置、错误控制和指标的真实场景。 | 对尚未实现功能的伪通过断言、生产在线调试 API。 |

三个生产源文件 `src/Config.cpp`、`src/Runtime.cpp`、`src/Threads/MetricsThread.cpp` 组成本版静态库 `qbook_support`，供测试和后续服务复用；三个测试源文件仅加入 `qbook_tests`，并使用 Catch2 提供的 main。`qbook_support` 是本计划为已有职责确定的构建目标，不引入新的业务架构层；源码文件必须实现真实行为，禁止以空 `.cpp` 填满列表。

### 2.4 模块与类之间的实现依赖

先固定 qlib，再建立公共类型与 ABI 检查；Config 的容量验证使用真实事件类型和队列分配规则。RunControl 只依赖标准库；Metric/Metrics 依赖 qlib 的缓存对齐宏及传输字段，不依赖 Config。Config 与错误/指标基础可在公共契约后独立实现，最终由同一测试目标验证。

`qbook_support` 不链接 Catch2；只有 `qbook_tests` 链接测试框架。配置实现使用 fkYAML/spdlog，公共指标头不传播 YAML 类型；后续业务核心可继续独立于网络、Arrow 和 UI。具体构建目标依赖在第 3.1 节列出。

### 2.5 整体编码顺序及其原因

1. 完成 V0-01：核对文档、来源、版本和已有目录，得到唯一基线与清理边界。
2. 完成 V0-02：写公共类型和契约测试，先让真实测试能够编译与执行；此时可以只登记已经存在的测试源文件。
3. 完成 V0-03：加入 Config 源码、示例和测试；同步扩展当前构建目标，不预先列出不存在的实现文件。
4. 完成 V0-04：加入 RunControl 和指标代码及测试，验证多线程报告和独立字段映射。
5. 在同一标准构建目录运行 V0 全部验证，检查文档和依赖，写一份最小验收记录，清理任务新建的临时产物。

## 3. Module Implementation（模块实现方案）

以下按“模块概述 → 类及关键函数 → 模块协作、算法、并发、错误、顺序、验收”展开。工程模块不强行引入类；Config 的自由函数放在其配套实现中说明。关键函数的约定覆盖模板要求的输入输出、执行步骤、状态、调用关系、错误、复杂度、线程和验证。

### 3.1 工程与公共契约

#### 3.1.1 模块实现概述

1. **模块实现目标：** 对应 V0-01、V0-02。先让仓库明确依赖哪个 qlib，随后让真实测试使用相同数据类型、模式和队列别名编译，避免后续组件各自定义一套不一致的接口。
2. **对应的 Spec 章节：** `specs.md` 2.4、3.2～3.4、3.8、5.4、5.6；需求依据为 NFR-06、NFR-07 和公共数据约定。本版只冻结契约，不实现网络字节解码。
3. **需要实现的组件与文件：** `CMakeLists.txt`、`include/PipelineTypes.h`、`tests/contracts_tests.cpp`、`.gitignore` 和 README 的实际说明；维护现有四份项目文档的准确名称。没有对应业务职责时不新建 `Core`、依赖管理器或空主程序。
4. **依赖与复用组件：** 复用 qlib 的 `common.h`、`single_queue.h`；固定源码形式的 Catch2 提供测试入口。fkYAML、spdlog 在配置模块实现时接入，并与本节的最终构建结构一致。

#### 3.1.2 Class Implementation（公共组件实现）

本模块没有需要实例化的业务类。模板的类实现栏目按适用项说明 `BookMode`、队列别名、布局检查和构建组件，不为凑齐“类”而增加没有职责的对象。

**1. 实现目标。** 用一个公共头文件表达 qbook 的运行模式和阶段交接类型；用编译期断言锁定当前协议布局，用少量运行测试确认 qbook 依赖的队列行为。

**2. 声明与文件组织。** `PipelineTypes.h` 包含自身使用的标准头和 qlib 头，不依赖任何 `Threads/*Thread.h`。公共声明如下，字段布局断言置于同一公共契约头的明确区域，避免只有开启测试时才检查协议前提：

```cpp
#pragma once
#include <cstdint>
#include <qlib/common.h>
#include <qlib/single_queue.h>

namespace qbook {
enum class BookMode : std::uint8_t { Mbo, L2 };
using FeedQueue = qlib::single_queue<qlib::feed_event>;
using RecordQueue = qlib::single_queue<qlib::record>;
}  // namespace qbook
```

按实际使用补上 `<bit>`、`<cstddef>`、`<type_traits>` 等断言需要的头文件，不依赖 qlib 间接替本项目包含标准头。`contracts_tests.cpp` 包含公共头，测试默认初始化与队列使用；不重新定义 qlib 的六种记录或 variant。

**3. 数据、初始化、所有权与不变量。**

| 维度 | 具体实现 |
|---|---|
| 数据类型与容器 | `BookMode` 是本地 `uint8_t` 枚举；FeedQueue 保存 `qlib::feed_event`，RecordQueue 保存 `qlib::record`。别名不增加包装层、分配或成员字段。 |
| 初始化与生命周期 | 记录由调用者使用 `T{}` 初始化，再写明确字段；零初始化不代表业务输入合法。队列在获得合法容量后构造，构造时只分配槽位，不为每个槽默认构造记录。 |
| 所有权与资源管理 | 队列拥有槽存储与尚未弹出的对象；`try_pop()` 返回的 optional 独立拥有值。记录中的订单链接只是非拥有指针，本模块不解引用或释放它们。 |
| 状态约束与不变量 | 同一队列一次仅有一个生产者和一个消费者；销毁前两侧已停止并同步。队列不可复制、不可移动；值类型的移动构造和析构必须实际不抛异常。 |

**4. 重要操作与检查的实现。** 本模块不新增生产成员函数；以下检查覆盖 qbook 使用现有接口时的声明、输入输出、执行步骤、状态、错误、复杂度和并发前提。

**4.1 公共数据与 ABI 编译期检查。** ABI 是当前编译器和平台对字段在内存中的排布约定。本项目使用已选定的小端 x86-64 配置，不能把这些检查当成通用序列化能力。

| 函数/检查维度 | 具体实现与契约 |
|---|---|
| 声明与职责 | 使用 `static_assert`、`sizeof`、`offsetof`、类型特征及 `std::endian::native`；它们是编译期表达式，不增加运行入口。 |
| 输入、输出与契约 | 输入为锁定版本 qlib 的真实定义和当前工具链；满足已选布局才继续构建，不返回运行结果。 |
| 核心算法 | 逐一比较预期类型、枚举值、倍率、大小和每个被协议使用的字段偏移，不只验证结构体总大小。 |
| 执行步骤 | 先检查标准布局/字段类型和 `sizeof(void*) == 8`，再检查小端与倍率，最后检查下表全部布局；错误信息标明记录或字段。 |
| 数据读取与修改 | 只观察类型属性和编译期常量，不创建共享对象，不修改 qlib 定义。 |
| 调用关系 | 公共头包含实际 `qlib/common.h`；配置和后续阶段使用公共头时自动得到这些前提检查。 |
| 边界与错误 | 任一不匹配直接编译失败；不能改预期值“让它过”而不更新协议和对应规格。当前宏基线为 `QLIB_CACHE_LINE == 64`，不支持某个编译单元单独覆盖。 |
| 异常安全与一致性 | 不存在运行期部分更新；失败时没有可执行文件被认定为协议兼容。 |
| 时间与空间复杂度 | 工作发生在编译期，数量固定；不引入运行期分配或每事件检查。 |
| 并发与线程安全 | 没有并发执行；静态布局通过仍不能证明并发算法正确或网络版本完全兼容。 |
| 验证方法 | 编译真实契约测试，检查各类型值初始化及命名字段；不通过整块 `memcmp` 要求所有 padding 都为零。固定网络字节样本与解码在 V2 完成。 |

需要冻结的常量为 `price_scale = 10'000'000'000`、`qty_scale = 100'000'000`、`snapshot_depth = 1`；tag 为 order=0、trade=1、level=2、cancel=3；side 为 buy=0/sell=1、type 为 limit=0/market=1、action 为 add=0/modify=1，枚举底层均为 `uint8_t`。时间、价格、数量、订单 ID 和序号的实际字段类型为 `int64_t`，品种为 `uint32_t`；指标时间为 `int64_t`，其余 16 项为 `uint64_t`。

| 记录 | `sizeof` | 必须逐字段断言的偏移 |
|---|---:|---|
| `qlib::order` | 80 | seq=0、order_id=8、price=16、qty=24、new_qty=32、prev=40、next=48、instrument_id=56、side=60、type=61、action=62、event_ns=64、recv_ns=72。 |
| `qlib::trade` | 64 | seq=0、buy_order_id=8、sell_order_id=16、price=24、qty=32、event_ns=40、instrument_id=48、side=52、recv_ns=56。 |
| `qlib::cancel` | 48 | seq=0、order_id=8、qty=16、instrument_id=24、side=28、event_ns=32、recv_ns=40。 |
| `qlib::level` | 48 | seq=0、price=8、qty=16、event_ns=24、instrument_id=32、side=36、recv_ns=40。 |
| `qlib::book` | 56 | event_ns=0、bid_price=8、bid_qty=16、ask_price=24、ask_qty=32、instrument_id=40、recv_ns=48；四个价格/数量成员均为 `int64_t[1]`。 |
| `qlib::metrics` | 136 | ts_ns=0；feed_messages=8、feed_bytes=16、feed_orders=24、feed_trades=32、feed_levels=40、feed_dropped=48、book_events=56、book_apply_ns=64、book_samples=72、book_instruments=80、book_resting_orders=88、book_memory_bytes=96、writer_orders=104、writer_trades=112、writer_levels=120、writer_books=128。 |

同时检查两种 variant 的候选类型及顺序与 qlib 定义一致；这用于确认 API，不表示可以把 variant 的内存直接发送出去。`qlib::price_level{}` 的 qty 为 0、head/tail 为空，只检查成员值，不为其本地链接结构定义网络格式。

**4.2 队列集成契约检查。**

| 函数/检查维度 | 具体实现与契约 |
|---|---|
| 声明与职责 | 直接使用现有 `single_queue<T>(capacity)`、`capacity()`、`try_push(T&&)` 和 `try_pop()`；不增加生产包装函数。 |
| 输入、输出与契约 | 测试用小容量 3 和可识别序号的记录；期望实际容量 4，四次成功入队，第五次失败，出队保持 FIFO。 |
| 核心算法 | 测试只操作公开接口，不查看私有 head/tail，不复制 qlib 的环形算法作为“参考实现”。 |
| 执行步骤 | 构造两种实际队列；分别加入四条可识别记录并检查成功；检查第五次失败；依次弹出并核对类型/序号；再检查空结果和槽位可复用。 |
| 数据读取与修改 | 输入记录按值入队；队列推进自身位置，弹出值与队列槽位独立；不得修改仍由队列拥有的对象。 |
| 调用关系 | `try_push()` 内部经 `try_emplace()` 转发；测试不依赖它的私有实现，满时不移动实参由下面的受控探针验证。 |
| 边界与错误 | 运行测试不申请极大容量；零容量在 qbook Config 中拒绝，不能因为 qlib 会将 0 变成 1 就允许服务配置为 0。构造分配失败属于异常，不能被当成空队列成功。 |
| 异常安全与一致性 | 满时返回 false 不发布元素；队列中既有元素仍可顺序弹出。测试探针移动和析构实际不抛异常，不能用违反 qlib 前提的类型测试正常承诺。 |
| 时间与空间复杂度 | 测试只使用固定数量的元素，O(1) 规模；验证容量行为而不是测量吞吐。 |
| 并发与线程安全 | 本组契约测试可在一个测试线程中先生产后消费，角色串行交接；不把测试当成完整 SPSC 压力验证。未来并发测试仍须保持单生产者和单消费者。 |
| 验证方法 | 对 FeedQueue、RecordQueue 做实际类型的容量/FIFO 测试；另用局部 `MoveProbe` 检查满队列失败推送没有调用移动构造。通过类型特征检查两个队列不可复制/移动及实际事件类型不抛移动/析构。 |

`MoveProbe` 只存在于测试文件，包含可检查的值和移动次数，移动构造为真实不抛实现。先填满小队列，保存候选对象的值和计数，再调用 `try_push(std::move(candidate))`；失败后两者均保持。普通 qlib 记录的移动可能表现为字段复制，仅检查它们“内容没变”无法证明没有发生移动，因此需要这个有观察意义的小探针，不需要通用测试框架。

**5. 构造、初始化与资源释放。** 公共别名没有生命周期；测试中的记录、队列与探针都由局部作用域管理。队列作用域结束前无其他访问者，剩余记录由队列析构释放；测试不能复制或搬移一个已经构造的队列。构建目录和临时测试数据按第 5.2 节处理，源码依赖不是本次可删除的临时产物。

**6. 关键实现流程。** 先锁定真实定义，再冻结它们的约束，随后执行小规模公开接口测试。这样能够尽早发现“依赖改名了但接口不兼容”“容量 3 是否实际只可用 3”“指标宏是否写错”等问题，而不需要先构建订单簿、网络或文件链路。

**7. 实现顺序与依赖关系。** V0-01 完成文档和依赖登记；V0-02 创建公共头及仅包含契约测试的 `qbook_tests`。之后 V0-03、V0-04 随实际源码增加 support 库和测试源；不在早期 CMake 中列出尚不存在的文件，也不创建空文件凑齐最终目标。

#### 3.1.3 模块内部协作与调用流程

最终 V0 使用一个支持库和一个测试程序。`qbook_support` 只包含本版实际实现的 Config、RunControl、Metric 与 SnapshotMetrics；它不叫 `qbook_core`，避免让尚未实现的订单簿核心看起来已经存在。`qbook_core` 留到 V1 随 Orderbook 建立，正式 `qbook` 主程序留到 V3。

| 构建目标或依赖 | 具体组成与使用方式 |
|---|---|
| `qlib::qlib` | 对锁定的 `third_party/qlib/` 调用 `add_subdirectory`，沿用真实头文件接口目标；先设 `QLIB_BUILD_TESTS=OFF`。当前 qlib 自带测试使用 GoogleTest 并有获取依赖步骤，本版不重复启动它们。 |
| `qbook_yaml` | 本项目的 INTERFACE 目标，指向已固定的 `third_party/fkYAML/include/`；配置实现私有使用，不把 YAML 类型泄露进 Config 公共接口。 |
| `qbook_logging` | 本项目的 INTERFACE 目标，指向已固定的 `third_party/spdlog/include/`；按所选源码的头文件模式使用，不启用与该方案冲突的编译库/外部 fmt 宏。 |
| `qbook_support` | STATIC 库，最终包含 `src/Config.cpp`、`src/Runtime.cpp`、`src/Threads/MetricsThread.cpp`。最后一个文件此时仅实现指标基础，不包含网络发布类或 ZMQ。 |
| `Catch2::Catch2WithMain` | 将固定提交的 `third_party/Catch2/` 作为子目录加入，仅在 BUILD_TESTING 开启时使用；采用其实际 `extras/Catch.cmake` 注册测试。 |
| `qbook_tests` | 一个可执行文件，最终包含 `tests/contracts_tests.cpp`、`tests/config_tests.cpp`、`tests/runtime_metrics_tests.cpp`；链接 support、Catch2WithMain 和 `Threads::Threads`。 |

各依赖的实际版本、来源、许可证和固定提交记录到 README 的依赖表；不能只写“最新版”或 ZIP 文件名。fkYAML/spdlog 采用保留目录结构的固定头文件源码，确认许可证一并保留；Catch2 的提交必须真实可核查；qlib 按用户本次授权以本地内容清单固定，不要求不存在的 Git 提交。

构建入口按以下顺序组织。这里是最终 V0 的实现清单，不表示这些文件已经存在或命令已经验证：

1. `cmake_minimum_required(VERSION 3.28)` 与 `project(qbook LANGUAGES CXX)`；设置 C++23、要求标准支持、关闭编译器扩展，并生成 compile_commands。
2. `include(CTest)` 提供默认开启的 `BUILD_TESTING`；定义默认关闭的 `QBOOK_BUILD_SERVICE` 和 `QBOOK_BUILD_BENCHMARKS`。V0 中任一个被显式设为 ON 时立即 `FATAL_ERROR` 说明对应能力尚未实现，不能静默忽略请求。
3. 接入 qlib；将 `QLIB_BUILD_TESTS` 明确关闭并加入子目录。确认生成的是实际 `qlib::qlib`，不改写成不存在的安装导出包。
4. 检查固定 fkYAML/spdlog 头文件和许可证位置，再建立两个 INTERFACE 目标；仅从实际源码位置暴露 include 路径，不猜测系统导出目标名。
5. `find_package(Threads REQUIRED)`；建立包含真实源文件的 `qbook_support`，公开本项目 `include/` 和 `qlib::qlib`，配置/日志依赖保持 PRIVATE。
6. BUILD_TESTING 为 ON 时，接入固定 Catch2 源码、建立 qbook_tests，并 `include(third_party/Catch2/extras/Catch.cmake)` 后调用 `catch_discover_tests(qbook_tests)`。必要的用例超时是防挂起保护，不是产品停止时间承诺。
7. 维护 `.gitignore` 排除 build、可再生测试输出和运行数据；不把手写源码、测试、文档和固定依赖源码误排除。

在 V0-02 的中间状态，qbook_tests 先只含 `contracts_tests.cpp`，直接链接 qlib 和 Catch2；V0-03 增加 Config 实现与测试，V0-04 再加入 Runtime/Metrics 实现与测试。每次更新仍复用同一个 `build/debug`，不创建四个任务专用构建目录。

#### 3.1.4 模块关键算法的实现方案

本模块不编写容器算法。它需要把 qlib 的真实内存约束交给配置模块：当前 `single_queue<T>` 通过对齐的 `operator new` 分配 `capacity() * sizeof(T)`，槽类型就是 T，没有另一个带状态的 Slot 结构。FeedQueue 与 RecordQueue 的字节检查分别使用 `sizeof(qlib::feed_event)`、`sizeof(qlib::record)`，不能使用 `sizeof(single_queue<T>)`。

实际容量先向上取 2 的幂，再用于字节乘积；结果必须可由 `size_t`、`uint64_t` 表示且小于 `2^64`。计算校验在 Config 模块完成，不通过真的申请巨大队列来验证边界；V0-02 的小容量测试只确认已选择 API 的行为没有误解。

#### 3.1.5 模块并发与同步实现

公共类型不添加锁或原子状态。队列同步复用 qlib，主线程或测试在线程结束并同步前不能销毁它；第三方观察线程不能读取运行中的 `size()`/`empty()`。本版契约测试只覆盖依赖交接前提，具体 Feed/Book/Writer 协作测试由 V2/V3 承担。

全项目保留一致的 `QLIB_CACHE_LINE` 配置，本版使用已核对的默认 64。这个数字控制类型对齐，不检测硬件缓存行，也不构成“原子操作必定 lock-free”或整体服务无锁的证明。

#### 3.1.6 模块错误处理与恢复方案

缺少固定依赖或所需头文件时，在 CMake 配置阶段指明缺失位置；不下载未经记录的“最新版本”，也不改成跳过测试的成功构建。ABI 不符时停止对应构建，先确认依赖/平台差异，再决定是否需要变更协议，而不是自动修改断言。

修复依赖路径、实际源码或编译错误后，在同一个标准构建目录重新配置和验证。只有确有缓存、生成器或工具链冲突时才增加隔离场景，并记录原因；不把整个 build 树无条件删除作为常规恢复方法。成功只保留必要命令和结果摘要，失败保留定位所需的片段，具体记录和清理规则见第 5.2 节。

#### 3.1.7 模块实现顺序

1. 检查实际工作区和已有修改，确认当前文件对应哪个文档与任务；统一大小写引用，只把真实已形成的文档改为已存在状态。
2. 固定 qlib、Catch2 及本版配置/日志依赖来源；保留许可证，记录真实版本，不修改依赖实现。
3. 创建公共头和静态断言，建立仅含真实契约测试的 qbook_tests；以唯一标准目录配置、构建并运行已登记测试。
4. V0-03/V0-04 落地时，将其真实源文件逐步加入 qbook_support 和 qbook_tests，保持目标与文件一致。
5. 按第 5 章运行最终 V0 验收，更新 README 和简明版本记录；记录当前没有 qbook 主程序、网络服务或订单簿能力。

#### 3.1.8 模块测试与完成条件

V0-01 的完成证据是可定位的文档、真实依赖身份和一致的范围说明，不为纯文案写测试。V0-02 的完成证据是公共契约可编译、两种实际队列的小容量/FIFO 测试与满时不移动探针通过，CTest 实际发现并执行用例，且没有依赖尚未接入的 ZMQ、Arrow 或 UI。

最终构建还应包含 V0-03/V0-04 的真实测试；本模块通过不自动表示它们通过。所有命令、测试数量、通过结果和必要环境信息到实际执行后再记录；本次实现与测试结果见 releases/V0.md。

### 3.2 配置模块

#### 3.2.1 模块实现概述

1. **模块实现目标：** 完成 V0-03，让调用者从默认值或 YAML 文件得到一份经过验证的 `Config`；错误配置不能等到创建大队列或启动工作线程后才暴露。
2. **对应 Spec：** `specs.md` 4.1.2.1、5.1、5.4；对应 FR-01、BR-14、AC-01，以及 AC-12 的配置错误部分。
3. **类与文件：** 在 `include/Config.h` 声明 Config、LoadConfig、ValidateConfig，在 `src/Config.cpp` 实现读取与检查，新增 `config/config.yaml`、`tests/config_tests.cpp`。本版不声明或实现假的 RunService/main。
4. **依赖与复用：** 复用固定版本 fkYAML 解析器、spdlog 和 qlib 的记录类型；Config.h 本身只依赖标准库，Config.cpp 为检查队列字节数而包含公共类型。无须链接 ZeroMQ、Arrow 或 Parquet。

#### 3.2.2 Class Implementation（Config 及配套自由函数）

##### 3.2.2.1 类的实现目标

Config 只保存参数，不代表服务已经启动。默认构造给出初始值；LoadConfig 在局部对象上覆盖文件提供的字段，最后调用 ValidateConfig。只有完整成功后才把值交给调用者，不能暴露“半份配置”。

##### 3.2.2.2 类的声明与文件组织

头文件采用普通值类型，不添加单例、全局可变配置或虚函数；公开声明为：

```cpp
namespace qbook {
struct Config;
Config LoadConfig(const std::filesystem::path& path);
void ValidateConfig(const Config& config);
}  // namespace qbook
```

实际 `Config` 定义紧跟下一节的九个成员，不能只留下前置声明。容量及时间检查辅助函数放在 Config.cpp 的匿名命名空间，供本文件复用，不为测试而扩展生产公共接口。公共头文件不包含 YAML 或日志库。

本计划补全规格尚未逐键固定的文件表示：**YAML 使用平坦的九个 snake_case 键，与 Config 成员同名；两个周期字段使用整数毫秒。** 实施 V0-01/V0-03 时将此表示同步到 specs 和 README。这不增加配置项，也不增加命令行或环境变量覆盖。

```yaml
feed_endpoint: "tcp://127.0.0.1:5555"
l2: false
feed_queue_capacity: 16384
snapshot_period: 3000
record_queue_capacity: 16384
out_dir: "data_out"
batch_rows: 1024
metrics_endpoint: "tcp://127.0.0.1:5556"
sample_period: 100
```

允许只写部分已知键，`{}` 表示全部采用默认；存在但完全空白、解析成 null 的文件不等于文件缺失，应报根节点类型错误。根必须是映射，键必须为字符串，不接受额外嵌套段；未知键、重复键和显式 null 不静默忽略。

##### 3.2.2.3 成员变量的具体实现

| 成员 | C++ 类型 / 默认值 | 文件类型与读取规则 |
|---|---|---|
| `feed_endpoint` | `std::string` / `tcp://127.0.0.1:5555` | YAML 字符串；空值无效。 |
| `l2` | `bool` / `false` | YAML 布尔；不把字符串 `"false"`、整数 0 当布尔。 |
| `feed_queue_capacity` | `std::size_t` / `16384` | 正整数；检查符号和目标范围后才转换。 |
| `snapshot_period` | `std::chrono::milliseconds` / `3000ms` | 正整数毫秒；检查 rep 范围后才构造 duration。 |
| `record_queue_capacity` | `std::size_t` / `16384` | 同 feed 容量，实际字节检查使用 record 类型。 |
| `out_dir` | `std::filesystem::path` / `data_out` | YAML 字符串；空路径无效，V0 不创建或探测写入文件。 |
| `batch_rows` | `std::int64_t` / `1024` | 正整数；同时检查列表元素数和相应字节运算。 |
| `metrics_endpoint` | `std::string` / `tcp://127.0.0.1:5556` | YAML 字符串；空值无效。 |
| `sample_period` | `std::chrono::milliseconds` / `100ms` | 与 snapshot_period 采用相同受检转换。 |

**初始化与生命周期：** 使用成员默认初始化，字符串、路径及 chrono 值随 Config 构造和析构；不将默认构造标为 noexcept，因为字符串和路径可能分配内存。

**所有权与资源管理：** Config 按值拥有字符串和路径，不保存 YAML 节点引用或指向文件缓冲的 string_view。LoadConfig 的文件流、解析树和局部对象按作用域释放；返回后销毁解析器不会使配置失效。

**状态约束与不变量：** 默认对象本身还没有取得“已验证”的标记；LoadConfig 的所有返回路径均经过 ValidateConfig。验证成功不修改请求容量，也不把它提前替换为取整容量；后续可以同时报告“请求 3、实际 4”。Config 不在运行中被其他线程修改。

##### 3.2.2.4 重要函数的具体实现

###### A. LoadConfig

| 实现维度 | 本版落实方式 |
|---|---|
| 函数声明与职责 | `Config LoadConfig(const std::filesystem::path& path)`；完成默认、读取、类型检查、覆盖和最终验证。 |
| 输入、输出与契约 | 输入配置文件位置；输出独立且已经通过 ValidateConfig 的值。空输入路径属于调用错误，不能把它当“缺文件使用默认”。 |
| 核心算法与数据结构 | 用局部 `Config config{}` 和一份 YAML 映射处理固定九键；不建立动态配置注册表。每个字段先确认节点类型，再检查数值范围，最后写入目标成员。 |
| 具体执行步骤 | ① 建默认对象；② 检查路径状态；③ 仅明确不存在时 warn、验证默认并返回；④ 打开和解析已有普通文件；⑤ 检查根、键集合和每项类型；⑥ 按字段受检覆盖；⑦ ValidateConfig；⑧ 返回值。 |
| 成员读取与修改 | 只修改局部 config 中出现的字段，其余保留默认；不修改调用者状态、不写配置文件。 |
| 对其他函数的调用 | 调用固定版本 fkYAML 解析入口、标准文件读取、spdlog 的缺文件提示和 ValidateConfig；不调用队列构造、RunService 或网络方法。 |
| 边界与错误处理 | 状态查询错误、目录冒充文件、文件存在但打不开均报带路径的 I/O 错误；解析、根类型、未知/重复键、null、字段类型及范围错误报带键名的配置错误。超范围整数在解析器或字段类型/范围检查处明确拒绝，不能转为浮点或截断。 |
| 异常安全与状态一致性 | 文件/解析树自动释放，异常不返回部分配置；只把确定的缺文件视为默认分支。不要 catch 所有异常后返回默认，`bad_alloc` 等资源错误应继续传播。 |
| 时间与空间复杂度 | 对文件长度 L，读取和解析至少需要 O(L) 时间及 O(L) 临时空间；字段数量固定，覆盖和业务校验为常量级，不宣称第三方解析器对任意 YAML 都是严格线性。 |
| 并发与线程安全 | 按规格在启动线程调用，不提供热更新；本函数不拥有长期工作线程。文件被外部同时修改的确定性不在保证中，读取期间失败明确报错。 |
| 单元测试与验证 | 覆盖完整配置、缺项、`{}`、明确缺失文件、空文件、非映射根、非字符串键、未知/重复键、null、错误类型和非法数值；缺失文件应有提示，其他错误不得悄悄返回默认。 |

读取整数时不能先写 `static_cast<std::size_t>(signed_value)` 再检查负数：-1 会先变成极大正数。正确顺序是确认整数节点、提取解析器实际支持的整数、检查大于 0、检查 `std::in_range<目标类型>`，再构造成员。带引号的 `"16384"`、浮点 `16384.0` 和布尔值都不作为容量整数接受；同样不能把任意 YAML 标量转成 string 后绕过类型规则。

固定的 fkYAML 0.4.4 在映射插入发现重复键时会抛解析错误，实际行为由重复键夹具验证。超大无标签整数字面量可能被解析成字符串，配置层按字段类型拒绝。不能等解析器已经用后值覆盖前值后再声称检测了重复键，也不为此另写一套 YAML 解析器。

###### B. ValidateConfig

| 实现维度 | 本版落实方式 |
|---|---|
| 函数声明与职责 | `void ValidateConfig(const Config& config)`；校验已解析值及后续必需的数值运算。 |
| 输入、输出与契约 | 输入可能由文件或直接构造得到的 Config；成功无返回值，错误抛 `std::invalid_argument` 并指出字段。函数不把负数、零或空值自动修正。 |
| 核心算法与数据结构 | 固定字段检查；分别调用受检容量和时间转换；批量用除法界限先检查再乘法。无需额外容器或实际分配队列。 |
| 具体执行步骤 | ① 检查端点、路径非空；② 分别验证两条队列请求容量和真实值类型大小；③ 验证两个周期和当前到期点可表示性；④ 验证 batch_rows 正值、固定列表元素数及字节运算；⑤ 成功返回。 |
| 成员读取与修改 | 仅读九个成员，不改变请求值和默认值；临时保存取整容量、转换后的时间和同一次 steady_clock 观察值。 |
| 对其他函数的调用 | 调用 CheckedQueueCapacity、CheckedSteadyPeriod 及标准范围查询；使用 `sizeof(qlib::feed_event)`、`sizeof(qlib::record)`、`qlib::snapshot_depth`。不调用 Arrow、ZMQ 或 filesystem 创建目录。 |
| 边界与错误处理 | 容量 0、周期≤0、批量≤0、空端点/路径、无法表示的取整/乘法/到期时间均拒绝。端点最终是否能连接/绑定，以及目录能否创建/写入，留给实际所属阶段；不在 V0 假装验证网络可达。 |
| 异常安全与状态一致性 | 无业务资源变化，失败保持输入不变；错误信息构造可能分配，因此不声明 noexcept。数值检查成功仍不保证后续机器有足够内存。 |
| 时间与空间复杂度 | 固定字段和固定次数算术，正常路径 O(1) 时间、O(1) 额外空间；错误消息的字符串成本按实际长度计。 |
| 并发与线程安全 | 主线程在启动前调用；不依靠验证过程发布其他共享数据。当前时间检查只证明这次检查时刻，后续每次重新计算 next_deadline 仍需检查。 |
| 单元测试与验证 | 直接构造 Config 覆盖 0、负周期/批量、最大值附近和空值；用真实事件类型大小选择安全边界和首个不安全值，确认不会为这些测试实际申请巨大内存。 |

批量检查先确认 `batch_rows > 0`。设 `d = qlib::snapshot_depth`，当前 d 为 1：先检查 `batch_rows <= INT64_MAX / d`，再计算列表元素数；同时确认元素数可由 size_t 表示并不超过 `SIZE_MAX / sizeof(std::int64_t)`。标量列的行数与字节数按相同方式检查。这里保证计划中这些数量运算可表示，不尝试估算 Arrow 内部全部缓冲或证明整个批次一定能分配；实际 Arrow 分配与 I/O 错误留到 V3。

###### C. CheckedQueueCapacity（内部辅助函数）

| 实现维度 | 本版落实方式 |
|---|---|
| 函数声明与职责 | `std::size_t CheckedQueueCapacity(std::size_t requested, std::size_t value_size, std::string_view field)`；返回安全的向上取整容量，辅助校验而不分配。 |
| 输入、输出与契约 | requested 来自相应配置；value_size 由调用点传入 `sizeof(feed_event)` 或 `sizeof(record)`，必须非零；field 是错误字段名。成功返回 2 的幂容量。 |
| 核心算法与数据结构 | 先用 size_t 可表示的最大 2 的幂作为 bit_ceil 输入上界，再核对 uint64 计数范围和实际存储字节数。 |
| 具体执行步骤 | ① 拒绝 requested==0；② 若 requested 大于 `std::bit_floor(SIZE_MAX)` 则拒绝；③ 执行 `capacity = std::bit_ceil(requested)`；④ 用 `std::in_range<std::uint64_t>(capacity)` 检查可表示性；⑤ 若 `capacity > SIZE_MAX / value_size` 则拒绝；⑥ 返回 capacity。 |
| 成员读取与修改 | 不访问成员，只处理参数和局部整数；调用者不将结果覆盖进 Config 的请求容量。 |
| 对其他函数的调用 | `std::bit_floor`、`std::bit_ceil`、`std::in_range` 和数值上限；不构造 single_queue，不调用 operator new。 |
| 边界与错误处理 | requested 为 1 保持 1，3 得到 4；超出取整上界必须在 bit_ceil 前拒绝。最高可表示 2 的幂不大于 2^63（受 uint64 上限约束），满足 qlib 容量小于 2^64 的契约；字节乘法另行限制通常更严格。 |
| 异常安全与状态一致性 | 检查失败抛带字段名的 invalid_argument，所有输入不变；没有先溢出的乘法，也没有需要回滚的资源。 |
| 时间与空间复杂度 | 对固定机器字长，O(1) 时间和 O(1) 空间。 |
| 并发与线程安全 | 纯数值函数，无共享写入；正常用于单线程配置验证。 |
| 单元测试与验证 | 通过 ValidateConfig 验证 1、3、恰为 2 的幂、最大安全请求及边界外请求；小容量 3 的实际队列契约测试属于 V0-02，巨大值只测算术拒绝。 |

实际 qlib 队列源码是 `T* slots_`，调用 `operator new(capacity() * sizeof(T), alignof(T))`。所以容量检查的单位是 **事件变体的 sizeof(T)**，既不是 `sizeof(single_queue<T>)`，也不是臆造的槽结构大小。该依据随 qlib 固定版本记录；以后依赖改变存储布局时，必须重新核对配置检查。

###### D. CheckedSteadyPeriod（内部辅助函数）

| 实现维度 | 本版落实方式 |
|---|---|
| 函数声明与职责 | `std::chrono::steady_clock::duration CheckedSteadyPeriod(std::chrono::milliseconds period, std::string_view field)`；将正周期安全转换为调度时钟的 duration。 |
| 输入、输出与契约 | 输入已能由 milliseconds::rep 表示的值；输出能由 steady_clock::duration::rep 表示且大于零的时间差。它不读取系统墙钟，也不生成全局定时器。 |
| 核心算法与数据结构 | 从实际目标时钟的 `period` 得到毫秒到 tick 的比例，先检查乘除转换的界限再转换。当前 Linux/WSL2 常见纳秒 tick 时，先检查毫秒数不超过 tick 最大正值 / 1,000,000，再乘 1,000,000；此公式必须与实际编译目标的时钟类型核对。 |
| 具体执行步骤 | ① 检查 count>0；② 核对目标 duration 的 rep 和换算比；③ 用目标上限及比例做受检范围判断；④ 只在已证明运算安全后转换；⑤ 拒绝转换后为零的结果；⑥ 返回差值。 |
| 成员读取与修改 | 只读 period，使用局部整数；不修改 Config，不设置下一次触发点。 |
| 对其他函数的调用 | 复用标准 chrono、ratio 和 numeric_limits；可以在检查通过后使用 duration_cast，不能靠 duration_cast 本身检测溢出。 |
| 边界与错误处理 | 零和负周期拒绝；很大的正毫秒数可能在转纳秒时越界，同样拒绝。目标时钟单位不同于已核对环境时，按实际比例实现等价受检转换，不能仍无条件乘一百万。 |
| 异常安全与状态一致性 | 失败抛带字段名的 invalid_argument；没有溢出后的检查，没有已发布状态；不因时间参数非法而创建等待线程。 |
| 时间与空间复杂度 | 固定次数数值转换，O(1) 时间与 O(1) 空间。 |
| 并发与线程安全 | 无共享状态；ValidateConfig 调用后独立检查当前 now 与差值的和。业务阶段未来每次重新计算到期点均要重复和的范围检查。 |
| 单元测试与验证 | 在实际目标时钟表示下验证最小正值、最大可转换毫秒数及下一值；用可直接构造的时钟计数测试到期加法边界，不通过等待数百年或大规模 sleep 验证。 |

在 ValidateConfig 中读一次 `steady_clock::now()`，令当前计数为 n、已转换正差值为 d、时钟 rep 上限为 M。先检查 `n > M - d`，为真则拒绝该周期；通过后才允许计算 n+d。因为 d 已被证明为正且不超过 M，M-d 本身安全；这种写法也避免直接计算 `M - n` 时 n 为负产生溢出。**V0 的检查不能保证无限期运行后的时间加法永远安全**，因此 V2/V4 在每次安排下一到期点时仍必须使用同样的受检加法。

##### 3.2.2.5 构造、初始化与资源释放

Config 使用默认成员初始化；文件覆盖在局部值上完成。标准字符串和 path 自动管理内存；LoadConfig 的文件流和 YAML 树离开作用域自动释放。没有 Close、启动、停止或自定义析构函数；不将保存参数的类变成服务生命周期管理器。

##### 3.2.2.6 类内部的关键实现流程

例如文件只包含 `feed_queue_capacity: 3`：先得到九项默认配置，再把该请求容量覆盖为 3；ValidateConfig 计算实际容量 4，检查 `4 * sizeof(feed_event)` 可以表示，但不会分配队列；成功返回时 Config 中仍是请求值 3。未来启动阶段用请求值创建队列并报告实际容量 4，其余字段保持默认。

若文件写成 `feed_queue_capacity: -1`，解析器得到有符号整数后即拒绝，不能先转换成 size_t；如果文件把键误拼为 `feed_queue_capcity`，应报告未知键，不能因为原字段仍有默认值就悄悄启动。

##### 3.2.2.7 类的实现顺序与依赖关系

1. 先落地九个成员、默认值和头文件声明，并用直接构造验证默认含义。
2. 实现受检容量、周期和批量运算，再实现 ValidateConfig；这些检查不依赖文件解析。
3. 固定并接入 fkYAML/spdlog，按明确的九键表示实现 LoadConfig，所有路径复用 ValidateConfig。
4. 写配置示例和实际测试，最后更新文档中的字段表示与错误约定；不能让示例字段和解析器支持的字段各自演化。

#### 3.2.3 模块内部协作与调用流程

本版测试调用 `LoadConfig(path)`，由它创建默认 Config、覆盖文件字段、调用 ValidateConfig，再把结果交给测试核对。直接构造的 Config 可以单独送入 ValidateConfig 测边界；只在 V0-02 的小容量契约集成中实际构造队列，不靠配置加载副作用创建资源。

#### 3.2.4 模块关键算法的实现方案

关键顺序是“检查类型 → 检查范围 → 转换 → 检查后续运算 → 返回”。容量先检查 bit_ceil 的可表示上界，字节乘法先除法比较；周期先检查 duration 转换，再检查到期加法；批量先检查列表元素数，再检查字节数。所有计算使用真实 qlib 类型，不把数组深度、存储单位或缓存行宏当作记忆中的常数直接复制。

#### 3.2.5 模块并发与同步实现

本模块没有长期线程或原子状态，启动时完成配置后按值或 const 引用交给后续装配。不能把 Config 放成运行中可写的全局对象；UI 的 `NEXT_PUBLIC_METRICS_SAMPLE_PERIOD_MS` 属于后续 UI 构建参数，既不由本模块读取，也不覆盖 C++ 的 sample_period。

#### 3.2.6 模块错误处理与恢复方案

只有确定的文件不存在允许 warn 后使用默认。存在但不可读、内容无效、未知键或数值无效均使本次加载失败，调用者修正输入后可以重新调用；函数不缓存失败状态。I/O 错误保留路径与系统原因，配置错误保留键名和期望类型/范围，资源分配异常不伪装成配置缺失。V0 的测试核对这些异常，不提前建立服务进程返回码逻辑。

#### 3.2.7 模块实现顺序

按 V0-02 的公共类型和实际依赖完成后执行 V0-03：声明与默认值 → 数值验证 → 文件解析 → 示例与测试 → 文档同步。始终复用本计划统一的 `build/debug`，不为每个字段或错误样本创建新构建目录；配置测试使用隔离的临时数据目录并按测试作用域清理。

#### 3.2.8 模块测试与完成条件

| 测试组 | 必要场景与可观察结果 |
|---|---|
| 默认与覆盖 | 缺文件提示后得到默认；`{}`、部分配置、完整配置均得到准确字段；不存在的文件不会被测试顺手创建。 |
| 文件与结构 | 目录路径、不可读取、空文件、根序列、非字符串键、未知键、重复键和 null 都失败；失败消息可定位路径或字段。 |
| 类型 | 布尔、整数、浮点和字符串不相互隐式替代；超过解析器整数范围的输入有明确错误。 |
| 容量 | 0 拒绝；小容量 1/3/4 安全；取整边界和字节边界先检查再运算，不实际分配巨大队列。 |
| 周期与批量 | 非正值、目标 duration 转换边界、到期加法边界、列表元素/字节边界正确；合法参数保持原值。 |
| 独立性 | 连续加载两个不同文件不会残留前一份字段；加载失败后重新加载合法文件仍正常；调用不打开网络、不创建输出目录。 |

不可读文件测试要考虑执行身份：root 可能仍能读取 chmod 去掉权限的文件。只有实际复现拒绝读取时才将该场景记为通过；否则使用能确定失败的读取场景并说明权限场景的限制，不把未触发的错误当成已覆盖。

完成条件是：上述相关测试在统一构建目录实际执行通过，Config 默认值、YAML 九键、错误分类和边界公式与规格一致，AC-01 有真实结果；不保留重复成功日志，不声称已经验证 ZMQ 连接、Arrow 分配或正式服务启动。本次对应实现与验收结果见 releases/V0.md。

### 3.3 故障控制基础（V0-04 的 RunControl 部分）

#### 3.3.1 模块实现概述

1. **模块实现目标：** 实现一个全程存活的故障对象，让后续工作线程获知“本次运行已经失败”，并在所有线程结束后取得第一条失败诊断。V0 只实现和测试这个基础对象，不启动真实业务阶段。
2. **对应的 Spec 章节：** `specs.md` 4.1.2.2、5.1、5.2；对应 Sprint 的 V0-04、FR-02 的故障基础和 NFR-03。实际停止队列、文件与进程由 V2/V3 接入，不能在本版宣称正常保存已验收。
3. **需要实现的类与文件：** 在 `include/Runtime.h` 声明 `qbook::RunControl`，在 `src/Runtime.cpp` 定义其方法；验证放入 `tests/runtime_metrics_tests.cpp` 的独立 RunControl 测试组。V0 不增加空的 `RunService()`、`main()` 或信号处理函数。
4. **依赖与复用组件：** 使用标准库的 `std::atomic<bool>`、`std::exception_ptr` 和受控测试线程；构建加入 `qbook_support`，测试加入同一个 `qbook_tests`。本模块不依赖 ZMQ、Arrow、订单簿或日志框架。

#### 3.3.2 Class Implementation（RunControl 类实现）

**1. 类的实现目标。** 只保留一次运行的首个致命故障。运行中的其他线程只需要读取布尔标志以退出等待；错误内容由调用方在全部相关线程结束后报告。这里的“首个”是第一个赢得原子交换的报告者，不保证对应最早发生的物理故障或最早打印的日志。

**2. 类的声明与文件组织。** 头文件直接包含 `<atomic>` 和 `<exception>`，不通过其他业务头间接取得声明；源文件包含自己的头和实际需要的 `<utility>`。按如下接口组织实现：

```cpp
namespace qbook {
class RunControl {
public:
  RunControl() noexcept = default;
  RunControl(const RunControl&) = delete;
  RunControl& operator=(const RunControl&) = delete;
  RunControl(RunControl&&) = delete;
  RunControl& operator=(RunControl&&) = delete;

  void Fail(const char* stage, std::exception_ptr error) noexcept;
  bool Failed() const noexcept;
  std::exception_ptr Error() const noexcept;
  const char* Stage() const noexcept;

private:
  std::atomic<bool> failed_{false};
  std::exception_ptr error_{};
  const char* stage_ = nullptr;
};
}  // namespace qbook
```

实际接口位于 include/Runtime.h，与此声明一致。无需自定义析构函数，也不增加 Reset、重新启动或运行中查询详细错误的接口。

**3. 成员变量的具体实现。**

| 实现维度 | 具体安排 |
|---|---|
| 数据类型与容器 | `failed_` 是原子布尔值；`error_` 是持有异常对象的标准句柄；`stage_` 是非拥有字符指针。无需 map、错误队列或互斥锁。 |
| 初始化与生命周期 | 初始失败标志为 false、异常和阶段名为空。每次运行新建一个对象，在所有借用它的线程结束后销毁。 |
| 所有权与资源管理 | `error_` 的异常对象生命周期由 exception_ptr 管理；`stage_` 只允许指向静态存储期文本，如 `"writer"`，不得传临时 string 的 c_str。对象本身既不拥有也不 join 线程。 |
| 状态约束与不变量 | failed_ 只从 false 变成 true；只有交换得到旧值 false 的调用者写其余两项；失败详情一旦写入不再覆盖。看到 true 不代表详情已经写完。 |

**4. 成员函数的具体实现。** 以下各表逐项给出模板要求的函数契约与执行方法。

**`Fail()`：让第一个报告者写入诊断。**

| 实现项 | 方案 |
|---|---|
| 函数声明与职责 | `void Fail(const char* stage, std::exception_ptr error) noexcept`；设置失败并保留首条详情。 |
| 输入、输出与契约 | stage 必须是非空、静态存储期阶段名，error 必须是已捕获的非空异常；无返回值。允许多个线程竞争调用，调用期间对象必须存活。 |
| 核心算法与数据结构操作 | 对 failed_ 执行一次 `exchange(true, std::memory_order_relaxed)`；旧值为 true 就直接返回，旧值为 false 才拥有写入详情的资格。 |
| 具体执行步骤或伪代码 | ① 交换 failed_；② 已经失败则结束；③ `stage_ = stage`；④ `error_ = std::move(error)`。不在写入详情前额外发信号或调用回调。 |
| 成员变量的读取与修改 | 所有调用者只修改同一原子标志；获胜者随后写 stage_、error_。失败者不能读取或写入这两个普通成员。 |
| 对其他函数的调用 | 只使用 atomic::exchange 和 exception_ptr 的不抛移动赋值；异常创建/捕获发生在调用者。这里不调用 spdlog、字符串格式化、其他阶段或文件操作。 |
| 边界情况与错误处理 | 重复调用不会替换已有诊断。空异常或短生命周期阶段名属于调用契约错误，不能把它们加入正常路径；V0 的所有调用点及测试遵守前置条件，不通过引入抛错或 abort 处理它们。 |
| 异常安全与状态一致性 | 函数保持 noexcept；选定的成员赋值不抛异常。failed_ 先可见、详情后完成是特意允许的中间状态，因为运行中的观察者不读取详情。 |
| 时间复杂度与空间复杂度 | 固定次数原子与句柄操作，算法工作量 O(1)、额外空间 O(1)；不分配自定义容器、不等待其他线程。不据此承诺原子在所有平台都无锁或有硬实时耗时。 |
| 并发、同步与线程安全 | relaxed 交换的原子性保证只有一个获胜者；不使用这个标志发布详情。详情的可读性由调用方完成所有潜在报告线程的 join 来保证，不能靠把交换改成 release 就提前读详情。 |
| 单元测试与验证方法 | 先顺序报告 A、B，检查只保留 A；再让多个受控线程竞争，全部 join 后检查 Stage/Error 属于同一个实际报告者，不指定谁必须获胜。线程内不直接使用可能抛出的测试断言。 |

**`Failed()`：供工作循环检查退出条件。**

| 实现项 | 方案 |
|---|---|
| 函数声明与职责 | `bool Failed() const noexcept`；返回一次对失败标志的读取。 |
| 输入、输出与契约 | 无参数，返回 bool；允许在线程运行期间调用，调用者不能由 true 推导详细错误已可读取。 |
| 核心算法与数据结构操作 | 直接执行 `failed_.load(std::memory_order_relaxed)`，不读取异常或阶段名。 |
| 具体执行步骤或伪代码 | ① 原子读取 failed_；② 原样返回读到的值。不加入缓存副本或重试循环。 |
| 成员变量的读取与修改 | 仅读取 failed_；不修改任何状态。 |
| 对其他函数的调用 | 仅调用 atomic::load，不调用 Error/Stage。 |
| 边界情况与错误处理 | 新对象返回 false；已经观察到失败后，标志不会由本类重置。函数不提供“多少毫秒内一定观察到”的调度保证。 |
| 异常安全与状态一致性 | noexcept，无部分更新；任意正常返回不改变首条诊断。 |
| 时间复杂度与空间复杂度 | 一次读取，O(1) 时间、O(1) 额外空间；没有等待与动态分配。 |
| 并发、同步与线程安全 | 与 Fail 的交换并发安全；该读数只作取消提示，不承载其他内存的发布或资源销毁同步。 |
| 单元测试与验证方法 | 检查初始 false、顺序 Fail 后 true、竞争测试全部 join 后 true。可以让观察线程只读 Failed 来退出受控循环，但不通过无限忙等和精确时限证明内存模型。 |

**`Error()` 与 `Stage()`：在报告线程全部结束后读取详情。**

| 实现项 | 方案 |
|---|---|
| 函数声明与职责 | `std::exception_ptr Error() const noexcept` 返回异常句柄副本；`const char* Stage() const noexcept` 返回保存的静态阶段名。 |
| 输入、输出与契约 | 无参数；所有可能调用 Fail 的线程均已 join，或当前场景根本不存在并发报告者。没有失败时返回空句柄/nullptr；有失败时返回同一获胜者的一对信息。 |
| 核心算法与数据结构操作 | 分别直接返回 error_ 和 stage_；不增加原子包装，不根据 Failed 重试读取。 |
| 具体执行步骤或伪代码 | 调用方先完成全部 join，再调用 Stage 和 Error；需要展示异常时，由调用方在 try/catch 中 rethrow_exception，不在访问器中重新抛异常。 |
| 成员变量的读取与修改 | 只读取各自普通成员；Error 复制句柄但不改变 RunControl 的诊断选择。 |
| 对其他函数的调用 | 使用 exception_ptr 的不抛复制；本类不调用日志或线程接口。 |
| 边界情况与错误处理 | 无故障时必须先判断异常句柄是否为空，不能对空句柄调用 rethrow_exception。运行中读取详情违反前置条件，不能通过写一个可能发生数据竞争的测试来验证。 |
| 异常安全与状态一致性 | 两个方法均 noexcept；返回的句柄副本可延长所持异常的生命周期，阶段名自身具有静态生命周期。 |
| 时间复杂度与空间复杂度 | 固定数量成员读取与句柄复制，O(1) 工作量和额外空间；不建立诊断字符串副本。 |
| 并发、同步与线程安全 | join 将获胜线程的普通写入与之后调用方的读取建立先后关系；读取期间不得再启动会使用同一 RunControl 的新报告线程。 |
| 单元测试与验证方法 | 在无并发者的新对象上检查空值；在全部测试线程 join 后读取并重新抛出保存异常，核对异常内容和阶段名相匹配。验证后续 Fail 不覆盖已经写入的详情。 |

**5. 构造、初始化与资源释放。** 使用默认构造和默认析构；不在构造函数里捕获“空故障”，不启动线程。测试先创建 RunControl，再创建借用它的线程，结束时显式完成 join，最后才离开对象作用域。复制和移动删除，避免把正在被多个线程引用的对象换到另一个地址。

**6. 类内部的关键实现流程。** 例如两个测试线程分别报告 `"feed"` 与 `"writer"`：谁先通过 exchange 看到 false，谁写入详情；另一个线程只看到已有失败并返回。第三个观察线程可以据 Failed 结束等待，但不会读取 Stage。测试主线程等三个线程都结束后，才核对保存的是 feed 的完整一对信息或 writer 的完整一对信息。

**7. 类的实现顺序与依赖关系。** 先确定成员初始化和不可复制/移动约束，再实现 Fail、Failed，最后实现只有 join 后可用的访问器；先通过无并发的状态测试，再加入受控竞争。异常文本和线程标签在测试端准备，避免把测试调度机制塞进产品类。

#### 3.3.3 模块内部协作与调用流程

V0 测试创建 RunControl → 构造实际异常句柄 → 测试线程调用 Fail → 其他观察者只调用 Failed → 测试主线程 join → 检查 Stage/Error。V2/V3 会把测试线程替换为阶段入口包装，但本版不提前创建这些工作类。

#### 3.3.4 模块关键算法的实现方案

采用“一次原子交换选出唯一诊断写者”。普通错误成员无需锁的前提不是“它们不会同时写”，而是还必须保证**写入期间没有读者**；因此代码注释和测试都明确 join 后才能读详情。这个方案无需等待诊断就绪的第二标志，避免为并不存在的运行中详情查询引入额外状态。

#### 3.3.5 模块并发与同步实现

在 RunControl 内，failed_ 的交换和读取统一使用 relaxed；交换负责唯一写者竞争，join 负责详情的完成同步。不要把失败标志同时用于发布订单数据或判断其他资源已经销毁。测试线程通过标准同步设施协调起跑，失败检查和线程结束仍有明确边界；所有线程的异常都在测试驱动中接住，不允许异常逃出线程入口导致 terminate。

#### 3.3.6 模块错误处理与恢复方案

本对象只记录失败，不负责恢复；同一次运行失败后不清零重新使用。调用者构造或捕获异常的失败属于调用者处理范围。V0 的验证结论只证明故障记录可用，不等于 Writer 失败后整条流水线已能退出；解除队列等待、停止顺序和进程非零返回需在 V2/V3 的真实使用点验收。

#### 3.3.7 模块实现顺序

1. 在 `Runtime.h` 写声明、初值及读取前提的中文注释，确认没有后续版本的空实现。
2. 在 `Runtime.cpp` 实现一次交换、单次读取和 join 后访问器，加入 `qbook_support` 的实际源文件。
3. 在 `tests/runtime_metrics_tests.cpp` 加入初始状态、顺序重复故障和受控竞争三个测试组；测试完成后由测试主线程统一断言。
4. 使用标准 `build/debug` 和同一个 `qbook_tests` 验证，必要时按第 5 章的规则开启诊断工具；不要为本模块另建常规构建目录。

#### 3.3.8 模块测试与完成条件

必须实际验证初始状态、首条保留、竞争结果成对、全部 join 后可读取及对象不可复制/移动。可以用 `static_assert` 检查类型特征，但不能用类型检查代替并发行为测试。正常与竞争场景通过且所有调用点满足读取前提后，本模块才完成；本次对应测试已执行，结果见 releases/V0.md。成功记录只包含实际命令、用例数量、结果和必要环境，失败时保留定位所需信息，不保存可重新生成的完整编译日志。

### 3.4 原子指标基础

#### 3.4.1 模块实现概述

1. **模块实现目标：** 完成 V0-04 中的 `Metric`、`Metrics` 和 `SnapshotMetrics()`，使后续阶段可以安全更新各自的数字，观察者可以取得普通值副本。本版只建立指标数据基础，业务调用点在 V2/V3 落地，发布线程在 V4 落地。
2. **对应的 Spec 章节：** `specs.md` 4.5.1、4.5.2.1、4.5.2.2、5.2，对应 FR-13 的基础部分和 NFR-05；这里不宣称 FR-14 的实际抽样或 FR-15 的网络发布已经完成。
3. **需要实现的类与文件：** 在 `include/Threads/MetricsThread.h` 声明 `Metric`、`Metrics`、`SnapshotMetrics()`，在 `src/Threads/MetricsThread.cpp` 实现函数。虽然沿用最终规划文件名，V0 不添加空的 `MetricsThread` 类；测试写入共用的 `tests/runtime_metrics_tests.cpp`，由 `qbook_tests` 执行。
4. **依赖与复用组件：** 依赖标准库 `<atomic>`、`<cstdint>`、`<limits>`、`<stdexcept>` 和 qlib 的 `common.h`；实现编入 `qbook_support`。不包含 ZMQ、Arrow、订单簿或队列实例，也不从构建系统引入它们。

#### 3.4.2 Class Implementation（类实现）

##### 3.4.2.1 Metric

**1. 类的实现目标。** `Metric` 表示一个可以被观察的非负整数。一个线程负责更新这个单元，其他线程只能读取；例如未来 Feed 更新 `feed_messages`，监控线程读取它。这里的“原子”保证单次访问不会发生普通整数那样的读写竞争，不会自动把多次操作变成一个整体。

**2. 类的声明与文件组织。** 在头文件中保持规格声明，定义放在对应 `.cpp` 中；类位于 `qbook` 命名空间。需要的头文件显式包含，不依赖别的头文件偶然间接引入。

```cpp
namespace qbook {
struct alignas(QLIB_CACHE_LINE) Metric {
  void Add(std::uint64_t n = 1);
  void Set(std::uint64_t n) noexcept;
  std::uint64_t Read() const noexcept;
  std::atomic<std::uint64_t> value{0};
};
}
```

保留规格中的公开成员 `value`，但项目调用者应使用三个操作函数，不绕开它们任意重置累计量。原子成员使默认复制和移动不可用；不添加逐字段复制原子的构造函数来绕过这一限制，也不为本版增加与规格不同的公共接口。

**3. 成员变量的具体实现。**

| 维度 | 具体实现 |
|---|---|
| 数据类型与容器 | 唯一成员为 `std::atomic<std::uint64_t> value{0}`，不使用容器、字符串索引或动态注册表。 |
| 初始化与生命周期 | 每个新单元从 0 开始，随所属 `Metrics` 对象生存；不能在其他线程仍访问时销毁。 |
| 所有权与资源管理 | 按值拥有一个原子对象，不借用订单地址，不分配业务存储；复制和移动保持不可用。 |
| 状态约束与不变量 | 累计计数只允许唯一写者使用 `Add`；最近状态值使用 `Set`，允许下降。`Add` 溢出时保持旧值；正常运行中不通过 `Set(0)` 重置累计量。 |

`alignas(QLIB_CACHE_LINE)` 采用当前 qlib 的宏约定，默认值为 64。所有翻译单元使用一致宏值，不能把不存在的 `qlib::cache_line_size` 当接口，也不能将这个配置值描述为已经探测到的硬件缓存行大小。对齐会增加每个指标占用的空间；V0 检查实际 `alignof/sizeof`，不声称对齐已经带来某个性能收益。

**4. 成员函数的具体实现。**

**A. `Add`：增加累计量。**

| 实现项 | 具体要求 |
|---|---|
| 1. 函数声明与职责 | `void Metric::Add(std::uint64_t n)`；默认参数 `= 1` 只写在头文件声明，增加一个非负工作量。 |
| 2. 输入、输出与契约 | 输入为 uint64 增量，无返回值；本单元必须只有一个写者，旧值加 n 可表示时更新成功。 |
| 3. 核心算法与数据结构操作 | 先读取旧值，再用 `n > max - old` 检查越界，最后保存总量；检查不能放在加法之后。 |
| 4. 具体执行步骤 | ① `old = value.load(relaxed)`；② 若 `n > UINT64_MAX - old`，抛 `std::overflow_error`；③ `value.store(old + n, relaxed)`。n 为 0 也满足同一流程。 |
| 5. 成员变量的读取与修改 | 读取 `value` 一次；成功时写入一次，失败分支不写入。 |
| 6. 对其他函数的调用 | 调用标准原子 load/store 和数值上限；不调用时钟、日志、网络或 `RunControl::Fail`。后续阶段包装器负责处理异常。 |
| 7. 边界情况与错误处理 | 验证 0 增量、恰好到最大值及超过最大值；不允许无符号静默回绕变成小值。 |
| 8. 异常安全与状态一致性 | 正常路径不分配；越界路径抛出错误且旧值不变，异常对象自身的建立由标准库管理。不标记 noexcept，也不捕获后假装计数成功。 |
| 9. 时间复杂度与空间复杂度 | 正常路径固定工作量 O(1)、额外空间 O(1)；不将复杂度等同于固定纳秒延迟或保证底层无锁。 |
| 10. 并发、同步与线程安全 | load/store 均使用 relaxed；它们不发布其他业务内存。单写者条件使读取后写回不会覆盖另一写者的更新；不能用多写者测试暗示这种用法受支持。 |
| 11. 单元测试与验证方法 | 单线程验证默认增量、指定增量、0 和 uint64 上界；唯一写线程累计 N 次、读线程并发 Read，join 后结果应恰好为 N。不要求每次中途读取都得到最新值。 |

**B. `Set`：替换最近观察状态。**

| 实现项 | 具体要求 |
|---|---|
| 1. 函数声明与职责 | `void Metric::Set(std::uint64_t n) noexcept`；将状态指标设为指定值。 |
| 2. 输入、输出与契约 | 接受 uint64 的任意值，无返回值；唯一写者负责保证该单元的业务口径允许替换。 |
| 3. 核心算法与数据结构操作 | 一次原子 store，不做读取、累加或比较循环。 |
| 4. 具体执行步骤 | 调用 `value.store(n, std::memory_order_relaxed)`，然后返回。 |
| 5. 成员变量的读取与修改 | 只写 `value`，不读取和修改其他单元。 |
| 6. 对其他函数的调用 | 仅调用原子 store；不清零整个 Metrics，不触发发布。 |
| 7. 边界情况与错误处理 | 0、最大值和低于旧值的新值均可表示；下降对状态量合法，不代表服务重启。 |
| 8. 异常安全与状态一致性 | 不分配、不抛异常；单次原子写入，不出现部分整数。 |
| 9. 时间复杂度与空间复杂度 | 时间 O(1)，额外空间 O(1)。 |
| 10. 并发、同步与线程安全 | 与 Add 相同，单元只有一个写者；观察者可并发 Read。relaxed 不同步订单簿状态。 |
| 11. 单元测试与验证方法 | 按 9→4→0→最大值设置并读取，确认允许下降；独立核对累计字段的使用说明没有安排运行时重置。 |

**C. `Read`：读取单个指标。**

| 实现项 | 具体要求 |
|---|---|
| 1. 函数声明与职责 | `std::uint64_t Metric::Read() const noexcept`；返回一次原子读取的值。 |
| 2. 输入、输出与契约 | 无参数，返回 uint64；调用者保证对象存活。返回值不代表全部指标同一时刻的状态。 |
| 3. 核心算法与数据结构操作 | 一次原子 load，不加入重试或等待。 |
| 4. 具体执行步骤 | `return value.load(std::memory_order_relaxed);`。 |
| 5. 成员变量的读取与修改 | 读取 `value`，不修改对象和任何计数。 |
| 6. 对其他函数的调用 | 仅调用原子 load，不读取队列或业务容器。 |
| 7. 边界情况与错误处理 | 初始返回 0；写者运行时可以读到允许的中间值，不据此报丢失。 |
| 8. 异常安全与状态一致性 | noexcept、无分配、无状态变更。 |
| 9. 时间复杂度与空间复杂度 | 时间 O(1)，额外空间 O(1)。 |
| 10. 并发、同步与线程安全 | 允许多个观察者并发读取；不从这一值推导其他普通成员已经可见或资源已经可以释放。 |
| 11. 单元测试与验证方法 | 使用 const 引用读取初始及更新结果；并发测试只要求合法范围，全部写者 join 后再断言最终确切值。 |

**5. 构造、初始化与资源释放。** 保留隐式默认构造，成员初始化器将 value 设为 0；不写需要网络或运行时注册的构造函数。通过类型特征验证 `Metric` 不可复制、不可移动；析构只销毁原子，前提是所有借用者已经结束。本版由测试作用域和 join 管理生存期。

**6. 类内部的关键实现流程。** 累计值为 5 时调用 `Add(3)`，函数先读取 5，确认可表示后保存 8；观察者读取 5 或 8 都可接受。假如旧值已是 uint64 最大值，`Add(1)` 抛错，之后读取仍为最大值。这里不以“计数不重要”为由允许回绕。

**7. 类的实现顺序与依赖关系。** 先落地声明和零初始化，再实现 Read/Set，随后实现含越界检查的 Add；完成边界和单写者测试后再用它组装 Metrics。代码顺序不要求把这几步拆成新的 Sprint 任务。

##### 3.4.2.2 Metrics 与 SnapshotMetrics

**1. 类的实现目标。** `Metrics` 为各阶段提供固定命名的 16 个单元。`SnapshotMetrics()` 将它们逐一读出，形成可以独立保存、比较和后续传输的 `qlib::metrics`；本地原子集合与传输结构明确分开。

**2. 类的声明与文件组织。** 在 `include/Threads/MetricsThread.h` 中声明一个只包含下表 16 个 `Metric` 成员的 `struct Metrics`，不添加构造参数、字符串查找或统一 `Update`。紧接着声明自由函数，在对应 `.cpp` 实现：

```cpp
qlib::metrics SnapshotMetrics(const Metrics& source,
                              std::int64_t sample_ns) noexcept;
```

`Metrics` 保持聚合；原子成员间接使它不可复制和移动，不需要为了禁止复制而给聚合添加自定义构造函数。时间戳只属于输出记录，不在共享 Metrics 中再维护一个原子时间。

**3. 成员变量的具体实现。** 每个下列成员的类型都是 `Metric`，名称与 qlib 输出字段一一对应；“写者”是后续实现时的唯一写入归属，V0 用受控测试代替真实业务阶段。

| 本地成员 → 输出字段 | 后续唯一写者 | 更新含义 |
|---|---|---|
| `feed_messages → feed_messages` | Feed | 收到的完整消息数，包含拒绝消息；Add。 |
| `feed_bytes → feed_bytes` | Feed | 接收消息内容的字节数，不含网络开销；Add。 |
| `feed_orders → feed_orders` | Feed | 筛选通过的订单及撤单数，入队前计数；Add。 |
| `feed_trades → feed_trades` | Feed | 筛选通过的成交数；Add。 |
| `feed_levels → feed_levels` | Feed | 筛选通过的 L2 数；Add。 |
| `feed_dropped → feed_dropped` | Feed | 入站格式、字段或模式拒绝数；Add。 |
| `book_events → book_events` | Book | 完成应用的事件数，包含业务忽略；Add。 |
| `book_apply_ns → book_apply_ns` | Book | 完成的抽样应用耗时累计纳秒；Add。 |
| `book_samples → book_samples` | Book | 完成的应用抽样次数；Add。 |
| `book_instruments → book_instruments` | Book | 快照观察时已建立的品种数；Set。 |
| `book_resting_orders → book_resting_orders` | Book | 快照观察时本地逐笔挂单数，L2 为 0；Set。 |
| `book_memory_bytes → book_memory_bytes` | Book | 约定口径的订单簿存储估计；Set。 |
| `writer_orders → writer_orders` | Writer | 成功追加的订单与撤单记录数；Add。 |
| `writer_trades → writer_trades` | Writer | 成功追加的成交记录数；Add。 |
| `writer_levels → writer_levels` | Writer | 成功追加的 L2 记录数；Add。 |
| `writer_books → writer_books` | Writer | 成功追加的快照记录数；Add。 |

- **数据类型与容器：** 固定 16 个 Metric，不用 vector/map，不将原子集合的对象字节当作网络格式。
- **初始化与生命周期：** 每个成员依 Metric 的初始化器为 0；集合在借用它的所有线程之前创建，在全部访问者结束后销毁。
- **所有权与资源管理：** 按值拥有各单元，外部阶段借用引用；Snapshot 返回的值独立于原集合，不持有其中地址。
- **状态约束与不变量：** 每字段只有一个写者，计数按定义累加；三个 Book 状态量允许 Set 降低。字段之间允许短暂不一致，不能添加 `feed_messages == book_events` 这样的全局不变量。

**4. 成员函数的具体实现（含配套自由函数）。** Metrics 无自定义业务成员函数；隐式初始化的验证与逐字段映射的实现分别处理。

| 实现项 | SnapshotMetrics 的具体要求 |
|---|---|
| 1. 函数声明与职责 | `qlib::metrics SnapshotMetrics(const Metrics& source, std::int64_t sample_ns) noexcept`；把本地原子集合转换为普通值记录。 |
| 2. 输入、输出与契约 | 输入为存活的 Metrics 和调用者提供的 Unix epoch 纳秒采样时间；返回独立 `qlib::metrics`。函数保存完整 int64 时间，不自行读取时钟，也不把时间转换为浮点数。 |
| 3. 核心算法与数据结构操作 | 创建零初始化的输出，按字段名分别读取 16 个 Metric，将 sample_ns 写到 ts_ns；不按数组下标或原始内存复制。 |
| 4. 具体执行步骤 | ① `qlib::metrics out{}`；② `out.ts_ns = sample_ns`；③ 按上表顺序逐项执行 `out.field = source.field.Read()`；④ 返回 out。每字段仅读取一次。 |
| 5. 成员变量的读取与修改 | 只读 source 的 16 个单元，写本次局部 out；不清零原计数，不写共享时间戳。 |
| 6. 对其他函数的调用 | 调用 16 次 Read；不调用 Add/Set、时钟、队列 size/empty、Orderbook 或网络发送函数。 |
| 7. 边界情况与错误处理 | 全零输入仍是有效值快照；高于 `2^53` 的整数和接近类型上界的值逐字段保持不变。不在本函数计算速率或均值，样本数为零不引发除零。 |
| 8. 异常安全与状态一致性 | 输出初始化、标量赋值和 Read 均无异常；不分配、不抛错、不改变源对象。返回结果可在源对象销毁后继续使用。 |
| 9. 时间复杂度与空间复杂度 | 固定 16 次读取，时间 O(1)，额外空间为一个 qlib::metrics，O(1)；无需建立历史缓存。 |
| 10. 并发、同步与线程安全 | 可与符合单写者约定的更新并行；各字段可能来自不同瞬间，函数不提供整组事务快照。精确逐字段断言在写者停止并 join 后执行。 |
| 11. 单元测试与验证方法 | 给 16 字段设置互不相同的值，逐项断言映射；时间使用超过 `2^53` 的 int64 值，另检查 uint64 最大值完整保留。修改源对象后旧快照不变；并发采样只检查各字段合法性，不断言跨字段比例瞬时一致。 |

**5. 构造、初始化与资源释放。** 使用 `Metrics metrics{}` 建立集合，检查所有字段为 0；不添加 `Reset()` 或按位复制接口。用类型特征检查不可复制、不可移动。测试中先结束并 join 所有写者与观察者，再让集合离开作用域；普通 qlib 输出值则可以正常复制。

**6. 类内部的关键实现流程。** 例如 `book_apply_ns` 已更新但 `book_samples` 尚未更新时，快照可能跨越这两个操作。V0 只保证两项分别读取安全，不把二者强行组成一致数据，也不增加互斥锁来修改规格。V4 的页面据此处理零分母和监控近似；准确性能实验在 V5 独立测量。

**7. 类的实现顺序与依赖关系。** 先完成 Metric，再按 qlib::metrics 的字段清单声明 Metrics，然后实现逐字段 SnapshotMetrics，最后加入映射、独立副本及并发读取测试。构建中仅把当前源文件加入 qbook_support，不提前声明尚未实现的网络阶段。

#### 3.4.3 模块内部协作与调用流程

测试创建 Metrics → 唯一测试写者调用某字段的 Add/Set → 观察者通过 SnapshotMetrics 取得普通值 → 全部线程 join → 在稳定值上检查精确结果。RunControl 测试与指标测试放在同一测试源中可以共享框架，但生产 Metric 不反向依赖 RunControl；后续阶段的最外层包装才负责把计数异常转为运行失败。

#### 3.4.4 模块关键算法的实现方案

1. **先检查再相加。** 用 `n > max - old` 判断超界，确保计算总量前已经知道其可表示；不得先做回绕加法再事后识别。
2. **固定字段显式映射。** 每个 `out.field` 只从同名 `source.field` 读取，避免字段错位或遗漏；测试用各不相同的值识别抄写错误。
3. **对象表示与传输分开。** Metrics 中的对齐和原子属于本地同步布局；本函数只生成 qlib::metrics，不直接 memcpy 共享对象。V0 的 ABI 契约测试验证输出类型，V4 才实现网络编码和跨语言检查。

#### 3.4.5 模块并发与同步实现

读写数字使用 relaxed 原子操作，因为计数不承担业务对象发布或停止同步。`Add` 的“读、检查、写”依靠唯一写者成立；不将它包装成可支持任意并发更新的通用计数器。正式线程尚未建立，V0 使用小规模受控线程验证这一真实调用模式。

并发测试通过明确的开始协调和完成条件运行：一个写者累加固定次数，一个或多个读者只读，结束后 join 并核对最终结果。不在工作线程中直接执行可能跨线程破坏测试框架状态的断言；把观察结果存入各自独占记录，主测试线程在 join 后判断。记录目标平台的 `is_lock_free()` 结果作为环境信息，不将“原子可用”扩大成“整个服务无锁”。

#### 3.4.6 模块错误处理与恢复方案

可表示的指标更新没有失败返回；Add 越界抛错并保持旧值。V0 的测试捕获并核对这一异常，后续工作线程将由公共异常边界报告失败；本模块不自行打印错误、忽略计数或恢复为 0。错用多写者、并发销毁或读取不存在对象属于调用契约错误，不靠在每次访问时加锁来掩盖。

#### 3.4.7 模块实现顺序

1. 在头文件声明 Metric、16 字段 Metrics 和 SnapshotMetrics，编译检查所需 qlib 宏与类型可见。
2. 在对应实现文件完成 Read、Set、Add 和 SnapshotMetrics；加入 qbook_support 的实际源文件列表。
3. 在 `tests/runtime_metrics_tests.cpp` 增加零值、越界、对齐/类型特征和逐字段映射测试，再增加合法单写者并发读取场景。
4. 使用本计划第 5.2 节的唯一常规 `build/debug` 目录运行相关测试和本版回归；结果只登记实际命令、用例数量和必要环境。清理按本计划统一规则执行，不单独制造第二套报告或构建目录。

#### 3.4.8 模块测试与完成条件

测试必须覆盖：16 个初始零值；Add 默认增量、0、最大值和越界不改值；Set 可下降；const Read；对齐与不可复制移动；16 项不同值的正确映射；超过 `2^53` 的时间与计数；输出副本独立；单写者与观察者并发后可得到确定的最终值。

所有必要测试实际通过、构建无需 ZMQ/Arrow/UI、注释明确单写者与非一致快照后，才可完成 V0-04 的指标基础部分。V0 的完成记录仍应明确：真实 Feed/Book/Writer 插桩、处理耗时抽样、MetricsThread 和监控页面均未实现。本次对应测试已执行，结果见 releases/V0.md。

## 4. Implementation Sequence（编码顺序）

### 4.1 模块实现先后顺序

| 阶段 | 对应任务 | 具体执行动作 | 结束条件 |
|---|---|---|---|
| A：基线归位 | V0-01 | 读取实际文件，记录工作区和已有目录；统一文档名；固定四类依赖来源和版本；检查许可证与包含路径。 | 可以说明每个依赖来自哪里；没有覆盖用户改动；实际接入方式可复取。 |
| B：真实测试入口 | V0-02 | 写 PipelineTypes 和 ABI/队列契约测试；只登记当前存在的源文件；建立 Catch2 与 CTest。 | `qbook_tests` 可执行，能够发现并通过当前契约用例。 |
| C：配置行为 | V0-03 | 声明 Config；实现数值检查、ValidateConfig，再接 YAML 读取与覆盖；加入配置测试和示例。 | 默认、合法覆盖、缺文件与错误路径都有明确结果。 |
| D：故障与指标 | V0-04 | 实现 RunControl、Metric、Metrics 与 SnapshotMetrics，加入受控并发与映射测试。 | 首错误保留、join 后读详情、单写者指标和 16 字段映射正确。 |
| E：版本核对 | 四项任务 | 在同一目录运行全部用例；核对文档与构建开关；填写最小结果记录并按归属清理产物。 | 满足第 5.5 节，四项任务才可在 Backlog 标记完成。 |

每阶段需要验证新引入的行为；已经通过且没有受后续改动影响的检查不重复扩展到多个环境。最后一次全量 V0 测试用于检查这些模块组合后仍一致，而不是重新创建一份项目或构建目录。

### 4.2 类与关键函数的实现依赖

| 实现项 | 必须先有的内容 | 可以独立完成的部分 |
|---|---|---|
| PipelineTypes、ABI 检查 | 固定 qlib 头文件与目标。 | 不需要 Config、RunControl、网络或持久化。 |
| Config 默认字段 | 规格字段和单位。 | 不需要打开 YAML 文件或创建任何业务资源。 |
| ValidateConfig | 安全整数、容量和时间检查规则；公共队列元素类型。 | 可以直接构造 Config 进行边界测试，不依赖 YAML。 |
| LoadConfig | Config 默认值、ValidateConfig、固定 YAML/日志依赖。 | 文件读取与字段检查失败时只销毁局部值，无业务状态需要回滚。 |
| RunControl | 标准库 atomic、exception_ptr 和线程测试入口。 | 与 Config、Metrics 独立，不等待生产 RunService。 |
| Metric | qlib 对齐宏与 std::atomic。 | 独立验证 Add/Set/Read 后再组装集合。 |
| Metrics、SnapshotMetrics | Metric、qlib::metrics 的完整字段表。 | 显式输入采样时间，不需要时钟线程或指标端点。 |

例如 Add 先通过边界测试，SnapshotMetrics 才使用 Read 做字段映射；这样字段错误不会和原子累加错误混在一起。LoadConfig 同样先使用已验证的 ValidateConfig，避免完整文件和缺项配置走出两套合法性规则。

### 4.3 分阶段可运行结果

1. **完成 B 后：** 可以运行契约测试，确认公共数据和所选 ABI；不存在 `qbook` 服务程序，也没有订单簿操作。
2. **完成 C 后：** 测试可以用临时 YAML 得到 Config 或收到预期错误；不需要在端口上启动假服务来验证配置。
3. **完成 D 后：** 测试可以创建短期线程报告错误并 join，也可以读取独立指标值；不声明整个采集链已经具备停止保存能力。
4. **完成 E 后：** 当前支持库和全部 V0 测试可复验，README 提供真实有效命令；以结果记录证明完成，不要求保留编译产物。

### 4.4 实现中的主要风险与处理方案

| 风险 | 实施中的具体处理 | 检查点 |
|---|---|---|
| qlib 与文档引用的版本不同 | 先核对源码和提交；发现不兼容时处理版本选择或显式更新设计，不改断言掩盖差异。 | V0-01/02 的依赖和 ABI 检查。 |
| 校验时就发生整数溢出 | 先比较上界再进行窄化、bit_ceil、乘法和时间相加，不通过真实巨大分配探测数值合法性。 | Config 的边界测试。 |
| YAML 值被宽松转换 | 先检查节点类型，整数和布尔严格按选定契约读取；未知和重复键必须拒绝。 | 字符串数字、浮点、null、重复键等固定文件。 |
| `Failed()==true` 被当成诊断已经可读 | 所有运行线程只读 Failed；测试与未来 main 都在全部报告者 join 后读取 Error/Stage。 | RunControl 的并发测试与公共注释。 |
| 指标 load/store 被误用为多写者累加 | 每指标只允许一个写者；测试验证该合法模型，不要求两个写者同时 Add 还能正确。 | Metric 注释、使用边界和测试设置。 |
| 头文件带入未来依赖 | 当前指标文件不包含 ZMQ；support 不链接 Arrow/Parquet；测试与服务构建开关分开。 | 当前 CMake 目标依赖。 |
| 权限测试在 root 下失真 | 不只靠 chmod 后打开文件来模拟不可读；使用确定性读取失败或非普通文件场景，并明确运行身份。 | config 测试实际命中的错误路径。 |
| 为重复验证积累构建和日志 | 常规始终使用 build/debug；仅为具体配置隔离创建临时目录，成功只记录摘要。 | 第 5.2 节清理清单与版本报告。 |

## 5. Verification（验证与验收）

### 5.1 单元测试

以下是应覆盖的行为组，不预填最终 Catch2 用例数量。实现者可以用数据驱动用例合并相似边界；不能为了凑编号复制同一断言，也不能仅逐个调用私有函数而不验证外部结果。

| 编号 | 位置 | 输入或操作 | 必须检查的结果 |
|---|---|---|---|
| UT-01 | `contracts_tests.cpp` | 编译公共头，核对数据类型、枚举、倍率、快照深度。 | 与固定 qlib 一致，调用端不复制另一套公共数据。 |
| UT-02 | 同上 | 编译期检查小端、指针宽度、standard-layout、sizeof/offsetof。 | 支持的 ABI 与规格逐项相符；不支持时明确编译失败。 |
| UT-03 | 同上 | 申请容量 3，连续投入 4 项，第 5 项失败，再逐项取出。 | 实际容量 4、全部槽位可用、FIFO 正确；不采用“预留一个空槽”的错误假设。 |
| UT-04 | 同上 | 满队列 push 使用可观察移动的局部测试值，检查队列类型特征。 | 失败 push 不移动参数；队列不可复制、不可移动。无需重测 qlib 全部内部算法。 |
| UT-05 | `config_tests.cpp` | Config 默认构造、完整文件、只覆盖一个字段、空映射。 | 九字段默认和覆盖正确，未提供字段保持默认；不创建队列、套接字或输出目录。 |
| UT-06 | 同上 | 路径不存在、存在却读取失败、空文档/错误根类型、未知或重复键。 | 只有真正缺文件可警告后回退默认；其余按本计划契约明确报错，不伪装为默认加载成功。 |
| UT-07 | 同上 | null、布尔/整数/字符串类型混用、浮点容量、窄化前超界值。 | 报错能定位字段或解析位置，没有静默截断或负数转成巨大无符号数。 |
| UT-08 | 同上 | 零容量、最大可取整边界、slot 字节乘积越界；合法小容量 1/3/4。 | 在危险运算之前拒绝不可表示数值；合法小容量与真实队列行为一致。 |
| UT-09 | 同上 | 零/负/过大周期与批量，时间换算/相加边界、空端点/路径。 | 无回绕或超大分配，错误可识别；不把验证成功当成将来绑定成功或内存足够。 |
| UT-10 | `runtime_metrics_tests.cpp` | 新 RunControl；顺序两次 Fail。 | 初始健康且详情为空，第一条 stage/error 保留，第二条不覆盖。 |
| UT-11 | 同上 | 多个线程在受控开始点后竞争 Fail，随后全部 join。 | 结果匹配某一个完整候选的 stage/error 对，不要求固定线程获胜；运行期间不读普通诊断字段。 |
| UT-12 | 同上 | Metric 默认、Add()、Add(0)、Set、Read。 | 初值为 0，默认增量为 1，状态量可下降，读不改变值。 |
| UT-13 | 同上 | 设置到 uint64 边界后做合法增量和溢出 Add。 | 最大合法结果可表示；溢出抛 overflow_error 且旧值保持。 |
| UT-14 | 同上 | 一个写者反复 Add、一个或多个读者 Read，最后 join。 | 合法并发模型下运行正确，join 后最终值等于确定总量；不从一次瞬时观测推断其他字段同步。 |
| UT-15 | 同上 | 16 项分别赋不同值，传入固定 sample_ns 后 SnapshotMetrics。 | 时间和每个字段准确映射，含大于 2^53 的值；返回值与源独立，之后改变源不改变旧快照。 |
| UT-16 | 同上 | 检查对齐、禁止复制/移动的类型特征及独立字段读取。 | 使用实际 QLIB_CACHE_LINE；不把对象大小当作传输帧大小，不声称多字段完全一致。 |

测试线程中的错误应先保存到可安全回传的结果中，主线程 join 后断言；不能让未经捕获的断言或异常直接逃出线程导致 `std::terminate`，从而掩盖真正错误。不要解引用失效对象或并发读非原子字段来“测试”调用前置条件。

### 5.2 集成测试

**常规构建目录与前提。** 所有命令从 qbook 仓库根目录执行。标准目录只使用 `build/debug`，任务内各阶段重复构建都复用它。V0-01 已记录本目录在任务开始前是否存在；若已有缓存属于其他生成器、工具链或配置，先说明差异，不删除用户目录来强行复用。

下面是常规验证命令，本次实际执行与结果见 releases/V0.md。四个固定依赖需已按第 1.4 节取得，CMake 文件需已实现所述选项；不是让缺失依赖时绕过检查：

```bash
c++ --version
cmake --version
ninja --version

cmake -S . -B build/debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DQBOOK_BUILD_SERVICE=OFF \
  -DQBOOK_BUILD_BENCHMARKS=OFF \
  -DQLIB_BUILD_TESTS=OFF

cmake --build build/debug --parallel

ctest --test-dir build/debug --show-only
ctest --test-dir build/debug --output-on-failure --no-tests=error
```

`--show-only` 用于确认注册结果，不算执行通过；最后一条才实际运行测试。日常修正后通常只需重新 build 和运行受影响测试；CMake 或编译选项改变时再运行 configure，不机械重复安装依赖或查看版本。

**成功标准。** 配置和编译返回 0，CTest 发现实际用例且覆盖当前已实现的契约/配置/运行与指标三组，所有本次要求的检查通过。V0 最终验收时三组必须都存在；不能以部分测试已经通过代替尚未注册的测试。构建成功不等于 qbook 服务已经可运行。

**跨组件集成。** 测试将合法 Config 的小容量交给真实 FeedQueue/RecordQueue，核对请求容量到实际容量的关系；使用同一个 qlib 版本生成公共事件并核对 ABI；将 Metrics 的 16 项取值转换成普通 qlib::metrics；用短期线程报告异常并在全部 join 后读取 RunControl。V0 不需要网络、磁盘采集或浏览器集成场景。

**配置隔离场景按需增加。** 仅当本任务确实新增/修改了相应 CMake 分支，或存在具体缓存与编译器问题时，才进行下面的额外配置检查；它们不是每次功能修改后都要重复跑的第二套常规构建：

| 需要解决的问题 | 允许的额外检查 | 结果与清理要求 |
|---|---|---|
| `BUILD_TESTING=OFF` 是否仍意外要求 Catch2 | 在 V0-03/04 已建立真实 support 后，于最终阶段 E 使用确认不存在的 `build/v0-check-no-tests` 配置并构建。 | 不创建 qbook_tests，不获取测试依赖，support 可构建；记录结果后清理本任务新建目录。 |
| 当前服务开关是否被静默忽略 | 在新建 `build/v0-check-service` 中设置 `QBOOK_BUILD_SERVICE=ON`。 | 配置按计划明确失败，说明 V0 尚无正式服务；这是预期拒绝测试，不能写作服务构建通过。 |
| 当前 benchmark 开关是否被静默忽略 | 在新建 `build/v0-check-bench` 中设置 `QBOOK_BUILD_BENCHMARKS=ON`。 | 配置按计划明确失败，说明 benchmark 尚未实现；清理本任务新建目录。 |
| 工具链/缓存冲突或具体内存、线程问题 | 先写出要排除的问题，再选择一个专用临时目录。 | 只增加必要检查，记录实际选项；不要把所有 sanitizer、编译器和 Release 组合变成 V0 默认门槛。 |

若 V0-02 首次建立这些开关，服务与 benchmark 的拒绝分支属于本任务需要验证的新增行为；`BUILD_TESTING=OFF` 的支持库构建检查则放在 V0-03/04 已产生真实 support 源码后的阶段 E，不在 V0-02 要求尚不存在的库。以下命令分别在对应条件满足、临时路径确认不存在时执行一次：

```bash
cmake -S . -B build/v0-check-no-tests -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF \
  -DQBOOK_BUILD_SERVICE=OFF -DQBOOK_BUILD_BENCHMARKS=OFF \
  -DQLIB_BUILD_TESTS=OFF
cmake --build build/v0-check-no-tests --parallel

cmake -S . -B build/v0-check-service -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF \
  -DQBOOK_BUILD_SERVICE=ON -DQBOOK_BUILD_BENCHMARKS=OFF \
  -DQLIB_BUILD_TESTS=OFF

cmake -S . -B build/v0-check-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF \
  -DQBOOK_BUILD_SERVICE=OFF -DQBOOK_BUILD_BENCHMARKS=ON \
  -DQLIB_BUILD_TESTS=OFF
```

最后两个命令的预期结果是配置拒绝；执行者分别检查退出码和清楚的原因，不将它们拼进一个遇到首次失败就跳过后续检查的模糊脚本。若目录已存在，不能套用本次新建的清理规则；应选一个事先记录、确实不存在的临时名称或复用可解释的现有配置。

**成功记录的最小内容。** 在唯一的 `docs/releases/V0.md` 中记录：实际代码与依赖标识、操作系统/编译器/CMake 的必要版本、执行命令、CTest 实际用例数量及通过数、预期拒绝场景结果、未执行项、完成后的目录清理情况。未提交工作区可以写“未提交＋修改文件清单”，不能编造提交哈希。无需保存成功构建的 stdout 全文、依赖下载内容、环境变量全集、重复 JSON/Markdown 报告或每条成功断言截图。

**失败时保存什么。** 保留导致失败的命令、首个相关诊断和上下文、失败用例名、可复现输入及必要工具链信息；只有定位确实需要完整日志时才暂存一份。修复后运行受影响检查，再按本版完成条件复核；仍失败或依赖不可用时明确标为失败/阻塞，并列出尚未执行的检查。不要通过删除测试、吞掉异常或降低输入校验把失败改成通过。

**临时产物与目录清理。**

1. 开始前记录 `build/debug` 和可能使用的临时路径是否已经存在；只把本任务确实新建的产物列入清理清单。已有目录和其内容由用户管理，不自动删除。
2. 配置测试使用独占临时目录，测试结束后以作用域清理删除自己创建的 YAML、普通文件和子目录。失败重现文件如仍有价值，保留最小样本并记录用途，不保留整份可重新生成的目录。
3. 本任务新建的 `build/v0-check-*` 或其他诊断目录在相应检查结束后删除；校验绝对路径确实位于当前项目约定位置，逐个按清单操作，不用通配符批量删除未知目录。
4. 常规验证始终复用 `build/debug`。若它也是本任务从无到有新建的，最终验证和记录结束后按模板要求清理这份可再生构建产物；若任务开始前就存在，则保留。不要为证明清理成功再重新编译，把同一批产物重新生成。
5. 清理本任务产生且已无诊断用途的完整成功日志、下载缓存和解压中转内容。固定的第三方源码、源码、测试、配置示例、手写文档、最小验收记录和用户已有修改都保留。
6. 最终报告清理了哪些本任务路径、保留哪些原有目录及哪些必要失败证据；不通过删除源码或未授权 Git 操作获取“干净工作区”。

### 5.3 边界与异常测试

**配置边界。** 必须分别验证“缺字段使用默认”和“字段存在但值非法”；明确区分空映射与空文档。测试能够触发 bit_ceil、槽字节数、周期换算、下一时刻相加及批次计算的边界，禁止在检查前进行会溢出的表达式。合法但过大的资源请求与数值不能表示分开：V0 验证表示范围，不创建真实巨大队列或 Arrow 缓冲。

**错误记录。** 顺序 Fail 验证首条不被覆盖，竞争 Fail 验证只有一个候选完整获胜。测试不能假定同时开始时某个线程必然先执行，也不能在 Failed 为 true 后、join 前读取 stage_ 或 error_；这种测试本身违反契约。

**指标边界。** `Add(0)` 不变，最大合法值可读，越界抛出且旧值保持；Set 用于状态值，允许下降。SnapshotMetrics 逐字段核对，不要求正在更新时 `book_apply_ns` 与 `book_samples` 来自同一瞬间；暂时不一致不是这个设计下的测试失败。

**异常传播与恢复。** 文件或解析错误不能落入缺文件默认路径；内存不足等系统错误不可被包装成合法默认配置。V0 不设计运行中恢复：加载失败让调用者终止该次启动，RunControl 失败后不复位，指标越界由未来工作线程异常边界处理。当前测试只验证这些组件契约，不声称已完成整个服务的故障退出。

### 5.4 性能测试与测量方法

V0 不增加 benchmark、不运行 perf、不报告纳秒耗时或吞吐目标。这里只核对结构成本与资源边界：配置仅启动时解析；公共别名不复制数据定义；RunControl、Metric 和 SnapshotMetrics 正常路径不构造动态容器；指标采样为固定 16 次读取。

可以记录目标平台上原子是否 lock-free，但结果不能扩大为整个服务无锁；不能因为 alignas(64) 就声称实测降低了延迟。如果出现明确的启动卡顿、异常分配或测试挂起，先定位该问题，必要的诊断检查按第 5.2 节记录。正式基准实验与收益对照由 V5 处理。

### 5.5 模块实现验收

| 任务 | 本版通过标准 | 当前结果 |
|---|---|---|
| V0-01 | 文档链接/状态准确，固定依赖来源和真实标识可核查；修改范围清楚。 | 已完成，见 releases/V0.md |
| V0-02 | 当前源码可构建，公共契约和队列关键集成用例实际通过；无未来依赖或空服务目标。 | 已完成，见 releases/V0.md |
| V0-03 | AC-01 的配置行为和必要异常通过；示例九键、默认值、单位及校验规则一致。 | 已完成，见 releases/V0.md |
| V0-04 | RunControl、Metric、Metrics 的关键顺序/并发/溢出/映射测试通过；生命周期规则明确。 | 已完成，见 releases/V0.md |
| V0 版本 | 四任务满足全局 DoD；实际用例数与结果已记录，文档一致，清理已按来源执行。 | 自动化验收通过，30/30 测试及构建分支验证完成 |

版本验收后只将本版实际覆盖的需求部分标记完成。FR-02 的整个服务启停、FR-13 的真实业务计数，以及 AC-16 的完整运行复验继续由后续版本承担。

### 5.6 与 Spec 的一致性检查

| 规格约定 | 实现核对点 |
|---|---|
| 复用 qlib 类型和队列 | PipelineTypes 的别名、CMake 链接目标及当前版本一致。 |
| 固定 ABI 不是通用协议 | 所有必要布局断言齐全，不发送 variant 或本地原子指标的对象内存。V0 只检查，不实现发送。 |
| Config 不拥有业务资源 | LoadConfig/ValidateConfig 不创建套接字、队列、线程或运行输出目录。 |
| 缺文件警告，存在但坏文件失败 | 检查读取错误分类及示例 YAML，不以 catch-all 使用默认值。 |
| 首失败标志与详情读取分离 | Fail 赢家写普通字段；所有潜在报告者 join 后才读 Error/Stage。 |
| 指标单写者、relaxed | 每项独立原子；不以普通 uint64 共享，也不把 load+store 当作多写者累加。 |
| 16 项与 qlib::metrics 对应 | 明确赋值，不通过 memcpy 复制 Metrics；sample_ns 原样成为 ts_ns。 |
| V0 仅实现基础 | 没有生产阶段、RunService、UI、benchmark 或伪完成测试。 |
| 文档与构建实际一致 | README、AGENTS、Backlog 同步已存在内容，未实现功能继续标明版本边界。 |

本计划补全的 YAML 表示、内部检查步骤和构建目标属于实施细化。落实 V0-01 时将这些具体约定同步到对应规格位置，保留既有业务语义与默认值，不修改 requirement 来迁就程序。

## 6. Plan Review（实现方案复核）

### 6.1 是否覆盖 Spec 中的设计

本计划覆盖 specs 2.4、3.2～3.4、4.1.2.1～4.1.2.2、4.5.2.1～4.5.2.2 中属于 V0 的设计；运行组装、工作阶段、协议接收、业务、文件和 UI 明确留到后续版本。Config 与 Metrics 是数据集合，不为满足模板虚构成员方法；配套自由函数按同样详细程度说明。

本节说明方案覆盖；实际实现已逐项核对，独立评审和测试结果见 releases/V0.md。

### 6.2 算法与数据结构是否可行

容量校验依据锁定队列的实际 T 槽分配，不猜测私有 slot 布局；整数和时间运算先检查再计算。RunControl 依赖原子 exchange 选出唯一详情写者，并依赖 join 提供详情可读时机；Metric 的 load/store 依赖唯一写者。任何一项前提变化都需要回查设计，不能只局部修改一行内存序或类型。

配置、故障和指标基础均不需要额外容器框架。未完成的后续类不进入构建目标，当前支持库的每个源文件都具有实际用途。

### 6.3 依赖与编码顺序是否合理

公共契约先于配置容量检查和指标映射，Metric 先于 Metrics，ValidateConfig 先于文件覆盖组合，测试入口先于实际功能验收。后续才加入的网络和 Arrow 依赖不会阻塞 V0。固定源依赖与已有源码在执行前核对，无法取得时只记录真实阻塞，不伪造备用接口或自动换库。

### 6.4 性能、资源与线程安全风险

当前主要风险是整数检查写晚、解析错误被当成缺文件、错误详情读取过早以及误用多写者指标；相应测试已分配。原子对齐只表达内存布局策略，不是测得性能收益。所有测试线程结束后再释放共享对象，测试临时文件按所有权清理，不影响用户原有数据。

配置验证能防止不可表示的资源请求，不能保证实际内存充足；后续创建资源仍必须处理分配失败。当前计划不承诺任意调度或操作系统调用的硬实时期限。

### 6.5 待确认的实现问题

| 项目 | 当前结论 | 处理时机与影响 |
|---|---|---|
| qlib 的实际来源和内容版本 | 用户确认采用 `/mnt/d/Desktop/unsw/qlib` 当前源码，无 Git 提交。 | V0-01 复制固定快照并生成逐文件 SHA-256 清单；验收复核副本与原目录。 |
| Catch2、fkYAML、spdlog 的确切版本与源码形式 | 已固定 Catch2 3.8.1、fkYAML 0.4.4、spdlog 1.17.0；来源见 README。 | V0-01/02 落实并核对许可证；不根据 ZIP 名编造提交。 |
| 用户实际仓库和已有构建目录 | 本次已检查实际工作区，初始无 build 和实现源码。 | 执行前记录工作区、已有目录和缓存；决定可复用目录与清理清单。 |
| 工具链对 C++23 和对齐原子的实际支持 | GCC 13.3.0 / CMake 3.28.3 构建和公共契约测试已通过。 | V0-02 的实际编译和契约检查确认。 |
| YAML 节点表示及重复键行为 | 本计划冻结九个平坦 snake_case 键和整数毫秒；类型严格，重复键拒绝。 | 已用固定 fkYAML 0.4.4 验证严格类型和重复键拒绝，不自写第二套 YAML 扫描器。 |
| 时钟与极大周期的算术限制 | 按第 3.2 节做可表示性检查；未来每次 deadline 计算仍需检查。 | V0-03 验证算术；生产调度在对应版本落实，不能由一次启动检查保证无限期安全。 |
| 真实行情、UI 和绝对性能目标 | 本版范围外。 | 仍保留各自 OI 状态，不阻塞明确的本地 V0 基础实现。 |

这些待确认项需要在实现记录中给出实际事实，不要求对每个常规选择再次询问。若缺失资料确实阻止固定依赖或会改变契约，说明具体缺口和已完成工作，再请求必要信息。

## 7. Change Log（变更记录）

| 文档版本 | 日期 | 修改人 | 内容 |
|---|---|---|---|
| 0.1 | 2026-10-20 | 项目负责人提出目标，Codex 起草 | 按上传模板建立 V0-01～V0-04 实现计划，细化工程契约、Config、RunControl、Metric/Metrics；落实单一标准构建目录、最小验证记录和按来源清理产物的规则。当前仅完成计划，未实现或执行 qbook 构建测试。 |
| 0.2 | 2026-10-20 | Codex | 按现有计划完成 V0；依用户授权用本地 qlib 源码与 SHA-256 清单固定依赖，校正 docs 路径和六份计划状态。实际验收见 releases/V0.md。 |
