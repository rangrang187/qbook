#pragma once

#include "PipelineTypes.h"
#include "Runtime.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <vector>

namespace qbook::testing {

struct CollectorControl {
  std::atomic<bool> pause{false};
  std::atomic<bool> fail_requested{false};
};

struct CollectGoal {
  std::size_t target_raw_count = 0;
  std::optional<std::uint32_t> snapshot_after_raw_for;
};

struct CollectorSignals {
  std::atomic<std::size_t> raw_count{0};
  std::atomic<std::size_t> snapshot_count{0};
  std::atomic<bool> goal_reached{false};
  std::atomic<bool> done{false};
};

// 唯一消费 RecordQueue 并保存有限记录；普通 stop 在上游 join 后排空。
class RecordCollector {
public:
  RecordCollector(RecordQueue& input, std::size_t max_records,
                  CollectGoal goal, CollectorControl& commands,
                  CollectorSignals& signals);
  RecordCollector(const RecordCollector&) = delete;
  RecordCollector& operator=(const RecordCollector&) = delete;
  void Run(std::stop_token stop, const RunControl& control);

  // 调用前须 join Collector；返回值借用当前对象的结果存储。
  const std::vector<qlib::record>& Results() const noexcept;

private:
  RecordQueue& input_;
  std::size_t max_records_;
  CollectGoal goal_;
  CollectorControl& commands_;
  CollectorSignals& signals_;
  std::vector<qlib::record> records_;
};

}  // namespace qbook::testing
