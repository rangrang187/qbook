#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace qbook::bench {

inline constexpr std::uint64_t kSchemaVersion = 1;
inline constexpr std::string_view kGeneratorVersion = "qbook-v5-1";

// case_id 区分规模；operation 选择有限的真实内核，不执行配置中的任意代码。
struct CaseSpec {
  std::string case_id;
  std::string operation;
  std::string mode;
  std::uint64_t instruments = 1;
  std::uint64_t initial_orders = 0;
  std::uint64_t levels = 0;
  std::uint64_t events_per_trial = 0;
  std::uint64_t working_set_frames = 0;
  std::uint64_t batch_rows = 1024;
  std::map<std::string, std::uint64_t> frame_mix;
  std::map<std::string, std::uint64_t> record_mix;
  std::uint64_t feed_queue_capacity = 1024;
  std::uint64_t record_queue_capacity = 1024;
  std::uint64_t snapshot_period_ms = 100;
  std::uint64_t metrics_period_ms = 100;
  std::uint64_t input_rate = 0;
  std::uint64_t warmup_sec = 0;
  std::uint64_t window_sec = 0;
  std::uint64_t guard_sec = 0;
  std::uint64_t sample_period_ms = 100;
  std::uint64_t stable_windows = 3;
  double rate_tolerance = 0.25;
  std::uint64_t lag_drift_tolerance = 64;
  std::uint64_t feed_book_lag_limit = 128;
  std::uint64_t book_writer_lag_limit = 128;
  std::uint64_t send_lateness_tolerance_ms = 50;
  std::uint64_t sample_lateness_tolerance_ms = 50;
  std::uint64_t trial_timeout_sec = 30;
};

struct ExperimentSpec {
  std::uint64_t schema_version = kSchemaVersion;
  std::string experiment_id;
  std::string measurement_kind = "baseline";
  std::uint64_t seed = 20261027;
  std::uint64_t warmup_rounds = 1;
  std::uint64_t repetitions = 5;
  std::uint64_t max_input_bytes = 268435456;
  std::uint64_t max_output_bytes = 1073741824;
  std::vector<CaseSpec> cases;
};

struct PhaseSpec {
  std::string phase;
  std::string count_kind;
  std::string timing_boundary;
};

struct PhaseMeasurement {
  std::string phase;
  std::string count_kind;
  std::uint64_t work_count = 0;
  std::uint64_t duration_ns = 0;
  std::uint64_t checksum = 0;
};

struct TrialContext {
  std::filesystem::path work_dir;
  bool warmup = false;
  std::uint64_t trial_index = 0;
  std::string measurement_kind;
};

struct TrialResult {
  std::vector<PhaseMeasurement> measurements;
  std::string validation_status = "valid";
  std::string validation_message;
  // 模块的固定 JSON 对象；pipeline 的 counts / pipeline_samples 放在这里。
  // 字符串值必须经 JsonString 转义，未知 accepted_count 使用 null。
  std::string details_json = "{}";
};

struct PreparedBenchmark {
  CaseSpec spec;
  std::string workload_hash;
  std::vector<PhaseSpec> phases;
  std::string instrumentation = "none";
  std::function<TrialResult(const TrialContext&)> run;
};

using PrepareFunction = std::function<PreparedBenchmark(const CaseSpec&, const ExperimentSpec&)>;

struct BenchmarkOptions {
  std::filesystem::path config;
  std::filesystem::path output;
  std::string selected_case = "all";
  bool list_cases = false;
  bool smoke = false;
};

ExperimentSpec LoadExperiment(const std::filesystem::path& file);
void ValidateExperiment(const ExperimentSpec& spec);
void ValidateCase(const CaseSpec& spec, const ExperimentSpec& experiment);
CaseSpec SmokeCase(const CaseSpec& spec);
BenchmarkOptions ParseOptions(int argc, char** argv);
std::string CanonicalCaseJson(const CaseSpec& spec);
std::string JsonString(std::string_view value);
std::uint64_t CheckedMultiply(std::uint64_t a, std::uint64_t b);
std::uint64_t CheckedAdd(std::uint64_t a, std::uint64_t b);

// factory 在计时之外准备只读输入；每次 run 必须重新创建自己的业务状态。
int BenchmarkMain(int argc, char** argv, std::string_view family,
                  const std::vector<std::string>& operations,
                  const PrepareFunction& factory);

}  // namespace qbook::bench
