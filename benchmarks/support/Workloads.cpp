#include "Workloads.h"

#include <fkYAML/node.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>

#ifndef QBOOK_SOURCE_DIR
#define QBOOK_SOURCE_DIR "."
#endif

namespace qbook::bench {
namespace {
using Node = fkyaml::node;
[[noreturn]] void Invalid(std::string_view field, std::string_view why) {
  throw std::invalid_argument(std::string(field) + ": " + std::string(why));
}
std::uint64_t Integer(const Node& value, std::string_view name) {
  if (!value.is_integer() || value.as_int() < 0) Invalid(name, "expected a nonnegative integer");
  return static_cast<std::uint64_t>(value.as_int());
}
std::string String(const Node& value, std::string_view name) {
  if (!value.is_string()) Invalid(name, "expected a string");
  return value.as_str();
}
std::map<std::string, std::uint64_t> Mix(const Node& value, std::string_view name) {
  if (!value.is_mapping()) Invalid(name, "expected a mapping");
  std::map<std::string, std::uint64_t> result;
  for (const auto& [key, count] : value.as_map()) {
    const auto text = String(key, name);
    if (!result.emplace(text, Integer(count, name)).second) Invalid(name, "duplicate mix key");
  }
  return result;
}
std::uint64_t Sum(const std::map<std::string, std::uint64_t>& values) {
  std::uint64_t total = 0;
  for (const auto& [key, count] : values) total = CheckedAdd(total, count);
  return total;
}
bool IsPipeline(const CaseSpec& s) { return s.operation.starts_with("pipeline/"); }
bool IsSteady(const CaseSpec& s) { return s.operation.starts_with("pipeline/network-steady/"); }

CaseSpec ParseCase(const Node& node) {
  if (!node.is_mapping()) Invalid("case", "expected a mapping");
  CaseSpec result;
  std::set<std::string> supplied;
  for (const auto& [key, value] : node.as_map()) {
    const auto name = String(key, "case key");
    if (!supplied.insert(name).second) Invalid(name, "duplicate key");
    if (name == "case_id") result.case_id = String(value, name);
    else if (name == "operation") result.operation = String(value, name);
    else if (name == "mode") result.mode = String(value, name);
    else if (name == "frame_mix") result.frame_mix = Mix(value, name);
    else if (name == "record_mix") result.record_mix = Mix(value, name);
    else if (name == "rate_tolerance") {
      if (value.is_float_number()) result.rate_tolerance = value.as_float();
      else if (value.is_integer()) result.rate_tolerance = static_cast<double>(value.as_int());
      else Invalid(name, "expected a finite number");
    }
#define QBOOK_READ_FIELD(field) else if (name == #field) result.field = Integer(value, name)
    QBOOK_READ_FIELD(instruments);
    QBOOK_READ_FIELD(initial_orders);
    QBOOK_READ_FIELD(levels);
    QBOOK_READ_FIELD(events_per_trial);
    QBOOK_READ_FIELD(working_set_frames);
    QBOOK_READ_FIELD(batch_rows);
    QBOOK_READ_FIELD(feed_queue_capacity);
    QBOOK_READ_FIELD(record_queue_capacity);
    QBOOK_READ_FIELD(snapshot_period_ms);
    QBOOK_READ_FIELD(metrics_period_ms);
    QBOOK_READ_FIELD(input_rate);
    QBOOK_READ_FIELD(warmup_sec);
    QBOOK_READ_FIELD(window_sec);
    QBOOK_READ_FIELD(guard_sec);
    QBOOK_READ_FIELD(sample_period_ms);
    QBOOK_READ_FIELD(stable_windows);
    QBOOK_READ_FIELD(lag_drift_tolerance);
    QBOOK_READ_FIELD(feed_book_lag_limit);
    QBOOK_READ_FIELD(book_writer_lag_limit);
    QBOOK_READ_FIELD(send_lateness_tolerance_ms);
    QBOOK_READ_FIELD(sample_lateness_tolerance_ms);
    QBOOK_READ_FIELD(trial_timeout_sec);
#undef QBOOK_READ_FIELD
    else Invalid(name, "unknown case parameter");
  }
  if (result.operation.empty()) result.operation = result.case_id;
  const std::set<std::string> common{"case_id", "operation", "events_per_trial"};
  std::set<std::string> allowed = common;
  auto add = [&](std::initializer_list<const char*> names) { for (auto name : names) allowed.insert(name); };
  if (result.operation.starts_with("core.")) add({"mode", "instruments", "initial_orders", "levels"});
  else if (result.operation.starts_with("protocol.")) add({"mode", "working_set_frames", "frame_mix"});
  else if (result.operation.starts_with("writer.")) add({"batch_rows", "record_mix"});
  else if (IsPipeline(result)) {
    add({"mode", "instruments", "initial_orders", "levels", "batch_rows", "feed_queue_capacity",
         "record_queue_capacity", "snapshot_period_ms", "metrics_period_ms", "trial_timeout_sec"});
    if (IsSteady(result)) add({"input_rate", "warmup_sec", "window_sec", "guard_sec", "sample_period_ms",
         "stable_windows", "rate_tolerance", "lag_drift_tolerance", "feed_book_lag_limit",
         "book_writer_lag_limit", "send_lateness_tolerance_ms", "sample_lateness_tolerance_ms"});
  } else Invalid("operation", "unknown benchmark family");
  for (const auto& name : supplied) if (!allowed.contains(name)) Invalid(name, "not applicable to this case");
  return result;
}
std::string MixJson(const std::map<std::string, std::uint64_t>& mix) {
  std::ostringstream out; out << '{'; bool first = true;
  for (const auto& [key, value] : mix) { if (!first) out << ','; first = false; out << JsonString(key) << ':' << value; }
  return out.str() + '}';
}
void ScaleMix(std::map<std::string, std::uint64_t>& mix, std::uint64_t target) {
  const auto total = Sum(mix);
  if (total == 0) return;
  std::uint64_t used = 0;
  auto last = mix.end();
  for (auto it = mix.begin(); it != mix.end(); ++it) if (it->second) last = it;
  for (auto it = mix.begin(); it != mix.end(); ++it) {
    if (it == last) it->second = target - used;
    else { it->second = CheckedMultiply(it->second, target) / total; used += it->second; }
  }
}
}  // namespace

std::uint64_t CheckedMultiply(std::uint64_t a, std::uint64_t b) {
  if (b && a > std::numeric_limits<std::uint64_t>::max() / b) throw std::overflow_error("benchmark multiplication overflow");
  return a * b;
}
std::uint64_t CheckedAdd(std::uint64_t a, std::uint64_t b) {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) throw std::overflow_error("benchmark addition overflow");
  return a + b;
}

ExperimentSpec LoadExperiment(const std::filesystem::path& file) {
  if (!std::filesystem::is_regular_file(file) || std::filesystem::file_size(file) > 1024 * 1024)
    Invalid("config", "requires an existing regular file no larger than 1 MiB");
  std::ifstream input(file, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open benchmark configuration");
  const std::string text((std::istreambuf_iterator<char>(input)), {});
  if (input.bad()) throw std::runtime_error("cannot read benchmark configuration");
  const auto docs = Node::deserialize_docs(text);
  if (docs.size() != 1 || !docs.front().is_mapping()) Invalid("config", "requires one mapping document");
  ExperimentSpec result;
  for (const auto& [key, value] : docs.front().as_map()) {
    const auto name = String(key, "root key");
    if (name == "experiment_id") result.experiment_id = String(value, name);
    else if (name == "measurement_kind") result.measurement_kind = String(value, name);
    else if (name == "cases") {
      if (!value.is_sequence() || value.as_seq().size() > 128) Invalid(name, "requires at most 128 cases");
      for (const auto& item : value.as_seq()) result.cases.push_back(ParseCase(item));
    }
#define QBOOK_READ_FIELD(field) else if (name == #field) result.field = Integer(value, name)
    QBOOK_READ_FIELD(schema_version);
    QBOOK_READ_FIELD(seed);
    QBOOK_READ_FIELD(warmup_rounds);
    QBOOK_READ_FIELD(repetitions);
    QBOOK_READ_FIELD(max_input_bytes);
    QBOOK_READ_FIELD(max_output_bytes);
#undef QBOOK_READ_FIELD
    else Invalid(name, "unknown experiment parameter");
  }
  ValidateExperiment(result);
  return result;
}

void ValidateCase(const CaseSpec& s, const ExperimentSpec& experiment) {
  if (s.case_id.empty() || s.case_id.size() > 120 || s.operation.empty()) Invalid("case_id", "missing or too long");
  if (s.case_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._/-") != std::string::npos)
    Invalid("case_id", "unsupported character");
  if (s.events_per_trial == 0 && s.operation != "writer.empty" && !IsSteady(s)) Invalid("events_per_trial", "must be positive");
  if (s.instruments == 0 || s.instruments > 10000) Invalid("instruments", "outside [1,10000]");
  const bool writer = s.operation.starts_with("writer.");
  if (writer) {
    if (!s.mode.empty()) Invalid("mode", "not applicable to Writer");
    if (s.batch_rows == 0 || s.batch_rows > 1048576) Invalid("batch_rows", "outside [1,1048576]");
    if (s.operation == "writer.empty" && s.events_per_trial != 0) Invalid("writer.empty", "requires zero rows");
    if (s.operation == "writer.mixed") {
      if (Sum(s.record_mix) != s.events_per_trial) Invalid("record_mix", "sum must match events_per_trial");
      for (const auto& [key, count] : s.record_mix)
        if (key != "order" && key != "trade" && key != "cancel" && key != "level" && key != "book") Invalid(key, "unknown record category");
    } else if (!s.record_mix.empty()) Invalid("record_mix", "only mixed Writer accepts a mix");
  } else if (s.mode != "Mbo" && s.mode != "L2") Invalid("mode", "must be Mbo or L2");
  if (s.operation.starts_with("protocol.")) {
    if (s.working_set_frames == 0) Invalid("working_set_frames", "must be positive");
    if (s.operation == "protocol.mixed") {
      if (Sum(s.frame_mix) != s.working_set_frames) Invalid("frame_mix", "sum must match working_set_frames");
      for (const auto& [key, count] : s.frame_mix)
        if (key != "valid_order" && key != "valid_trade" && key != "valid_cancel" && key != "valid_level" && key != "invalid_field" && key != "wrong_size") Invalid(key, "unknown frame category");
    } else if (!s.frame_mix.empty()) Invalid("frame_mix", "only mixed protocol accepts a mix");
  }
  auto inputs = CheckedAdd(s.initial_orders, s.events_per_trial);
  inputs = CheckedAdd(inputs, s.working_set_frames);
  inputs = CheckedAdd(inputs, CheckedMultiply(s.levels, s.instruments));
  if (IsPipeline(s)) {
    if ((s.operation.ends_with("/mbo") && s.mode != "Mbo") ||
        (s.operation.ends_with("/l2") && s.mode != "L2")) Invalid("mode", "does not match pipeline case suffix");
    if (s.levels == 0 || s.levels > 999) Invalid("levels", "pipeline requires [1,999]");
    const auto slots = CheckedMultiply(CheckedMultiply(s.instruments, s.levels), 2);
    if (s.mode == "Mbo" && (s.initial_orders < slots || s.initial_orders % slots)) Invalid("initial_orders", "must be a positive multiple of both-side instrument/level slots");
    if (s.mode == "L2" && s.initial_orders != 0) Invalid("initial_orders", "L2 has no individual orders");
    if (!IsSteady(s) && s.events_per_trial < 2) Invalid("events_per_trial", "pipeline requires at least two events");
    for (auto n : {s.lag_drift_tolerance, s.feed_book_lag_limit, s.book_writer_lag_limit})
      if (n > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()/4)) Invalid("lag tolerance", "signed difference range exceeded");
    for (auto n : {s.feed_queue_capacity, s.record_queue_capacity})
      if (!n || n > (1ULL << 24)) Invalid("queue_capacity", "outside [1,2^24]");
    if (!s.snapshot_period_ms || !s.metrics_period_ms || !s.batch_rows || !s.trial_timeout_sec)
      Invalid("pipeline", "periods, batch and timeout must be positive");
    if (s.trial_timeout_sec > 600) Invalid("trial_timeout_sec", "maximum 600 seconds");
    if (IsSteady(s)) {
      if (!s.input_rate || !s.warmup_sec || !s.window_sec || !s.guard_sec || !s.sample_period_ms || !s.stable_windows)
        Invalid("steady", "positive rate/stages/sample period/windows required");
      if (!std::isfinite(s.rate_tolerance) || s.rate_tolerance < 0 || s.rate_tolerance >= 1)
        Invalid("rate_tolerance", "requires 0 <= tolerance < 1");
      const auto duration = CheckedAdd(CheckedAdd(s.warmup_sec, s.window_sec), s.guard_sec);
      if (duration >= s.trial_timeout_sec) Invalid("trial_timeout_sec", "must exceed all continuous stages");
      for (auto seconds : {s.warmup_sec, s.window_sec, s.guard_sec})
        if (CheckedMultiply(seconds, 1000) % s.sample_period_ms) Invalid("sample_period_ms", "must divide each stage");
      if (CheckedMultiply(s.guard_sec,1000) < s.sample_period_ms) Invalid("guard_sec", "must cover a sample period");
      if (CheckedMultiply(s.guard_sec,s.input_rate) < CheckedAdd(s.feed_book_lag_limit,s.book_writer_lag_limit))
        Invalid("guard_sec", "must cover both frozen lag limits at the configured input rate");
      for (auto seconds : {s.warmup_sec, s.window_sec})
        if (CheckedMultiply(seconds, 1000) / s.sample_period_ms < CheckedAdd(s.stable_windows, 2)) Invalid("stable_windows", "insufficient complete windows");
      const auto measured = CheckedMultiply(s.input_rate, s.window_sec);
      if (s.events_per_trial && s.events_per_trial != measured) Invalid("events_per_trial", "steady count must equal input_rate*window_sec");
      if (measured < CheckedMultiply(s.batch_rows, 2)) Invalid("window_sec", "must cover two complete Writer batches");
      if (CheckedMultiply(s.window_sec, 1000) < CheckedMultiply(std::max(s.snapshot_period_ms, s.metrics_period_ms), 2)) Invalid("window_sec", "must span multiple snapshot and metric periods");
      inputs = CheckedAdd(inputs, CheckedMultiply(s.input_rate, duration));
    }
  }
  // 每条至少为一个记录及独立答案预留空间；进一步模块专用预算仍须独立校验。
  if (CheckedMultiply(inputs, 256) > experiment.max_input_bytes) Invalid("resources", "prepared input exceeds frozen memory budget");
  if (CheckedMultiply(CheckedAdd(inputs, 1024), 256) > experiment.max_output_bytes) Invalid("resources", "output estimate exceeds frozen disk budget");
  if (inputs > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 1024)) Invalid("events", "ID/time range overflow");
}

void ValidateExperiment(const ExperimentSpec& spec) {
  if (spec.schema_version != kSchemaVersion) Invalid("schema_version", "unsupported version");
  if (spec.experiment_id.empty()) Invalid("experiment_id", "required");
  if (spec.measurement_kind != "baseline" && spec.measurement_kind != "candidate" && spec.measurement_kind != "profile" && spec.measurement_kind != "smoke") Invalid("measurement_kind", "unsupported kind");
  if (!spec.repetitions || spec.repetitions > 100 || spec.warmup_rounds > 10) Invalid("rounds", "invalid finite repeat budget");
  if (!spec.max_input_bytes || !spec.max_output_bytes || spec.max_input_bytes > (1ULL<<34) || spec.max_output_bytes > (1ULL<<36)) Invalid("resources", "invalid resource budget");
  if (spec.cases.empty() || spec.cases.size() > 128) Invalid("cases", "requires 1..128 cases");
  std::set<std::string> ids;
  for (const auto& s : spec.cases) {
    if (!ids.insert(s.case_id).second) Invalid("case_id", "duplicate id");
    ValidateCase(s, spec);
  }
}

CaseSpec SmokeCase(const CaseSpec& original) {
  auto s = original;
  s.instruments = std::min<std::uint64_t>(s.instruments, 4);
  s.initial_orders = std::min<std::uint64_t>(s.initial_orders, 64);
  s.levels = std::min<std::uint64_t>(s.levels, 16);
  s.events_per_trial = std::min<std::uint64_t>(s.events_per_trial, 512);
  s.working_set_frames = std::min<std::uint64_t>(s.working_set_frames, 64);
  if (s.operation.starts_with("writer.")) {
    s.batch_rows = std::min<std::uint64_t>(s.batch_rows, 16);
    if (original.events_per_trial == original.batch_rows - 1) s.events_per_trial = s.batch_rows - 1;
    else if (original.events_per_trial == original.batch_rows) s.events_per_trial = s.batch_rows;
    else if (original.events_per_trial == original.batch_rows + 1) s.events_per_trial = s.batch_rows + 1;
    else if (original.events_per_trial == 2 * original.batch_rows + 1) s.events_per_trial = 2 * s.batch_rows + 1;
    if (!s.record_mix.empty()) ScaleMix(s.record_mix, s.events_per_trial);
  }
  if (!s.frame_mix.empty()) ScaleMix(s.frame_mix, s.working_set_frames);
  // 快照按品种实际调用，已有价位改单还需四张每品种锚定单。
  if (s.operation == "core.snapshot")
    s.events_per_trial = std::min(s.events_per_trial, std::uint64_t{512}/s.instruments);
  if (s.operation == "core.modify.existing_price")
    s.initial_orders = std::min(s.initial_orders, std::uint64_t{64}-4*s.instruments);
  if (s.operation == "core.cancel.full" || s.operation == "core.modify.new_price") s.events_per_trial = s.initial_orders;
  if (s.operation == "core.trade.full") s.events_per_trial = s.initial_orders / 2;
  if (s.operation == "core.l2.delete") s.events_per_trial = s.levels;
  if (IsPipeline(s)) {
    s.batch_rows = std::min<std::uint64_t>(s.batch_rows, 16);
    s.feed_queue_capacity = std::min<std::uint64_t>(s.feed_queue_capacity, 32);
    s.record_queue_capacity = std::min<std::uint64_t>(s.record_queue_capacity, 32);
    if (IsSteady(s)) {
      // 缩小有限输入规模，保留原实验的观测周期、窗口和全部稳定容差。
      s.input_rate = std::min<std::uint64_t>(s.input_rate,128);
      if (!s.input_rate || !s.sample_period_ms) Invalid("steady smoke", "positive input rate and sample period required");
      s.events_per_trial = CheckedMultiply(s.input_rate,s.window_sec);
      const auto lag = CheckedAdd(s.feed_book_lag_limit,s.book_writer_lag_limit);
      const auto minimum_guard = lag/s.input_rate+(lag%s.input_rate!=0);
      s.guard_sec = std::max(s.guard_sec,minimum_guard);
      // 整秒阶段边界仍须落在原采样网格上，不能截断最后一个窗口。
      const auto quantum = s.sample_period_ms/std::gcd(s.sample_period_ms,std::uint64_t{1000});
      s.guard_sec = CheckedMultiply(s.guard_sec/quantum+(s.guard_sec%quantum!=0),quantum);
    }
  }
  return s;
}

BenchmarkOptions ParseOptions(int argc, char** argv) {
  BenchmarkOptions options;
  options.config = std::filesystem::path(QBOOK_SOURCE_DIR) / "benchmarks/config/V5-baseline.yaml";
  std::set<std::string> seen;
  for (int i = 1; i < argc; ++i) {
    const std::string name = argv[i];
    if (!seen.insert(name).second) Invalid(name, "duplicate CLI option");
    if (name == "--list-cases") options.list_cases = true;
    else if (name == "--smoke") options.smoke = true;
    else if (name == "--config" || name == "--case" || name == "--output") {
      if (++i >= argc || std::string_view(argv[i]).starts_with("--")) Invalid(name, "requires a value");
      if (name == "--config") options.config = argv[i];
      else if (name == "--output") options.output = argv[i];
      else options.selected_case = argv[i];
    } else Invalid(name, "unknown CLI option");
  }
  if (options.selected_case.empty()) Invalid("--case", "must not be empty");
  if (!options.list_cases && options.output.empty()) Invalid("--output", "required for measurement runs");
  return options;
}

std::string JsonString(std::string_view value) {
  std::ostringstream out; out << '"';
  constexpr char hex[] = "0123456789abcdef";
  for (unsigned char c : value) {
    if (c == '"') out << "\\\"";
    else if (c == '\\') out << "\\\\";
    else if (c < 0x20) out << "\\u00" << hex[c >> 4] << hex[c & 15];
    else out << static_cast<char>(c);
  }
  out << '"'; return out.str();
}

std::string CanonicalCaseJson(const CaseSpec& s) {
  std::ostringstream out; out.imbue(std::locale::classic());
  out << "{\"case_id\":" << JsonString(s.case_id) << ",\"operation\":" << JsonString(s.operation);
#define QBOOK_JSON_NUM(field) out << ",\"" #field "\":" << s.field
  QBOOK_JSON_NUM(events_per_trial);
  if (!s.mode.empty()) out << ",\"mode\":" << JsonString(s.mode);
  if (s.operation.starts_with("core.") || IsPipeline(s)) {
    QBOOK_JSON_NUM(instruments); QBOOK_JSON_NUM(initial_orders); QBOOK_JSON_NUM(levels);
  }
  if (s.operation.starts_with("protocol.")) {
    QBOOK_JSON_NUM(working_set_frames); out << ",\"frame_mix\":" << MixJson(s.frame_mix);
  }
  if (s.operation.starts_with("writer.")) {
    QBOOK_JSON_NUM(batch_rows); out << ",\"record_mix\":" << MixJson(s.record_mix);
  }
  if (IsPipeline(s)) {
    QBOOK_JSON_NUM(batch_rows); QBOOK_JSON_NUM(feed_queue_capacity); QBOOK_JSON_NUM(record_queue_capacity);
    QBOOK_JSON_NUM(snapshot_period_ms); QBOOK_JSON_NUM(metrics_period_ms); QBOOK_JSON_NUM(trial_timeout_sec);
    out << ",\"actual_feed_queue_capacity\":" << std::bit_ceil(s.feed_queue_capacity)
        << ",\"actual_record_queue_capacity\":" << std::bit_ceil(s.record_queue_capacity);
    if (IsSteady(s)) {
      QBOOK_JSON_NUM(input_rate); QBOOK_JSON_NUM(warmup_sec); QBOOK_JSON_NUM(window_sec); QBOOK_JSON_NUM(guard_sec);
      QBOOK_JSON_NUM(sample_period_ms); QBOOK_JSON_NUM(stable_windows); QBOOK_JSON_NUM(lag_drift_tolerance);
      QBOOK_JSON_NUM(feed_book_lag_limit); QBOOK_JSON_NUM(book_writer_lag_limit);
      QBOOK_JSON_NUM(send_lateness_tolerance_ms); QBOOK_JSON_NUM(sample_lateness_tolerance_ms);
      out << ",\"rate_tolerance\":" << std::setprecision(17) << s.rate_tolerance;
    }
  }
#undef QBOOK_JSON_NUM
  return out.str() + '}';
}

void WorkloadHasher::Byte(std::uint8_t value) { state_ ^= value; state_ *= 1099511628211ULL; }
void WorkloadHasher::U64(std::uint64_t value) { for (unsigned i = 0; i != 8; ++i) { Byte(value & 255); value >>= 8; } }
void WorkloadHasher::String(std::string_view value) { U64(value.size()); for (unsigned char c : value) Byte(c); }
void WorkloadHasher::Bytes(std::span<const std::byte> bytes) { for (auto b : bytes) Byte(std::to_integer<std::uint8_t>(b)); }
void WorkloadHasher::Record(const qlib::record& record) {
  std::visit([&](const auto& value) {
    using T = std::remove_cvref_t<decltype(value)>;
    U64(record.index());
    if constexpr (!std::is_same_v<T, qlib::book>) I64(value.seq);
    I64(value.event_ns); I64(value.recv_ns); U64(value.instrument_id);
    if constexpr (std::is_same_v<T, qlib::book>) {
      for (auto n : value.bid_price) I64(n);
      for (auto n : value.bid_qty) I64(n);
      for (auto n : value.ask_price) I64(n);
      for (auto n : value.ask_qty) I64(n);
    } else {
      U64(static_cast<std::uint8_t>(value.side)); I64(value.qty);
      if constexpr (!std::is_same_v<T, qlib::cancel>) I64(value.price);
      if constexpr (std::is_same_v<T, qlib::order>) {
        I64(value.order_id); I64(value.new_qty); U64(static_cast<std::uint8_t>(value.type)); U64(static_cast<std::uint8_t>(value.action));
      } else if constexpr (std::is_same_v<T, qlib::trade>) { I64(value.buy_order_id); I64(value.sell_order_id); }
      else if constexpr (std::is_same_v<T, qlib::cancel>) I64(value.order_id);
    }
  }, record);
}
void WorkloadHasher::Event(const qlib::feed_event& value) { std::visit([&](const auto& v) { Record(qlib::record{v}); }, value); }
std::string WorkloadHasher::Finish() const { std::ostringstream out; out << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << state_; return out.str(); }
std::string HashWorkload(const CaseSpec& spec, std::uint64_t seed, std::span<const qlib::feed_event> initial, std::span<const qlib::feed_event> events) {
  WorkloadHasher hash; hash.String(kGeneratorVersion); hash.String(CanonicalCaseJson(spec)); hash.U64(seed);
  hash.U64(initial.size()); for (const auto& e : initial) hash.Event(e);
  hash.U64(events.size()); for (const auto& e : events) hash.Event(e);
  return hash.Finish();
}
std::string HashRecords(const CaseSpec& spec, std::uint64_t seed, std::span<const qlib::record> records) {
  WorkloadHasher hash; hash.String(kGeneratorVersion); hash.String(CanonicalCaseJson(spec)); hash.U64(seed);
  hash.U64(records.size()); for (const auto& record : records) hash.Record(record);
  return hash.Finish();
}
std::string HashFile(const std::filesystem::path& file) {
  std::ifstream stream(file, std::ios::binary); if (!stream) throw std::runtime_error("cannot hash file: " + file.string());
  WorkloadHasher hash; char buffer[16384];
  while (stream.read(buffer, sizeof(buffer)) || stream.gcount()) hash.Bytes(std::as_bytes(std::span{buffer, static_cast<std::size_t>(stream.gcount())}));
  if (stream.bad()) throw std::runtime_error("file hash read failed: " + file.string());
  return hash.Finish();
}
}  // namespace qbook::bench
