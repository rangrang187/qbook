# V5 实验与复验结果

2026-10-27，本轮实际实现四个benchmark、确定性负载与独立校验、16列CSV/manifest、分析器及测试构建目录选择。正式矩阵53个case，53个独立预热轮和265个测量轮均已执行；264个测量轮有效，1个L2稳态轮未满足冻结条件。**不能声明整个稳态矩阵通过；OI-04绝对性能门槛未定，V5完整验收保持待定。**

## 环境、身份和证据

环境与冻结规则见[实验设计](V5-experiment.md)。本机为开发容器/WSL2、i7-13650HX、GCC13.3.0，输出位于D:的9p/DrvFS挂载。Release使用`-O3 -DNDEBUG`、C++23、IPO/LTO OFF；实际链接和生产调用反汇编已核对。未锁频、未固定CPU亲和性、未清缓存；下列结果是该环境的批均值，不是单事件P99、最大服务容量或磁盘fsync速度。

本地原始证据根为 `results/benchmarks/V5-20261027/`，不提交大批实验数据；本地验收命令与失败过程另记 `docs/releases/V5.md`。关键内容：

| 路径 | 内容 |
|---|---|
| `initial-workspace.json`、`frozen-parameters.json` | 初始文件/目录、Git状态、正式参数SHA-256 |
| `smoke-01/`、`smoke-02/`、`failures/` | 未覆盖的早期配置/窗口/浏览器失败证据 |
| `smoke-03/`、`pilot/` | 53项有效smoke及53项全规模单轮试跑；均不作正式成绩 |
| `baseline/{core,protocol,writer,pipeline}/` | 每个程序的manifest、原始16列CSV、全部预热/正式轮校验；Pipeline另含worker、窗口和接纳账本 |
| `baseline/execution.json`、`baseline-analysis.md` | 真实命令、退出码和完整统计；Pipeline与分析器因无效轮返回1 |
| `build-audit.json`、`*-call-sites.txt`、`cmake-isolation.json` | 编译/链接/调用核对及轻量、无测试分支 |
| `perf-availability.json` | 实际`perf --version`失败：工具未安装，PMU/热点N/A；未改变系统策略 |

基线源码身份为 `fnv1a64:0136e4bc57630eaf`，Git基础`e2b1474ac87d74a12ce1a235889d8651ac326743`、dirty=true；四个程序分别保存自身二进制及构建身份。源码摘要涵盖源码、测试、实验参数、构建配置、依赖清单和UI锁文件，排除报告及生成物。FNV是可追溯内容标识，不是安全校验；固定依赖仍由SHA-256验证。

## 正式结果

每case固定1轮独立预热+5轮测量。以下数值为5个有效批均值的中位数与最小～最大；完整各阶段数据、IQR和慢轮均在分析器报告。没有删除失败轮或挑最快值。

| case / 阶段 | 每工作单位中位数 | 最小～最大 | 含义 |
|---|---:|---:|---|
| core.add.growth / apply | 57.788 ns | 49.807～112.866 ns | 8192次真实新增、包含增长分配 |
| core.modify.same_price / apply | 22.454 ns | 14.061～37.512 ns | 1024活单、65536次绝对量交替更新 |
| core.modify.same_price.large / apply | 33.375 ns | 21.180～36.853 ns | 8192活单；同一调用数 |
| core.modify.existing_price / apply | 42.491 ns | 34.703～54.803 ns | 已有目标价档，保留额外锚定单 |
| core.modify.new_price / apply | 109.080 ns | 90.028～267.776 ns | 1024次进入新价档 |
| core.cancel.full / apply | 119.871 ns | 96.293～129.934 ns | 1024个活单各删除一次 |
| core.cancel.full.large / apply | 41.413 ns | 39.097～81.828 ns | 8192个活单各删除一次 |
| core.snapshot.many / snapshot | 16.924 ns | 16.643～17.240 ns | 总1024单分布32簿，每簿买卖各1档 |
| protocol.valid.order / decode | 10.742 ns | 10.330～11.834 ns | 131072次解码，包含循环及结果消费 |
| protocol.mixed / decode | 6.131 ns | 5.002～11.330 ns | 256帧固定合法/拒绝混合工作集 |
| protocol.mixed.large / decode | 8.001 ns | 4.892～15.914 ns | 4096帧工作集；不能据重叠范围声称缓存瓶颈 |

准备、独立验证、析构另列纯耗时。快照32品种轮次中位541.569ns与每调用16.924ns来自同一批，不是两份独立测量。

受控链路有限1024事件的completed-cycle中位：Mbo 70.047µs/原事件，L2 75.163µs/原事件。网络有限输入同阶段定义下finite-completion中位：Mbo147.708µs，L2 98.287µs。它们包含实际异步批次处理和声明的完成观察，不是单事件延迟；受控接纳段极快也不能替代Writer完成率。`completed-window`使用N−1，停止/Close阶段另列，所有同轮区间仅在各自进程的单调时钟域相减。

Mbo稳态5/5满足冻结条件，在1024事件/秒输入下Writer正式完成窗口的速率中位1023.970间隔/秒。L2为4/5有效，**不将幸存4轮的中位数当成该case已完成的正式成绩**。全部正式链路轮都已正常Close/join并核对原事件和快照；网络`accepted_count=null`，不把发送、解码或追加数当成接纳账本。

### L2稳态无效轮

`baseline/pipeline/manifest.json`的`pipeline/network-steady/l2`、`trial_index=2`：两个相邻worker正式窗分别长201.345127ms和201.036990ms，Feed/Book各增加206条，Writer先增加141、再增加271条，速率700.290/s与1348.011/s均超出冻结的768～1280/s。Book−Writer估计从0到65再回0，两个窗均越过64条漂移限制。

采样读取跨度968/753ns、采样迟到7.46/8.50ms均合格；parent全部窗口有效、最大发送迟到0.916ms。Writer观察停在2559（20×128−1），与第2560条Add触发B128批写时停顿、随后追平的形态相符；这是批写/调度解释的证据，未取得perf，不能断言全部时间耗在磁盘或ZSTD。

该轮6160条L2原事件、122张快照逐字段核对成功，正式区间在保护窗内完成，正常关闭成功。**文件正确性通过不等于稳态有效**；整轮6个阶段均标invalid。全部正式995行中989行valid、6行invalid，分析器报告完整有效汇总=false并返回1；原始失败和成功轮均保留，没有放宽条件或重跑直到通过。

## 瓶颈调查与优化取舍

### Writer批量敏感性

同样的记录与写入路径，仅比较已冻结的B1024和B16配置；这是配置对照，**不是生产代码A/B**。物理row group数来自文件metadata，而非用应用批次数推算。

| 输入 | B | 实际row groups | 五文件字节数 | Add中位 | Close中位 | total中位 |
|---|---:|---:|---:|---:|---:|---:|
| orders 8193条 | 1024 | orders 9 | 164825 | 157.319ms | 97.148ms | 288.848ms |
| orders 8193条 | 16 | orders 513 | 1370715 | 3541.420ms | 1251.083ms | 5034.830ms |
| mixed各类型2049条 | 1024 | 每类3，总15 | 184031 | 74.892ms | 141.367ms | 278.303ms |
| mixed各类型2049条 | 16 | 每类129，总645 | 1360745 | 3191.674ms | 1644.606ms | 4796.766ms |

小批使文件约增至8.316倍/7.394倍、total中位约增至17.43倍/17.24倍。实际`ParquetWriter::Flush`每批Finish列、ValidateFull、构造Table、`WriteTable(rows)`、Reserve；固定Arrow26头文件说明该参数限制row group大小。metadata及代码支持“更多物理组增加编码、页和footer工作”的解释，但当前没有CPU/磁盘细分采样，不能将比例全部归因于某一层。

生产默认已经是B1024，无需改业务代码即可使用该配置。候选`WriteRecordBatch`会把刷新移到`NewBufferedRowGroup`/Close，并改变缓冲预算、错误出现阶段和关闭成本；本轮没有足够证据支持承担这些契约变化，未实施该候选，也不编造A/B收益。B−1时Add快、Close承担尾批，与B恰好触发刷新是不同工作，不能只用Add速度评价完整文件保存。

### Orderbook工作集和删除

`Orderbook::Erase`调用固定qlib的`get_iterator`，底层`locate`逐块查找，因此不能把删除普遍宣传为严格O(1)。然而本次全删8192单的每操作中位反而低于1024单，访问顺序、块逐步回收、短批及调度都会影响结果；现有数据没有证实块定位是主要瓶颈。同价修改的不同规模范围也重叠，更多价档的case中位没有单调上升。

因此没有依据替换容器、加入反向指针或预分配到改变增长成本；这类改动还牵涉移动/地址稳定性、异常回滚和内存代价。**V5-03已完成调查，最终不保留生产性能优化。** 保留的生产差异仅为私有冷路径生命周期观察，默认hook为空；没有Feed每事件探针。没有perf热点或硬件计数，也没有声称“未测优化已提升性能”。

## 功能及最终复验

本轮最终Debug完整167/167（5.47s）；最后增加生命周期证据检查后，常规Release含新增34项`benchmark::`，完整201/201（6.24s）；此前200/200仅是阶段结果。Python34/34，Node构建路径工具5/5。UI锁定安装、类型检查及153/153单元测试通过；100ms、2000ms分别重新生产build后真实Chromium各3/3，覆盖四状态、精度、请求关闭和观察端离线后继续业务与关闭文件读回。默认页面已恢复100ms。

最终独立新建`build/v5-final`：配置成功、307步完整构建成功、发现并运行201/201（5.81s），全部四程序53项smoke有效；使用`QBOOK_TEST_BUILD_DIR=build/v5-final`、已恢复100ms的UI生产构建，真实Chromium3/3通过（1.0min）。该轮没有回退旧程序，真实ZMQ→Node→浏览器及正常Close后的文件核对均通过。

正式基线后的唯一C++变更是在所有线程join后核对网络worker生命周期冷标记，并为重复/缺失/倒序/早返回增加测试；它不改变业务、负载、计时边界或热路径。最终源码身份`fnv1a64:d37ccf0d08125754`，各新二进制、编译/链接参数和系统`/usr`动态库解析保存于`final-build-audit.json`与`final-smoke-summary.json`。没有把新smoke当作正式性能重测，也没有用新构建成功覆盖旧L2无效轮。

真实浏览器精确显示`book_events=9007199254741003`，覆盖6组固定帧、connecting/live/stalled/offline及请求反复释放。页面和Node离线后发布第7条业务，三组浏览器的文件均为orders5、trades1、cancels1、levels0，books随真实时间增加，最终买一100/7、卖一101/6；具体每组行数、PID退出和读回结果见`browser-prior-summary.json`、`browser-final-summary.json`。C++服务及验证器退出0，Next由测试SIGTERM结束，退出143为其信号退出约定，未误写为全部进程退出0。

必要隔离构建也已执行：BUILD_TESTING=OFF下纯core/protocol、PERSISTENCE且无网络的Writer、完整服务加四benchmark均构建成功；编译/链接图没有Catch2、测试驱动或全局new替换进入生产/实验。无目的的sanitizer矩阵未追加。

| 需求/场景 | 可复验的证据与边界 |
|---|---|
| FR-01、UC-01、AC-01 | config测试、正式进程缺失/错误配置与初始化失败 |
| FR-02、UC-05/06、AC-11/12、NFR-02/03 | 四阶段服务SIGINT/SIGTERM、部分启动失败、尾批失败、Writer/Metrics故障解除背压；受控成功push账本与末条记录 |
| FR-03/04、AC-02、NFR-06 | 独立golden、非对齐/污染、模式校验、真实Feed非法帧恢复与multipart |
| FR-05～10、UC-02/03、AC-03～08、NFR-01 | Orderbook固定序列、FIFO/溢出/移动/分配失败；Book周期空簿与时间；服务Mbo/L2真实文件链 |
| FR-11/12、UC-07、AC-09/10 | 五类schema/整数/顺序/ZSTD/全部chunk；各B边界、Close/碰撞/真实故障；受控账本逐字段独立读回 |
| FR-13～17、UC-04、AC-13/14、NFR-05 | 16指标映射、Book抽样边界与计时排除Forward；真实PUB/SUB；UI153项及两周期Chromium，Writer Add不等于文件关闭 |
| NFR-04、AC-15 | 53case冻结参数、全部轮次、严格分析器；L2稳态1轮invalid，OI-04绝对门槛待定 |
| NFR-07、AC-16 | Debug/Release完整回归、无测试依赖隔离、最终全新构建与其真实浏览器；最终结果单独记录 |
| NFR-08 | 受控本机端点与文件权限边界；未验证公网部署、认证或不可信多租户安全 |

OI-02/OI-03真实行情源兼容仍未实测；未验证其他CPU/操作系统/ABI、长期容量、断电安全、跨重启恢复或跨类型文件全局顺序。仅Linux x86-64 Chromium本轮通过，不推广到其他浏览器。单次Next冷HTTP就绪超时的底层原因未确定，原失败保留；后续保持原期限通过，不以加长超时掩盖问题。

## 清理与复现

本轮所有子进程和请求已回收。初始不存在的build、UI生成目录和独立浏览器下载缓存已清理；系统固定Node/npm、Arrow与浏览器运行库保留，不清全局缓存。130个通过独立读回的Writer试次共650个Parquet工作文件已移除，删除前文件哈希、行数与物理组数见`parquet-cleanup.json`；全部原始CSV/manifest、冻结参数、失败轮、窗口、接纳账本及分析报告保留。必要失败证据、源码/固定依赖/许可证/fixtures/锁文件与V0～V4历史记录未删；详细路径和时点在`cleanup.json`。

从[实验README](../../benchmarks/README.md)的完整Release命令重新配置/构建并执行CTest，再列场景、全smoke、正式串行实验及分析。输出必须选择新的独占目录，不能覆盖本次原始记录。UI按[UI README](../../ui/README.md)执行npm ci、生产build、固定Chromium安装和配对验收；最终新构建测试显式设置`QBOOK_TEST_BUILD_DIR`。完整实际执行命令集中在本地`docs/releases/V5.md`。本轮没有commit、push或发布。
