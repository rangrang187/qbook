#pragma once

#include "Threads/BookThread.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace qbook::testing {

// 在唯一测试所有者下单步执行；Run 活跃期间不能从另一线程访问。
struct BookThreadInspector {
  static bool Consume(BookThread& stage, qlib::feed_event event,
                      Metrics& metrics, const RunControl& control) {
    return stage.Consume(std::move(event), metrics, control);
  }

  static bool Snapshot(BookThread& stage, Metrics& metrics,
                       const RunControl& control) {
    return stage.Snapshot(metrics, control);
  }

  static std::size_t Capacity(const BookThread& stage) { return stage.books_.capacity(); }
  static std::size_t Count(const BookThread& stage) { return stage.books_.size(); }
  static std::uint64_t Events(const BookThread& stage) { return stage.events_; }
  static const Orderbook& Book(const BookThread& stage, std::uint32_t id) {
    return stage.books_.at(id);
  }
  static std::uint64_t Diagnostics(const BookThread& stage, std::size_t index) {
    return stage.diagnostic_counts_.at(index);
  }
};

}  // namespace qbook::testing
