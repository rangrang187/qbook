#pragma once

#include "Orderbook.h"
#include "PipelineTypes.h"
#include "Runtime.h"
#include "Threads/MetricsThread.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <stop_token>

#include <qlib/map.h>

namespace qbook {
namespace testing { struct BookThreadInspector; }

// 独占品种订单簿，并连接输入消费端与输出生产端；每个实例只运行一次。
// 队列、指标、控制对象及本对象须存活到 Run 所在线程 join 之后。
class BookThread {
 public:
  // 绑定队列并检查模式和周期；构造期间不读取队列或启动线程。
  BookThread(FeedQueue& input, RecordQueue& output, BookMode mode,
             std::chrono::milliseconds snapshot_period);
  BookThread(const BookThread&) = delete;
  BookThread& operator=(const BookThread&) = delete;

  // 上游 join 后才可普通停止；返回前排空输入并交付原记录，下游应继续消费。
  // Fatal 中断等待且不保证完整输出；异常交给线程入口捕获并报告。
  void Run(Metrics& metrics, std::stop_token stop, const RunControl& control);

 private:
  friend struct testing::BookThreadInspector;
  using Books = qlib::map<std::uint32_t, Orderbook>;

  bool Consume(qlib::feed_event&& event, Metrics& metrics, const RunControl& control);
  ApplyResult Apply(const qlib::feed_event& event);
  bool Forward(qlib::record&& record, const RunControl& control);
  bool Snapshot(Metrics& metrics, const RunControl& control);

  FeedQueue& feed_queue_;
  RecordQueue& record_queue_;
  BookMode mode_;
  std::chrono::milliseconds snapshot_period_;
  Books books_;
  std::uint64_t events_ = 0;
  std::array<std::uint64_t, 2> diagnostic_counts_{};
};

}  // namespace qbook
