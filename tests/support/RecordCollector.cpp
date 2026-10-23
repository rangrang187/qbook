#include "RecordCollector.h"

#include <chrono>
#include <stdexcept>
#include <thread>

namespace qbook::testing {

RecordCollector::RecordCollector(RecordQueue& input, std::size_t max_records,
                                 CollectGoal goal, CollectorControl& commands,
                                 CollectorSignals& signals)
    : input_(input), max_records_(max_records), goal_(goal), commands_(commands),
      signals_(signals) {
  if (max_records == 0 || max_records > 1'000'000 || max_records > records_.max_size())
    throw std::invalid_argument("测试记录容量须为 1 至 1000000");
  records_.reserve(max_records);
}

void RecordCollector::Run(std::stop_token stop, const RunControl& control) {
  struct Finish {
    CollectorSignals& signals;
    ~Finish() { signals.done.store(true); }
  } finish{signals_};
  std::size_t raw_count = 0;
  std::size_t snapshot_count = 0;
  if (goal_.target_raw_count == 0 && !goal_.snapshot_after_raw_for)
    signals_.goal_reached.store(true);

  for (;;) {
    if (control.Failed()) return;
    if (commands_.fail_requested.load()) throw std::runtime_error("测试消费者主动失败");
    const bool draining = stop.stop_requested();
    if (commands_.pause.load() && !draining) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }
    auto record = input_.try_pop();
    if (!record) {
      if (draining) return;
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }
    if (records_.size() == max_records_) throw std::length_error("测试消费者记录容量超限");
    records_.push_back(std::move(*record));
    if (const auto* book = std::get_if<qlib::book>(&records_.back())) {
      signals_.snapshot_count.store(++snapshot_count);
      if (raw_count >= goal_.target_raw_count && goal_.snapshot_after_raw_for == book->instrument_id)
        signals_.goal_reached.store(true);
    } else {
      signals_.raw_count.store(++raw_count);
      if (raw_count >= goal_.target_raw_count && !goal_.snapshot_after_raw_for)
        signals_.goal_reached.store(true);
    }
  }
}

const std::vector<qlib::record>& RecordCollector::Results() const noexcept { return records_; }

}  // namespace qbook::testing
