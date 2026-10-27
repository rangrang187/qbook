# 指标跨语言固定样本

这些 `.bin` 是 `qbook_metrics_fixture_generator` 通过真实 `qlib::metrics` 值生成的
136 字节小端本机 ABI 样本。`src/detail/MetricsLayout.h` 在编译时检查全部字段类型、
偏移和总尺寸。它们不是 JavaScript 编码器的自测结果。

`.expected.json` 是独立手写的完整 17 字段十进制字符串期望；生成器只写二进制，
使用排他创建，不生成或覆盖期望 JSON。修改协议必须同步审查双方，不能重新生成答案来消除失败。

| 样本 | 验证内容 | 二进制 SHA-256 |
|---|---|---|
| layout | 17 个可区分字段、纳秒时间与大整数映射 | `ed11ff8501e38586f2ff29e25cbc6ccf28362e5b60ed5c3a3da0f27bdf9d1f3f` |
| bounds-min | int64 最小时间戳和全部零计数 | `839c28b95998215a163c7adacac4571b37c1e7096a7a8e3e502371d153ecace4` |
| bounds-max | int64 最大时间戳和全部 uint64 最大计数 | `4fdd88ff90ed7803e6d1644e150088b80bca0cce3661cd47430ea9e772499b52` |
| large-a | bookEvents=9007199254740993，建立精确基线 | `4c0ea167de639d6ff95ff625712234497c55761e52b67b798d6d1e281882d5c6` |
| large-b | bookEvents 增量10，抽样耗时增量200、样本增量2 | `eccade98413f7a985180b74813256a2721b7669bb628ea356062603b70337000` |
| zero-samples | 零抽样分母必须显示无数据 | `50cb6447a1135549d4f460cbb698cb4d8f166d99be0240f38493132a1a53e024` |

字段按偏移从0开始每8字节排列：`ts_ns`、`feed_messages`、`feed_bytes`、`feed_orders`、
`feed_trades`、`feed_levels`、`feed_dropped`、`book_events`、`book_apply_ns`、`book_samples`、
`book_instruments`、`book_resting_orders`、`book_memory_bytes`、`writer_orders`、`writer_trades`、
`writer_levels`、`writer_books`。仅时间戳为有符号 int64，其余为 uint64。

完整测试构建后，在新的临时目录重新生成并逐字节比较：

```bash
./build/debug/qbook_metrics_fixture_generator build/metrics-fixtures-check
for name in layout bounds-min bounds-max large-a large-b zero-samples; do
  cmp "build/metrics-fixtures-check/$name.bin" "tests/fixtures/metrics/$name.bin" || exit 1
done
```

UI 单元测试和浏览器测试读取这些文件。`qbook_monitoring_fixture_publisher metrics PERIOD_MS`
通过真实 XPUB 发送同一 C++ 固定值；stdin 只接受有限的 `use NAME`、`resume`、`pause`、
`malformed` 和 `quit` 命令，订阅建立由真实 XPUB 通知确认。它不属于生产服务，也不提供浏览器调试端点。
