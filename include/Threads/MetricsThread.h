#pragma once

#include <atomic>
#include <chrono>
#include <stop_token>
#include <string>
#include <cstdint>

#include <qlib/common.h>

static_assert(QLIB_CACHE_LINE == 64, "qbook 要求所有翻译单元使用 QLIB_CACHE_LINE=64");

namespace zmq { class socket_t; }

namespace qbook {
class RunControl;
namespace testing { class MetricsThreadInspector; }

// 单个非负指标由唯一写者更新，任意读者可并发观察。
// 所有翻译单元须使用一致的 QLIB_CACHE_LINE；销毁前须结束所有访问。
struct alignas(QLIB_CACHE_LINE) Metric {
  // 增加累计量；超出 uint64 范围时抛出 overflow_error 并保持旧值。
  void Add(std::uint64_t n = 1);

  // 替换状态量，允许下降；累计量在运行期间只用 Add 更新。
  void Set(std::uint64_t n) noexcept;

  // 返回单次 relaxed 读取；该值不发布其他业务数据。
  std::uint64_t Read() const noexcept;

  // 调用者通过上述操作访问；原子成员使单元不可复制或移动。
  std::atomic<std::uint64_t> value{0};
};

// 按阶段分配写入归属；各字段独立观察，整个集合须存活到所有读写者结束。
struct Metrics {
  Metric feed_messages;  // Feed 累计完整消息数，包含被拒绝输入。
  Metric feed_bytes;     // Feed 累计消息内容字节数，不含网络协议开销。
  Metric feed_orders;    // Feed 累计有效订单与撤单事件，入队前计数。
  Metric feed_trades;    // Feed 累计有效成交事件，入队前计数。
  Metric feed_levels;    // Feed 累计有效价位事件，入队前计数。
  Metric feed_dropped;   // Feed 累计格式、字段或模式拒绝数。

  Metric book_events;          // Book 累计完成应用的事件，包含业务忽略。
  Metric book_apply_ns;        // Book 累计抽样处理耗时，单位为纳秒。
  Metric book_samples;         // Book 累计耗时样本数。
  Metric book_instruments;     // Book 快照观察时的已建簿品种数。
  Metric book_resting_orders;  // Book 快照观察时的本地逐笔挂单数；L2 为 0。
  Metric book_memory_bytes;    // Book 快照观察时的订单簿存储估计，非进程 RSS。

  Metric writer_orders;  // Writer 累计成功追加的订单与撤单记录数。
  Metric writer_trades;  // Writer 累计成功追加的成交记录数。
  Metric writer_levels;  // Writer 累计成功追加的价位记录数。
  Metric writer_books;   // Writer 累计成功追加的快照数；追加不保证落盘。
};

// 读取 16 项指标并返回独立值，sample_ns 为本机 Unix epoch 纳秒采样时间。
// 调用期间 source 须存活；不同字段可以来自不同的更新时刻。
qlib::metrics SnapshotMetrics(const Metrics& source, std::int64_t sample_ns) noexcept;

// 只观察已有指标；Run 的调用线程独占创建、使用和释放 PUB/context。
// 不承诺保留中间样本或停止时发送最终帧；致命错误由阶段包装器传播。
class MetricsThread {
public:
  // 构造只保存并检查配置，不打开网络；周期须为可表示的正毫秒数。
  MetricsThread(std::string endpoint, std::chrono::milliseconds sample_period);
  // 同一实例仅由专属线程调用；借用对象须存活到返回，普通停止/失败均退出。
  void Run(const Metrics& metrics, std::stop_token stop, const RunControl& control);

private:
  friend class testing::MetricsThreadInspector;
  void Bind(zmq::socket_t& pub);
  bool Publish(zmq::socket_t& pub, const Metrics& metrics);
  std::string endpoint_;
  std::chrono::milliseconds sample_period_;
};

}  // namespace qbook
