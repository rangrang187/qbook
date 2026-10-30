# qbook V5 性能实验

四个程序调用实际生产库：`qbook_core_bench`、`qbook_protocol_bench`、`qbook_writer_bench`、`qbook_pipeline_bench`。实验配置与生产 `config/config.yaml` 分开。默认不构建实验；打开 `QBOOK_BUILD_BENCHMARKS` 不会自动打开其他能力。Writer 目标随 PERSISTENCE，Pipeline 目标随 SERVICE；核心和协议实验不需要 Arrow、ZMQ、Node 或 Catch2。

```bash
cmake -S . -B build/release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DQBOOK_BUILD_PIPELINE=ON -DQBOOK_BUILD_PERSISTENCE=ON \
  -DQBOOK_BUILD_SERVICE=ON -DQBOOK_BUILD_BENCHMARKS=ON \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DArrow_DIR=/usr/lib/x86_64-linux-gnu/cmake/Arrow \
  -DParquet_DIR=/usr/lib/x86_64-linux-gnu/cmake/Parquet
cmake --build build/release --parallel 4
ctest --test-dir build/release --output-on-failure -j 4
```

四个程序统一接受 `--config FILE`、`--case ID|all`、`--output DIR`、`--list-cases`、`--smoke`。输出目录必须尚不存在，父目录须已存在。程序拒绝覆盖旧结果。先列场景，再执行小规模正确性检查，之后串行正式测量：

```bash
build/release/qbook_core_bench --config benchmarks/config/V5-baseline.yaml --list-cases
mkdir -p results/benchmarks/my-run
build/release/qbook_core_bench --config benchmarks/config/V5-baseline.yaml \
  --case all --output results/benchmarks/my-run/core-smoke --smoke
build/release/qbook_core_bench --config benchmarks/config/V5-baseline.yaml \
  --case all --output results/benchmarks/my-run/core
```

其余三个程序同样调用。`--smoke` 缩小输入且独立标记；不能作为正式性能成绩。网络稳态仍有连续预热、测量和保护窗口，失败窗口不得删去。正式性能期间不要同时编译或启动其他性能任务。

以下从仓库根串行运行全部程序；`my-run`换成新的运行名，目录不得复用。每个命令都检查退出码，非零时保留该轮文件并调查，不能把后续报告写成全部通过：

```bash
mkdir -p results/benchmarks/my-run
for family in core protocol writer pipeline; do
  build/release/qbook_${family}_bench --config benchmarks/config/V5-baseline.yaml \
    --list-cases || exit $?
  build/release/qbook_${family}_bench --config benchmarks/config/V5-baseline.yaml \
    --output results/benchmarks/my-run/${family}-smoke --smoke || exit $?
done
# 全部smoke有效后再执行正式轮次，不与编译或浏览器测试并发。
for family in core protocol writer pipeline; do
  build/release/qbook_${family}_bench --config benchmarks/config/V5-baseline.yaml \
    --output results/benchmarks/my-run/${family} || exit $?
done
python3 scripts/analyse-benchmarks.py --input results/benchmarks/my-run \
  --output results/benchmarks/my-run-analysis.md
python3 -m unittest discover -s tests -p test_analyse_benchmarks.py
```

分析器退出0表示完整有效的正式集合；1表示无正式成绩或存在不完整/无效试次（仍输出报告）；2表示格式/身份等输入错误。它对smoke/profile单列排除，不允许拿它们补足baseline轮数。A/B使用`--baseline DIR --candidate DIR --output NEW.md`，会拒绝负载、构建政策、工具链或计时边界不同的比较。

每个运行目录保留 `manifest.json`、固定 16 列 `measurements.csv` 和各轮校验详情；网络实验还保存 worker 结果、有限采样和发送计划。`complete=true` 仅表示结果文件写完，须同时检查所有轮次的 `validation_status`。未知网络接纳数保持 `null`。Add 成功、正常 Close 和断电安全不同；文件只有经过显式 Close、join 和逐字段读回才作为有效结果。

计时为 `steady_clock` 批计时。`count_kind=none`、`work_count=0` 是纯耗时；不能计算吞吐。完成窗口按 N−1 个间隔计数。批均值、监控抽样均值均不是单事件 P99。

详细负载、计时边界、试跑冻结与环境见 [实验设计](../docs/benchmarks/V5-experiment.md)，实际结果和限制见 [结果报告](../docs/benchmarks/V5-results.md)。OI-04 未确定绝对性能门槛，任何实测数字都不代表绝对性能验收通过。
