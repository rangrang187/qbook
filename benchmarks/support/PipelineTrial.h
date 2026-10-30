#pragma once

#include "BenchmarkCase.h"
#include "Measure.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <qlib/common.h>

namespace qbook::detail { struct WriterHooks; }

namespace qbook::bench {

enum class InputPhase : std::size_t { Prepare, Warmup, Measure, Guard };
inline constexpr std::size_t kInputPhases = 4;

struct CompletionPhase {
  std::uint64_t count = 0;
  Clock::time_point first{}, last{};
};

// 只有 Writer 修改普通字段；协调者仅读完成位，全部 join 后才能读取阶段时间。
class RawCompletionObserver {
 public:
  RawCompletionObserver(std::array<std::uint64_t, kInputPhases> counts,
                        std::vector<std::int64_t> last_event_by_instrument,
                        std::vector<std::int64_t> prepared_event_by_instrument = {});
  void Observe(const qlib::record& record);
  bool Reached(InputPhase phase) const noexcept;
  bool AllInputReached() const noexcept;
  bool FinalSnapshotsReached() const noexcept;
  bool PreparedSnapshotsReached() const noexcept;
  const std::array<CompletionPhase, kInputPhases>& Phases() const noexcept { return phases_; }
  std::uint64_t Count() const noexcept { return next_seq_ - 1; }
  std::uint64_t SnapshotCount() const noexcept { return snapshots_; }
 private:
  std::array<std::uint64_t, kInputPhases> ends_{};
  std::array<CompletionPhase, kInputPhases> phases_{};
  std::array<std::atomic<bool>, kInputPhases> reached_{};
  std::atomic<bool> all_{false}, final_snapshots_{false}, prepared_snapshots_{false};
  std::vector<std::int64_t> last_event_;
  std::vector<bool> final_seen_;
  std::vector<std::int64_t> prepared_event_;
  std::vector<bool> prepared_seen_;
  std::size_t prepared_count_ = 0;
  std::size_t final_count_ = 0;
  std::uint64_t next_seq_ = 1, snapshots_ = 0;
};

// 所有偏移属于同一明示的时钟域；parent 的 feed/book/writer 三列都用 sent。
struct PipelineSample {
  std::uint64_t begin_ns = 0, end_ns = 0, lateness_ns = 0;
  std::uint64_t feed = 0, dropped = 0, book = 0, writer = 0;
  std::uint64_t first_send_ns = 0, last_send_ns = 0, max_send_lateness_ns = 0;
  std::string phase = "boundary", status = "boundary", reason;
};

struct StageBounds {
  std::uint64_t warmup_first = 0, warmup_last = 0;
  std::uint64_t measure_first = 0, measure_last = 0;
};

struct StabilityResult {
  bool valid = false;
  std::size_t warmup_windows = 0, measure_windows = 0;
  std::string message;
};

// 协调者独占的固定五阶段时间表；写入不分配，join 后校验完整性与顺序。
struct PipelineLifecycleTrace {
  Clock::time_point origin = Clock::now(), returned{};
  std::array<Clock::time_point, 5> points{};
  std::array<bool, 5> present{};
  bool malformed = false;
  void Record(std::size_t phase) noexcept;
  void Check() const;
};

// 纯函数只核对已有样本；不补样本、不截负积压、不挑选正式阶段的好窗口。
StabilityResult ValidateSteadySamples(std::vector<PipelineSample>& samples,
                                    const StageBounds& bounds, const CaseSpec& spec,
                                    bool parent_domain);

PreparedBenchmark PreparePipelineBenchmark(const CaseSpec&, const ExperimentSpec&);
// 故障验收复用同一受控链路与真实流工厂；不进入正式服务接口或 Feed 热路径。
PreparedBenchmark PrepareControlledPipelineBenchmark(const CaseSpec&, const ExperimentSpec&,
                                                     const detail::WriterHooks&);
std::string PipelineWorkerConfiguration(const CaseSpec&, const ExperimentSpec&);
int PipelineWorkerMain(int argc, char** argv);
std::vector<std::string> PipelineOperations();

}  // namespace qbook::bench
