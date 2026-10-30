#include "PipelineTrial.h"
#include "detail/WriterHooks.h"
#include "support/FailingOutputStream.h"
#include "support/StorageTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <stdexcept>
#include <fstream>

namespace {
using namespace qbook::bench;
qlib::record Raw(std::int64_t seq) {
  qlib::order value{}; value.seq = seq; value.instrument_id = 1;
  return value;
}
CaseSpec SteadySpec() {
  CaseSpec spec;
  spec.input_rate = 100; spec.sample_period_ms = 1000;
  spec.stable_windows = 2; spec.rate_tolerance = .25;
  spec.lag_drift_tolerance = 10; spec.feed_book_lag_limit = 30; spec.book_writer_lag_limit = 30;
  spec.sample_lateness_tolerance_ms = 50;
  return spec;
}
std::vector<PipelineSample> Samples() {
  std::vector<PipelineSample> result;
  for (std::uint64_t i = 0; i <= 8; ++i) {
    PipelineSample sample; sample.begin_ns = sample.end_ns = i * 1'000'000'000;
    sample.feed = sample.book = sample.writer = i * 100;
    result.push_back(sample);
  }
  return result;
}
const StageBounds kBounds{0, 4'000'000'000, 4'000'000'000, 8'000'000'000};
}

TEST_CASE("流水线完成观察器按原事件序号分阶段且快照不增加分母", "[benchmark][pipeline]") {
  RawCompletionObserver observer({2, 0, 2, 0}, {104}, {102});
  REQUIRE_FALSE(observer.PreparedSnapshotsReached());
  REQUIRE(observer.Reached(InputPhase::Warmup));
  observer.Observe(Raw(1)); observer.Observe(Raw(2));
  REQUIRE(observer.Reached(InputPhase::Prepare));
  qlib::book book{}; book.instrument_id = 1; book.event_ns = 102;
  observer.Observe(book);
  REQUIRE(observer.PreparedSnapshotsReached());
  observer.Observe(Raw(3)); observer.Observe(Raw(4));
  REQUIRE(observer.AllInputReached());
  REQUIRE_FALSE(observer.FinalSnapshotsReached());
  book.event_ns = 104; observer.Observe(book);
  REQUIRE(observer.FinalSnapshotsReached());
  REQUIRE(observer.Count() == 4);
  REQUIRE(observer.SnapshotCount() == 2);
  REQUIRE(observer.Phases()[0].count == 2);
  REQUIRE(observer.Phases()[2].count == 2);
  REQUIRE(observer.Phases()[2].last >= observer.Phases()[2].first);
}

TEST_CASE("流水线完成观察器拒绝重复乱序和多余原事件", "[benchmark][pipeline]") {
  SECTION("重复") {
    RawCompletionObserver observer({1, 0, 1, 0}, {2});
    observer.Observe(Raw(1)); REQUIRE_THROWS_AS(observer.Observe(Raw(1)), std::runtime_error);
  }
  SECTION("乱序") {
    RawCompletionObserver observer({1, 0, 1, 0}, {2});
    REQUIRE_THROWS_AS(observer.Observe(Raw(2)), std::runtime_error);
  }
  SECTION("多余") {
    RawCompletionObserver observer({1, 0, 1, 0}, {2});
    observer.Observe(Raw(1)); observer.Observe(Raw(2));
    REQUIRE_THROWS_AS(observer.Observe(Raw(3)), std::runtime_error);
  }
}

TEST_CASE("流水线快照完成要求所有已知品种达到独立最终时间", "[benchmark][pipeline]") {
  RawCompletionObserver observer({1, 0, 1, 0}, {101, 102});
  qlib::book snapshot{}; snapshot.instrument_id = 1; snapshot.event_ns = 101;
  observer.Observe(snapshot); observer.Observe(snapshot);
  REQUIRE_FALSE(observer.FinalSnapshotsReached());
  snapshot.instrument_id = 2; snapshot.event_ns = 100; observer.Observe(snapshot);
  REQUIRE_FALSE(observer.FinalSnapshotsReached());
  snapshot.event_ns = 102; observer.Observe(snapshot);
  REQUIRE(observer.FinalSnapshotsReached());
  snapshot.instrument_id = 3;
  REQUIRE_THROWS_AS(observer.Observe(snapshot), std::runtime_error);
}

TEST_CASE("稳态验证使用实际窗口时间并要求全部正式窗口", "[benchmark][pipeline]") {
  auto spec = SteadySpec(); auto samples = Samples();
  const auto valid = ValidateSteadySamples(samples, kBounds, spec, false);
  REQUIRE(valid.valid); REQUIRE(valid.warmup_windows == 4); REQUIRE(valid.measure_windows == 4);
  samples = Samples(); samples[6].feed += 30;
  const auto invalid = ValidateSteadySamples(samples, kBounds, spec, false);
  REQUIRE_FALSE(invalid.valid);
  REQUIRE(samples[6].status == "invalid");
}

TEST_CASE("稳态有符号积压估计保留容差内负值并拒绝越界", "[benchmark][pipeline]") {
  auto spec = SteadySpec(); auto samples = Samples();
  for (auto& sample : samples) { sample.book += 5; sample.writer += 10; }
  REQUIRE(ValidateSteadySamples(samples, kBounds, spec, false).valid);
  samples = Samples();
  for (auto& sample : samples) sample.writer += 11;
  REQUIRE_FALSE(ValidateSteadySamples(samples, kBounds, spec, false).valid);
}

TEST_CASE("逐窗漂移合格仍须检查正式区间总体积压增长", "[benchmark][pipeline]") {
  auto spec = SteadySpec(); auto samples = Samples();
  for (std::size_t i = 5; i < samples.size(); ++i) samples[i].feed += 4 * (i - 4);
  const auto result = ValidateSteadySamples(samples, kBounds, spec, false);
  REQUIRE_FALSE(result.valid);
  REQUIRE(result.message.find("whole measure feed-book lag growth") != std::string::npos);
}

TEST_CASE("稳态不能用不足窗口补齐通过或忽略采样迟到", "[benchmark][pipeline]") {
  const auto spec = SteadySpec();
  SECTION("不足") {
    auto samples = Samples(); samples.resize(6);
    REQUIRE_FALSE(ValidateSteadySamples(samples, kBounds, spec, false).valid);
  }
  SECTION("迟到") {
    auto samples = Samples(); samples[5].lateness_ns = 50'000'001;
    REQUIRE_FALSE(ValidateSteadySamples(samples, kBounds, spec, false).valid);
  }
  SECTION("跨度") {
    auto samples = Samples(); samples[5].begin_ns -= 50'000'001;
    REQUIRE_FALSE(ValidateSteadySamples(samples, kBounds, spec, false).valid);
  }
  SECTION("早期预热迟到仍无效") {
    auto samples = Samples(); samples[1].lateness_ns = 50'000'001;
    REQUIRE_FALSE(ValidateSteadySamples(samples, kBounds, spec, false).valid);
  }
}

TEST_CASE("稳态跨边界样本保留且不扩张完整窗口", "[benchmark][pipeline]") {
  auto spec = SteadySpec(); auto samples = Samples();
  const StageBounds shifted{1, 4'000'000'001, 4'000'000'001, 8'000'000'001};
  auto result = ValidateSteadySamples(samples, shifted, spec, false);
  REQUIRE(result.valid);
  REQUIRE(result.warmup_windows == 3); REQUIRE(result.measure_windows == 3);
  REQUIRE(samples[1].status == "boundary"); REQUIRE(samples[5].status == "boundary");
}

TEST_CASE("稳态预热只接受末尾稳定连续窗口且正式坏窗不能挑走", "[benchmark][pipeline]") {
  auto spec = SteadySpec(); auto samples = Samples();
  samples[1].feed += 30;
  REQUIRE(ValidateSteadySamples(samples, kBounds, spec, true).valid);
  samples = Samples(); samples[5].feed += 30;
  REQUIRE_FALSE(ValidateSteadySamples(samples, kBounds, spec, true).valid);
}

TEST_CASE("稳态任何阶段拒绝和计数倒退都会使试验无效", "[benchmark][pipeline]") {
  const auto spec = SteadySpec();
  SECTION("拒绝") {
    auto samples = Samples(); for (auto& sample : samples) if (sample.end_ns >= 5'000'000'000) sample.dropped = 1;
    REQUIRE_FALSE(ValidateSteadySamples(samples, kBounds, spec, false).valid);
  }
  SECTION("倒退") {
    auto samples = Samples(); samples[6].writer = 1;
    REQUIRE_FALSE(ValidateSteadySamples(samples, kBounds, spec, false).valid);
  }
}

TEST_CASE("受控链路完成所有Add后Close失败仍拒绝该轮成绩", "[benchmark][pipeline]") {
  qbook::testing::TempDirectory directory;
  auto probe = std::make_shared<qbook::testing::StreamProbe>();
  qbook::testing::StreamFaultPlan fault; fault.fail_close = true;
  qbook::detail::WriterHooks hooks;
  hooks.output_factory = qbook::testing::MakeOutputFactory(probe, fault);
  std::uint64_t raw = 0;
  hooks.after_add = [&](const qlib::record& record) {
    if (!std::holds_alternative<qlib::book>(record)) ++raw;
  };
  CaseSpec spec; spec.case_id = spec.operation = "pipeline/controlled-queue/mbo"; spec.mode = "Mbo";
  spec.instruments = 1; spec.levels = 1; spec.initial_orders = 2;
  spec.events_per_trial = 4; spec.batch_rows = 8;
  spec.snapshot_period_ms = 10; spec.metrics_period_ms = 10;
  const auto prepared = PrepareControlledPipelineBenchmark(spec, ExperimentSpec{}, hooks);
  REQUIRE_THROWS(prepared.run({directory.path(), false, 0, "smoke"}));
  REQUIRE(raw == 6);
  REQUIRE(probe->failures == 1);
  REQUIRE(std::filesystem::exists(directory.path() / "parquet"));
}

TEST_CASE("受控链路追加观察故障取消生产者与所有真实消费者", "[benchmark][pipeline]") {
  qbook::testing::TempDirectory directory;
  qbook::detail::WriterHooks hooks;
  std::uint64_t raw = 0;
  hooks.after_add = [&](const qlib::record& record) {
    if (!std::holds_alternative<qlib::book>(record) && ++raw == 3)
      throw std::runtime_error("controlled observer failure");
  };
  CaseSpec spec; spec.case_id = spec.operation = "pipeline/controlled-queue/mbo"; spec.mode = "Mbo";
  spec.instruments = 1; spec.levels = 1; spec.initial_orders = 2;
  spec.events_per_trial = 1024; spec.batch_rows = 8;
  spec.feed_queue_capacity = spec.record_queue_capacity = 2;
  spec.snapshot_period_ms = 10; spec.metrics_period_ms = 10;
  const auto prepared = PrepareControlledPipelineBenchmark(spec, ExperimentSpec{}, hooks);
  REQUIRE_THROWS_WITH(prepared.run({directory.path(), false, 0, "smoke"}), "controlled observer failure");
  REQUIRE(raw == 3);
}

TEST_CASE("网络worker配置严格重读原始参数而不接受manifest派生字段", "[benchmark][pipeline]") {
  qbook::testing::TempDirectory directory;
  CaseSpec spec; spec.case_id = spec.operation = "pipeline/network-finite/mbo"; spec.mode = "Mbo";
  spec.instruments = 2; spec.levels = 4; spec.initial_orders = 32; spec.events_per_trial = 128;
  spec.feed_queue_capacity = 17; spec.record_queue_capacity = 31;
  ExperimentSpec experiment; experiment.seed = 987;
  experiment.max_input_bytes = 1'048'576; experiment.max_output_bytes = 2'097'152;
  const auto path = directory.path() / "worker.yaml";
  { std::ofstream file(path); file << PipelineWorkerConfiguration(spec, experiment); }
  const auto loaded = LoadExperiment(path);
  REQUIRE(loaded.cases.size() == 1);
  REQUIRE(CanonicalCaseJson(loaded.cases.front()) == CanonicalCaseJson(spec));
  REQUIRE(loaded.seed == experiment.seed);
  REQUIRE(loaded.max_input_bytes == experiment.max_input_bytes);
  REQUIRE(loaded.max_output_bytes == experiment.max_output_bytes);
}

TEST_CASE("流水线成功退出前拒绝重复缺失倒序生命周期标记", "[benchmark][pipeline]") {
  PipelineLifecycleTrace trace;
  for (std::size_t phase = 0; phase < trace.points.size(); ++phase) trace.Record(phase);
  trace.returned = Clock::now();
  REQUIRE_NOTHROW(trace.Check());
  SECTION("重复即使五个原始时间点都保留也必须失败") {
    const auto original = trace.points;
    trace.Record(2);
    REQUIRE(trace.points == original);
    REQUIRE_THROWS_WITH(trace.Check(), "pipeline benchmark: duplicate or out-of-order lifecycle point");
  }
  SECTION("缺失") {
    trace.present[3] = false;
    REQUIRE_THROWS_WITH(trace.Check(), "pipeline benchmark: missing lifecycle point");
  }
  SECTION("倒序回调") {
    PipelineLifecycleTrace reversed;
    reversed.Record(0); reversed.Record(2); reversed.Record(1);
    reversed.Record(3); reversed.Record(4); reversed.returned = Clock::now();
    REQUIRE_THROWS_WITH(reversed.Check(), "pipeline benchmark: duplicate or out-of-order lifecycle point");
  }
  SECTION("返回早于最后join") {
    trace.returned = Clock::time_point{};
    REQUIRE_THROWS_WITH(trace.Check(), "pipeline benchmark: service returned before final join");
  }
}
