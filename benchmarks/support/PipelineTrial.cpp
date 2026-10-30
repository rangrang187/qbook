#include "PipelineTrial.h"

#include "Workloads.h"
#include "Config.h"
#include "Runtime.h"
#include "Threads/BookThread.h"
#include "Threads/MetricsThread.h"
#include "Threads/WriterThread.h"
#include "detail/ServiceInternal.h"
#include "detail/WriterHooks.h"
#include "support/FeedEncoder.h"
#include "support/ParquetReadback.h"
#include "support/ProcessHarness.h"
#include "support/WriterThreadInspector.h"

#include <arrow/api.h>
#include <fkYAML/node.hpp>
#include <spdlog/spdlog.h>
#include <zmq.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <csignal>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unistd.h>

namespace qbook::bench {
namespace {
using namespace std::chrono_literals;
constexpr std::int64_t kEventBase = 1'000'000;
constexpr std::array<const char*, kInputPhases> kPhaseNames{"prepare", "warmup", "measure", "guard"};

void Require(bool value, std::string_view reason) {
  if (!value) throw std::runtime_error("pipeline benchmark: " + std::string(reason));
}
std::uint64_t Relative(Clock::time_point origin, Clock::time_point time) {
  return CheckedDurationNs(origin, time);
}
std::int64_t SignedDifference(std::uint64_t a, std::uint64_t b) {
  constexpr auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (a >= b) { Require(a - b <= limit, "signed difference overflow"); return static_cast<std::int64_t>(a - b); }
  Require(b - a <= limit, "signed difference overflow");
  return -static_cast<std::int64_t>(b - a);
}
std::uint64_t SignedDistance(std::int64_t a, std::int64_t b) {
  // SignedDifference 不产生 INT64_MIN；异号差先转无符号，避免相减溢出。
  if ((a < 0) == (b < 0)) return static_cast<std::uint64_t>(a >= b ? a - b : b - a);
  const auto magnitude = [](std::int64_t value) {
    return static_cast<std::uint64_t>(value < 0 ? -value : value);
  };
  return CheckedAdd(magnitude(a), magnitude(b));
}
std::int64_t WallNow() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string JoinErrors(const std::vector<std::string>& errors) {
  std::string result;
  for (const auto& error : errors) { if (!result.empty()) result += "; "; result += error; }
  return result;
}
}  // namespace

RawCompletionObserver::RawCompletionObserver(
    std::array<std::uint64_t, kInputPhases> counts,
    std::vector<std::int64_t> last_event_by_instrument,
    std::vector<std::int64_t> prepared_event_by_instrument)
    : last_event_(std::move(last_event_by_instrument)), final_seen_(last_event_.size(), false),
      prepared_event_(std::move(prepared_event_by_instrument)),
      prepared_seen_(prepared_event_.size(), false) {
  Require(prepared_event_.empty() || prepared_event_.size() == last_event_.size(), "prepared instrument count mismatch");
  prepared_snapshots_.store(prepared_event_.empty(), std::memory_order_relaxed);
  std::uint64_t end = 0;
  for (std::size_t phase = 0; phase < counts.size(); ++phase) {
    end = CheckedAdd(end, counts[phase]);
    ends_[phase] = end;
    reached_[phase].store(counts[phase] == 0, std::memory_order_relaxed);
  }
  Require(end < std::numeric_limits<std::uint64_t>::max(), "observer sequence overflow");
  Require(counts[static_cast<std::size_t>(InputPhase::Prepare)] > 0 &&
          counts[static_cast<std::size_t>(InputPhase::Measure)] > 0,
          "prepare and measure must be nonempty");
}

void RawCompletionObserver::Observe(const qlib::record& record) {
  if (const auto* snapshot = std::get_if<qlib::book>(&record)) {
    snapshots_ = CheckedAdd(snapshots_, 1);
    Require(snapshot->instrument_id > 0 && snapshot->instrument_id <= last_event_.size(),
            "unexpected snapshot instrument");
    const auto index = snapshot->instrument_id - 1;
    if (!prepared_event_.empty() && snapshot->event_ns == prepared_event_[index] && !prepared_seen_[index]) {
      prepared_seen_[index] = true;
      if (++prepared_count_ == prepared_seen_.size()) prepared_snapshots_.store(true, std::memory_order_release);
    }
    if (snapshot->event_ns == last_event_[index] && !final_seen_[index]) {
      final_seen_[index] = true;
      if (++final_count_ == final_seen_.size()) final_snapshots_.store(true, std::memory_order_release);
    }
    return;
  }
  const auto seq = std::visit([](const auto& value) -> std::int64_t {
    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, qlib::book>) return 0;
    else return value.seq;
  }, record);
  Require(seq > 0 && static_cast<std::uint64_t>(seq) == next_seq_ && next_seq_ <= ends_.back(),
          "extra, duplicate or out-of-order raw record");
  std::size_t phase = 0;
  while (next_seq_ > ends_[phase]) ++phase;
  auto& observed = phases_[phase];
  ++observed.count;
  if (observed.count == 1) observed.first = Clock::now();
  if (next_seq_ == ends_[phase]) {
    observed.last = observed.count == 1 ? observed.first : Clock::now();
    reached_[phase].store(true, std::memory_order_release);
  }
  if (++next_seq_ == ends_.back() + 1) all_.store(true, std::memory_order_release);
}

bool RawCompletionObserver::Reached(InputPhase phase) const noexcept {
  return reached_[static_cast<std::size_t>(phase)].load(std::memory_order_acquire);
}
bool RawCompletionObserver::AllInputReached() const noexcept { return all_.load(std::memory_order_acquire); }
bool RawCompletionObserver::FinalSnapshotsReached() const noexcept {
  return final_snapshots_.load(std::memory_order_acquire);
}
bool RawCompletionObserver::PreparedSnapshotsReached() const noexcept {
  return prepared_snapshots_.load(std::memory_order_acquire);
}

void PipelineLifecycleTrace::Record(std::size_t phase) noexcept {
  if (phase >= points.size() || present[phase]) { malformed = true; return; }
  if (phase > 0 && !present[phase - 1]) malformed = true;
  points[phase] = Clock::now(); present[phase] = true;
}

void PipelineLifecycleTrace::Check() const {
  Require(!malformed, "duplicate or out-of-order lifecycle point");
  for (std::size_t i = 0; i < points.size(); ++i) {
    Require(present[i], "missing lifecycle point");
    if (i > 0) Require(points[i] >= points[i - 1], "lifecycle clock reversed");
  }
  Require(returned >= points.back(), "service returned before final join");
}

StabilityResult ValidateSteadySamples(std::vector<PipelineSample>& samples,
                                    const StageBounds& bounds, const CaseSpec& spec,
                                    bool parent_domain) {
  StabilityResult result;
  std::vector<std::size_t> warmup, measure;
  std::vector<std::string> errors;
  const auto tolerance_ns = CheckedMultiply(spec.sample_lateness_tolerance_ms, 1'000'000);
  const auto drift = static_cast<std::int64_t>(spec.lag_drift_tolerance);
  auto classify = [&](const PipelineSample& previous, const PipelineSample& current) {
    if (previous.begin_ns >= bounds.warmup_first && current.end_ns <= bounds.warmup_last)
      return std::string("warmup");
    if (previous.begin_ns >= bounds.measure_first && current.end_ns <= bounds.measure_last)
      return std::string("measure");
    return std::string("boundary");
  };
  for (std::size_t i = 0; i < samples.size(); ++i) {
    auto& current = samples[i];
    current.phase = "boundary"; current.status = "boundary"; current.reason.clear();
    std::vector<std::string> reasons;
    if (current.end_ns < current.begin_ns) reasons.emplace_back("negative sample span");
    else if (current.end_ns - current.begin_ns > tolerance_ns) reasons.emplace_back("sample span exceeded");
    if (current.lateness_ns > tolerance_ns) reasons.emplace_back("sample late");
    if (parent_domain && current.max_send_lateness_ns > CheckedMultiply(spec.send_lateness_tolerance_ms, 1'000'000))
      reasons.emplace_back("send late");
    bool malformed = !reasons.empty();
    if (i != 0) {
      const auto& previous = samples[i - 1];
      current.phase = classify(previous, current);
      if (current.phase == "warmup") warmup.push_back(i);
      if (current.phase == "measure") measure.push_back(i);
      if (current.end_ns <= previous.end_ns) reasons.emplace_back("nonpositive sample interval");
      if (current.feed < previous.feed || current.book < previous.book || current.writer < previous.writer ||
          current.dropped < previous.dropped) reasons.emplace_back("counter decreased");
      if (current.dropped != previous.dropped) reasons.emplace_back("feed rejected input");
      malformed = !reasons.empty();
      if (current.phase != "boundary" && reasons.empty()) {
        const long double elapsed = static_cast<long double>(current.end_ns - previous.end_ns) / 1'000'000'000.0L;
        const auto rate_ok = [&](std::uint64_t before, std::uint64_t after) {
          const long double rate = static_cast<long double>(after - before) / elapsed;
          return std::abs(rate - spec.input_rate) <= static_cast<long double>(spec.rate_tolerance) * spec.input_rate;
        };
        if (!rate_ok(previous.feed, current.feed)) reasons.emplace_back(parent_domain ? "sent rate outside tolerance" : "feed rate outside tolerance");
        if (!parent_domain) {
          if (!rate_ok(previous.book, current.book)) reasons.emplace_back("book rate outside tolerance");
          if (!rate_ok(previous.writer, current.writer)) reasons.emplace_back("writer rate outside tolerance");
          const auto fb = SignedDifference(current.feed, current.book);
          const auto bw = SignedDifference(current.book, current.writer);
          if (fb < -drift || fb > static_cast<std::int64_t>(spec.feed_book_lag_limit)) reasons.emplace_back("feed-book lag exceeded");
          if (bw < -drift || bw > static_cast<std::int64_t>(spec.book_writer_lag_limit)) reasons.emplace_back("book-writer lag exceeded");
          if (SignedDistance(fb, SignedDifference(previous.feed, previous.book)) > spec.lag_drift_tolerance) reasons.emplace_back("feed-book drift exceeded");
          if (SignedDistance(bw, SignedDifference(previous.book, previous.writer)) > spec.lag_drift_tolerance) reasons.emplace_back("book-writer drift exceeded");
        }
      }
    }
    current.reason = JoinErrors(reasons);
    current.status = reasons.empty() ? (current.phase == "boundary" ? "boundary" : "valid") : "invalid";
    // 时间/单调性错误即使发生在边界也必须保留并拒绝；边界不进行速率拟合。
    if ((current.phase == "boundary" || malformed) && !reasons.empty())
      errors.push_back(current.phase + " sample " + std::to_string(i) + ": " + current.reason);
  }
  result.warmup_windows = warmup.size(); result.measure_windows = measure.size();
  if (warmup.size() < spec.stable_windows) errors.emplace_back("insufficient warmup windows");
  else for (auto i = warmup.size() - spec.stable_windows; i < warmup.size(); ++i)
    if (samples[warmup[i]].status != "valid") errors.push_back("warmup: " + samples[warmup[i]].reason);
  if (measure.size() < spec.stable_windows) errors.emplace_back("insufficient measure windows");
  for (auto i : measure) if (samples[i].status != "valid") errors.push_back("measure: " + samples[i].reason);
  if (!parent_domain && !measure.empty()) {
    const auto& first = samples[measure.front() - 1];
    const auto& last = samples[measure.back()];
    const auto fb_last = SignedDifference(last.feed, last.book), fb_first = SignedDifference(first.feed, first.book);
    const auto bw_last = SignedDifference(last.book, last.writer), bw_first = SignedDifference(first.book, first.writer);
    if (fb_last > fb_first && SignedDistance(fb_last, fb_first) > spec.lag_drift_tolerance)
      errors.emplace_back("whole measure feed-book lag growth exceeded");
    if (bw_last > bw_first && SignedDistance(bw_last, bw_first) > spec.lag_drift_tolerance)
      errors.emplace_back("whole measure book-writer lag growth exceeded");
  }
  result.valid = errors.empty(); result.message = JoinErrors(errors);
  return result;
}

namespace {
struct PipelineInput {
  CaseSpec spec;
  std::uint64_t seed = 0;
  std::uint64_t max_input_bytes = 0, max_output_bytes = 0;
  bool network = false, steady = false;
  std::vector<qlib::feed_event> events;
  std::vector<testing::EncodedFrame> frames;
  std::vector<qlib::book> snapshots_by_seq;
  std::vector<std::int64_t> last_event, prepared_event;
  std::array<std::uint64_t, kInputPhases> phase_counts{};
  std::string hash;
};

PipelineInput PrepareInput(const CaseSpec& spec, std::uint64_t seed) {
  PipelineInput input;
  input.spec = spec; input.seed = seed;
  input.network = spec.operation.find("/network-") != std::string::npos;
  input.steady = spec.operation.find("/network-steady/") != std::string::npos;
  const bool l2 = spec.mode == "L2";
  const auto levels_total = CheckedMultiply(CheckedMultiply(spec.instruments, spec.levels), 2);
  const auto slots = l2 ? levels_total : spec.initial_orders;
  Require(spec.instruments > 0 && spec.levels > 0 && spec.levels < 1000 && slots >= levels_total && slots % levels_total == 0,
          "working set requires complete instrument/side/level cycles");
  input.phase_counts = {slots, input.steady ? CheckedMultiply(spec.input_rate, spec.warmup_sec) : 0,
      input.steady ? CheckedMultiply(spec.input_rate, spec.window_sec) : spec.events_per_trial,
      input.steady ? CheckedMultiply(spec.input_rate, spec.guard_sec) : 0};
  std::uint64_t total = 0;
  for (const auto n : input.phase_counts) total = CheckedAdd(total, n);
  Require(total < static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - kEventBase), "event sequence overflow");
  input.events.reserve(total); input.frames.reserve(total); input.snapshots_by_seq.reserve(total);
  input.last_event.resize(spec.instruments); input.prepared_event.resize(spec.instruments);
  struct State { std::map<std::int64_t, std::int64_t> bid, ask; };
  std::vector<State> states(spec.instruments);
  std::vector<std::int64_t> quantities(slots, 0);
  for (std::uint64_t index = 0; index < total; ++index) {
    const bool prepare = index < slots;
    const auto slot = prepare ? index : (index - slots) % slots;
    const auto instrument = static_cast<std::uint32_t>(slot % spec.instruments + 1);
    const auto local = slot / spec.instruments;
    const auto side = local % 2 ? qlib::side::sell : qlib::side::buy;
    const auto level = (local / 2) % spec.levels;
    const auto price = static_cast<std::int64_t>(side == qlib::side::buy ? 1000 - level : 2000 + level);
    const auto qty = prepare ? 10 : (quantities[slot] == 10 ? 11 : 10);
    const auto seq = static_cast<std::int64_t>(index + 1);
    const auto event_ns = kEventBase + seq;
    const auto recv_ns = input.network ? -777 : event_ns + 10'000;
    auto& state = states[instrument - 1];
    auto& prices = side == qlib::side::buy ? state.bid : state.ask;
    if (l2) {
      qlib::level value{};
      value.seq = seq; value.price = price; value.qty = qty; value.event_ns = event_ns;
      value.recv_ns = recv_ns; value.instrument_id = instrument; value.side = side;
      input.events.emplace_back(value);
      prices[price] = qty;
    } else {
      qlib::order value{};
      value.seq = seq; value.order_id = static_cast<std::int64_t>(local + 1);
      value.price = price; value.qty = prepare ? qty : 777;
      value.new_qty = prepare ? 0 : qty;
      value.event_ns = event_ns; value.recv_ns = recv_ns; value.instrument_id = instrument;
      value.side = side; value.type = qlib::order_type::limit;
      value.action = prepare ? qlib::order_action::add : qlib::order_action::modify;
      input.events.emplace_back(value);
      prices[price] += qty - quantities[slot];
    }
    quantities[slot] = qty;
    qlib::book snapshot{};
    snapshot.instrument_id = instrument; snapshot.event_ns = event_ns; snapshot.recv_ns = recv_ns;
    if (!state.bid.empty()) { snapshot.bid_price[0] = state.bid.rbegin()->first; snapshot.bid_qty[0] = state.bid.rbegin()->second; }
    if (!state.ask.empty()) { snapshot.ask_price[0] = state.ask.begin()->first; snapshot.ask_qty[0] = state.ask.begin()->second; }
    input.snapshots_by_seq.push_back(snapshot);
    input.last_event[instrument - 1] = event_ns;
    if (prepare) input.prepared_event[instrument - 1] = event_ns;
    input.frames.push_back(testing::EncodeFeedEvent(input.events.back()));
  }
  input.hash = HashWorkload(spec, seed, std::span(input.events).first(slots), std::span(input.events).subspan(slots));
  return input;
}

std::int64_t ReadInteger(const arrow::Table& table, std::string_view column, std::int64_t row) {
  auto values = table.GetColumnByName(std::string(column));
  Require(values != nullptr, "missing column " + std::string(column));
  for (const auto& chunk : values->chunks()) {
    if (row >= chunk->length()) { row -= chunk->length(); continue; }
    switch (chunk->type_id()) {
      case arrow::Type::INT64: return static_cast<const arrow::Int64Array&>(*chunk).Value(row);
      case arrow::Type::UINT32: return static_cast<const arrow::UInt32Array&>(*chunk).Value(row);
      default: throw std::runtime_error("pipeline unexpected scalar type");
    }
  }
  throw std::runtime_error("pipeline row outside chunks");
}

struct FileSummary { std::uint64_t orders = 0, levels = 0, books = 0, bytes = 0; };

FileSummary ValidateFiles(const PipelineInput& input, const std::filesystem::path& directory,
                          std::int64_t receive_min, std::int64_t receive_max,
                          const std::vector<std::uint64_t>* accepted = nullptr,
                          bool require_final_snapshot = true) {
  Require(HashWorkload(input.spec, input.seed, std::span(input.events).first(input.phase_counts[0]),
                      std::span(input.events).subspan(input.phase_counts[0])) == input.hash,
          "prepared input changed during pipeline run");
  const auto max_rows = std::max<std::uint64_t>(input.events.size() + 1,
      CheckedMultiply(input.spec.trial_timeout_sec * 1000 / input.spec.snapshot_period_ms + 10, input.spec.instruments));
  auto actual = testing::ReadRunDirectory(directory, static_cast<std::int64_t>(max_rows));
  testing::ExpectedRun expected;
  std::vector<std::int64_t> recv(input.events.size(), 0);
  if (accepted) {
    Require(accepted->size() == input.events.size(), "accepted ledger missing input");
    for (std::size_t i = 0; i < accepted->size(); ++i) Require((*accepted)[i] == i, "accepted ledger duplicate/out of order");
  }
  const auto& raw_table = input.spec.mode == "L2" ? *actual.levels : *actual.orders;
  Require(raw_table.num_rows() == static_cast<std::int64_t>(input.events.size()), "raw row count mismatch");
  for (std::size_t i = 0; i < input.events.size(); ++i) {
    recv[i] = ReadInteger(raw_table, "recv_ns", static_cast<std::int64_t>(i));
    std::visit([&](const auto& source) {
      using T = std::decay_t<decltype(source)>;
      auto value = source;
      if (input.network) {
        Require(recv[i] >= receive_min && recv[i] <= receive_max && recv[i] != source.recv_ns,
                "network recv_ns outside local receive interval");
        value.recv_ns = recv[i];  // 只接纳不可预定的本机接收时间，其他字段全由冻结输入给定。
      }
      if constexpr (std::is_same_v<T, qlib::order>) expected.orders.push_back(value);
      else if constexpr (std::is_same_v<T, qlib::level>) expected.levels.push_back(value);
      else throw std::logic_error("unexpected workload record type");
    }, input.events[i]);
  }
  std::vector<std::int64_t> latest(input.spec.instruments, 0);
  for (std::int64_t row = 0; row < actual.books->num_rows(); ++row) {
    const auto event_ns = ReadInteger(*actual.books, "event_ns", row);
    Require(event_ns > kEventBase, "snapshot event time outside frozen sequence");
    const auto seq = event_ns - kEventBase;
    const auto instrument = ReadInteger(*actual.books, "instrument_id", row);
    Require(seq > 0 && static_cast<std::uint64_t>(seq) <= input.events.size(), "snapshot refers to unknown input");
    auto value = input.snapshots_by_seq[static_cast<std::size_t>(seq - 1)];
    Require(instrument == value.instrument_id && instrument > 0 && instrument <= static_cast<std::int64_t>(latest.size()), "snapshot instrument mismatch");
    Require(seq >= latest[instrument - 1], "snapshot event sequence went backwards");
    latest[instrument - 1] = seq;
    value.recv_ns = recv[static_cast<std::size_t>(seq - 1)];
    expected.books.push_back(value);
  }
  for (std::size_t i = 0; i < latest.size(); ++i) {
    if (require_final_snapshot)
      Require(kEventBase + latest[i] == input.last_event[i], "final instrument state not snapshotted");
    else
      Require(kEventBase + latest[i] >= input.prepared_event[i], "prepared instrument state not snapshotted");
  }
  testing::CheckExpectedFiles(actual, expected);
  FileSummary summary{expected.orders.size(), expected.levels.size(), expected.books.size(), 0};
  for (const auto& entry : std::filesystem::directory_iterator(directory))
    if (entry.is_regular_file()) summary.bytes = CheckedAdd(summary.bytes, entry.file_size());
  Require(summary.bytes <= input.max_output_bytes, "actual Parquet bytes exceed frozen output budget");
  return summary;
}

std::filesystem::path FindRun(const std::filesystem::path& root) {
  std::optional<std::filesystem::path> found;
  for (const auto& entry : std::filesystem::directory_iterator(root)) if (entry.is_directory()) {
    Require(!found.has_value(), "multiple output runs in one trial"); found = entry.path();
  }
  Require(found.has_value(), "missing output run"); return *found;
}

void WriteText(const std::filesystem::path& file, const std::string& text) {
  Require(!std::filesystem::exists(file), "refusing to overwrite " + file.string());
  std::ofstream stream(file, std::ios::binary);
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  stream << text;
  stream.close();
}

Config TrialConfig(const CaseSpec& spec, const std::filesystem::path& output, std::string endpoint) {
  Config config;
  config.feed_endpoint = std::move(endpoint); config.l2 = spec.mode == "L2";
  config.feed_queue_capacity = spec.feed_queue_capacity;
  config.record_queue_capacity = spec.record_queue_capacity;
  config.snapshot_period = std::chrono::milliseconds(spec.snapshot_period_ms);
  config.sample_period = std::chrono::milliseconds(spec.metrics_period_ms);
  config.batch_rows = static_cast<std::int64_t>(spec.batch_rows);
  config.metrics_endpoint = "tcp://127.0.0.1:*";
  config.out_dir = std::filesystem::absolute(output);
  ValidateConfig(config);
  return config;
}

std::string ConfigJson(const Config& config) {
  std::ostringstream json;
  json << "{\"feed_endpoint\":" << JsonString(config.feed_endpoint)
       << ",\"l2\":" << (config.l2 ? "true" : "false")
       << ",\"feed_queue_capacity\":" << config.feed_queue_capacity
       << ",\"record_queue_capacity\":" << config.record_queue_capacity
       << ",\"snapshot_period\":" << config.snapshot_period.count()
       << ",\"sample_period\":" << config.sample_period.count()
       << ",\"batch_rows\":" << config.batch_rows
       << ",\"metrics_endpoint\":" << JsonString(config.metrics_endpoint)
       << ",\"out_dir\":" << JsonString(config.out_dir.string()) << "}\n";
  return json.str();
}

using LifecycleTrace = PipelineLifecycleTrace;

void AddShutdown(TrialResult& result, const LifecycleTrace& trace) {
  trace.Check();
  result.measurements.push_back({"drain-book", "none", 0,
      CheckedDurationNs(trace.points[1], trace.points[2]), 0});
  result.measurements.push_back({"drain-writer-close", "none", 0,
      CheckedDurationNs(trace.points[2], trace.points[3]), 0});
  result.measurements.push_back({"shutdown-total", "none", 0,
      CheckedDurationNs(trace.points[0], trace.returned), 0});
}

std::string SampleJson(const PipelineSample& sample, std::string_view domain) {
  std::ostringstream json;
  json << "{\"clock_domain\":" << JsonString(domain)
       << ",\"begin_ns\":" << sample.begin_ns << ",\"end_ns\":" << sample.end_ns
       << ",\"lateness_ns\":" << sample.lateness_ns << ",\"feed_messages\":" << sample.feed
       << ",\"feed_dropped\":" << sample.dropped << ",\"book_events\":" << sample.book
       << ",\"writer_raw_records\":" << sample.writer
       << ",\"first_send_ns\":" << sample.first_send_ns << ",\"last_send_ns\":" << sample.last_send_ns
       << ",\"max_send_lateness_ns\":" << sample.max_send_lateness_ns
       << ",\"phase\":" << JsonString(sample.phase) << ",\"status\":" << JsonString(sample.status)
       << ",\"reason\":" << JsonString(sample.reason) << '}';
  return json.str();
}

std::string LifecycleJson(const LifecycleTrace& trace) {
  std::ostringstream json;
  json << "{\"clock_domain\":\"worker\",\"points_ns\":[";
  for (std::size_t i = 0; i < trace.points.size(); ++i) {
    if (i) json << ',';
    if (trace.present[i]) json << Relative(trace.origin, trace.points[i]); else json << "null";
  }
  json << "],\"returned_ns\":" << Relative(trace.origin, trace.returned) << '}';
  return json.str();
}

std::string FileSummaryJson(const FileSummary& summary) {
  std::ostringstream json;
  json << "{\"orders\":" << summary.orders << ",\"trades\":0,\"cancels\":0,\"levels\":"
       << summary.levels << ",\"books\":" << summary.books << '}';
  return json.str();
}

// 三个真实消费者由明确的协调器管理，析构兜底只处理异常的全体取消。
struct ControlledStages {
  RunControl control;
  std::array<std::jthread, 4> threads;
  std::array<std::atomic<bool>, 4> done{};
  bool joined = false;
  ~ControlledStages() noexcept {
    if (joined) return;
    if (!control.Failed()) {
      try { throw std::runtime_error("controlled benchmark cleanup"); }
      catch (...) { control.Fail("benchmark", std::current_exception()); }
    }
    for (auto& thread : threads) if (thread.joinable()) thread.request_stop();
    for (auto& thread : threads) if (thread.joinable()) thread.join();
  }
  template<class Function> void Start(std::size_t index, const char* name, Function function) {
    threads[index] = std::jthread([&, index, name, function](std::stop_token stop) {
      try { function(stop); } catch (...) { control.Fail(name, std::current_exception()); }
      done[index].store(true, std::memory_order_release);
    });
  }
  void Join(std::size_t index, Clock::time_point deadline) {
    threads[index].request_stop();
    while (!done[index].load(std::memory_order_acquire)) {
      if (Clock::now() >= deadline && !control.Failed()) {
        try { throw std::runtime_error("controlled stage join deadline"); }
        catch (...) { control.Fail("benchmark", std::current_exception()); }
      }
      if (control.Failed()) for (auto& thread : threads) thread.request_stop();
      std::this_thread::sleep_for(100us);
    }
    threads[index].join();
  }
};

TrialResult RunControlled(const PipelineInput& input, const TrialContext& trial,
                          const detail::WriterHooks* extra_hooks = nullptr) {
  const auto& spec = input.spec;
  const auto directory = std::filesystem::absolute(trial.work_dir / "parquet");
  auto config = TrialConfig(spec, directory, "tcp://127.0.0.1:1");
  WorkloadHasher endpoint_hash; endpoint_hash.String(directory.string());
  config.metrics_endpoint = "ipc://@qbook-bench-" + std::to_string(::getpid()) + '-' + endpoint_hash.Finish();
  LifecycleTrace trace;
  const auto deadline = Clock::now() + std::chrono::seconds(spec.trial_timeout_sec);
  Metrics metrics;
  FeedQueue feed(spec.feed_queue_capacity); RecordQueue records(spec.record_queue_capacity);
  RawCompletionObserver observer(input.phase_counts, input.last_event, input.prepared_event);
  detail::WriterHooks hooks;
  if (extra_hooks) hooks.output_factory = extra_hooks->output_factory;
  hooks.after_add = [&](const qlib::record& record) {
    observer.Observe(record);
    if (extra_hooks && extra_hooks->after_add) extra_hooks->after_add(record);
  };
  auto writer = testing::WriterThreadInspector::Create(records, directory, config.batch_rows, hooks);
  BookThread book(feed, records, config.l2 ? BookMode::L2 : BookMode::Mbo, config.snapshot_period);
  MetricsThread monitor(config.metrics_endpoint, config.sample_period);
  std::vector<std::uint64_t> accepted; accepted.reserve(input.events.size());
  Clock::time_point producer_begin{}, producer_end{};
  ControlledStages stages;
  // 元素顺序与正常停止角色对应：producer, book, writer, metrics。
  zmq::context_t probe_context(1);
  zmq::socket_t probe(probe_context, zmq::socket_type::sub);
  probe.set(zmq::sockopt::linger, 0); probe.set(zmq::sockopt::subscribe, "");
  probe.set(zmq::sockopt::rcvtimeo, 1); probe.connect(config.metrics_endpoint);
  stages.Start(3, "metrics", [&](std::stop_token stop) { monitor.Run(metrics, stop, stages.control); });
  // 首帧证明真实 Metrics PUB 已启动；测量前结束这个独占就绪探针。
  for (;;) {
    Require(!stages.control.Failed(), "Metrics failed before readiness");
    Require(Clock::now() < deadline, "Metrics first frame deadline");
    zmq::message_t frame;
    if (probe.recv(frame, zmq::recv_flags::none)) {
      Require(frame.size() == sizeof(qlib::metrics), "Metrics readiness frame layout mismatch");
      break;
    }
  }
  probe.close(); probe_context.close();
  stages.Start(2, "writer", [&](std::stop_token stop) { writer->Run(metrics, stop, stages.control); });
  stages.Start(1, "book", [&](std::stop_token stop) { book.Run(metrics, stop, stages.control); });
  stages.Start(0, "producer", [&](std::stop_token stop) {
    for (std::size_t i = 0; i < input.events.size(); ++i) {
      if (i == input.phase_counts[0]) {
        while (!observer.Reached(InputPhase::Prepare) || !observer.PreparedSnapshotsReached()) {
          if (stages.control.Failed() || stop.stop_requested()) return;
          Require(Clock::now() < deadline, "prepare completion timeout");
          std::this_thread::sleep_for(100us);
        }
        producer_begin = Clock::now();
      }
      qlib::feed_event event = input.events[i];
      for (;;) {
        if (stages.control.Failed() || stop.stop_requested()) return;
        Require(Clock::now() < deadline, "producer admission timeout");
        if (feed.try_push(std::move(event))) { accepted.push_back(i); break; }
        std::this_thread::sleep_for(50us);
      }
    }
    producer_end = Clock::now();
  });
  // 唯一生产者成功 push 完全部计划即可开始排空；不提前等 Writer 追平。
  while (!stages.done[0].load(std::memory_order_acquire)) {
    if (stages.control.Failed()) break;
    Require(Clock::now() < deadline, "controlled completion timeout");
    std::this_thread::sleep_for(100us);
  }
  trace.Record(0);
  for (std::size_t i = 0; i < stages.threads.size(); ++i) { stages.Join(i, deadline); trace.Record(i + 1); }
  stages.joined = true; trace.returned = Clock::now();
  if (stages.control.Failed()) {
    if (stages.control.Error()) std::rethrow_exception(stages.control.Error());
    throw std::runtime_error("controlled pipeline failed");
  }
  Require(testing::WriterThreadInspector::Released(*writer), "Writer resources not released in Run");
  Require(observer.AllInputReached() && observer.PreparedSnapshotsReached(), "controlled targets incomplete");
  Require(metrics.book_events.Read() == input.events.size(), "Book count does not match accepted ledger");
  const auto verify_begin = Clock::now();
  const auto summary = ValidateFiles(input, testing::WriterThreadInspector::Directory(*writer), 0, 0, &accepted, false);
  const auto verify_ns = CheckedDurationNs(verify_begin, Clock::now());
  std::ostringstream ledger;
  ledger << "input_index,seq\n";
  for (const auto index : accepted) ledger << index << ',' << index + 1 << '\n';
  WriteText(trial.work_dir / "accepted.csv", ledger.str());
  const auto& measured = observer.Phases()[2];
  Require(measured.count >= 2, "completion interval requires at least two records");
  TrialResult result;
  result.measurements = {
    {"input-admission", "accepted_events", measured.count, CheckedDurationNs(producer_begin, producer_end), measured.count},
    {"completed-window", "completed_raw_intervals", measured.count - 1, CheckedDurationNs(measured.first, measured.last), measured.count},
    {"completed-cycle", "validated_raw_records", measured.count, CheckedDurationNs(producer_begin, measured.last), measured.count}
  };
  AddShutdown(result, trace);
  std::ostringstream details;
  details << "{\"accepted_count\":" << accepted.size() << ",\"offered_count\":" << input.events.size()
          << ",\"sent_count\":null,\"processed_count\":" << metrics.book_events.Read()
          << ",\"appended_count\":" << observer.Count() << ",\"measured_count\":" << measured.count
          << ",\"feed_queue_capacity_actual\":" << feed.capacity()
          << ",\"record_queue_capacity_actual\":" << records.capacity()
          << ",\"ledger_kind\":\"successful-push-indices\",\"ledger_size\":" << accepted.size()
          << ",\"ledger_file\":\"accepted.csv\""
          << ",\"ledger_first_index\":" << accepted.front() << ",\"ledger_last_index\":" << accepted.back()
          << ",\"file_rows\":" << FileSummaryJson(summary) << ",\"file_bytes\":" << summary.bytes
          << ",\"verification_ns\":" << verify_ns << ",\"close_success\":true,\"fields_verified\":true,\"input_unchanged\":true"
          << ",\"final_snapshot_required\":false,\"metrics_readiness\":\"real-first-frame\""
          << ",\"feed_joined_role\":\"controlled-producer\",\"lifecycle\":" << LifecycleJson(trace)
          << ",\"parquet_removed_after_readback\":true}";
  result.details_json = details.str();
  // 只删除该 trial 的独占工作副本；原始测量和核对摘要由公共 Runner 保留。
  std::filesystem::remove_all(directory);
  return result;
}

std::string WorkerSpecJson(const PipelineInput& input) {
  ExperimentSpec experiment;
  experiment.seed = input.seed;
  experiment.max_input_bytes = input.max_input_bytes; experiment.max_output_bytes = input.max_output_bytes;
  return PipelineWorkerConfiguration(input.spec, experiment);
}

struct WorkerOutcome {
  int exit_code = -1;
  std::uint64_t feed = 0, dropped = 0, processed = 0, appended = 0, snapshots = 0;
  std::array<std::uint64_t, kInputPhases> counts{}, first{}, last{};
  LifecycleTrace lifecycle;
  std::vector<PipelineSample> samples;
};

std::string WorkerOutcomeJson(const WorkerOutcome& outcome) {
  std::ostringstream json;
  json << "{\"exit_code\":" << outcome.exit_code << ",\"feed_messages\":" << outcome.feed
       << ",\"feed_dropped\":" << outcome.dropped << ",\"processed_count\":" << outcome.processed
       << ",\"appended_count\":" << outcome.appended << ",\"snapshot_count\":" << outcome.snapshots
       << ",\"phases\":[";
  for (std::size_t phase = 0; phase < kInputPhases; ++phase) {
    if (phase) json << ',';
    json << "{\"name\":" << JsonString(kPhaseNames[phase]) << ",\"count\":" << outcome.counts[phase]
         << ",\"first_ns\":" << outcome.first[phase] << ",\"last_ns\":" << outcome.last[phase] << '}';
  }
  json << "],\"lifecycle\":" << LifecycleJson(outcome.lifecycle) << ",\"samples\":[";
  for (std::size_t i = 0; i < outcome.samples.size(); ++i) {
    if (i) json << ',';
    json << SampleJson(outcome.samples[i], "worker");
  }
  json << "]}\n";
  return json.str();
}

WorkerOutcome ReadWorkerOutcome(const std::filesystem::path& file) {
  Require(std::filesystem::file_size(file) <= 32 * 1024 * 1024, "worker report exceeds size budget");
  std::ifstream stream(file);
  Require(stream.good(), "worker report missing");
  const auto node = fkyaml::node::deserialize(stream);
  const auto number = [](const fkyaml::node& object, const char* key) {
    const auto& value = object.at(key);
    Require(value.is_integer(), std::string("worker noninteger field ") + key);
    const auto result = value.get_value<std::int64_t>();
    Require(result >= 0, std::string("worker negative field ") + key);
    return static_cast<std::uint64_t>(result);
  };
  WorkerOutcome result;
  result.exit_code = static_cast<int>(number(node, "exit_code"));
  result.feed = number(node, "feed_messages"); result.dropped = number(node, "feed_dropped");
  result.processed = number(node, "processed_count"); result.appended = number(node, "appended_count");
  result.snapshots = number(node, "snapshot_count");
  const auto& phases = node.at("phases");
  Require(phases.is_sequence() && phases.size() == kInputPhases, "worker phases incomplete");
  for (std::size_t phase = 0; phase < kInputPhases; ++phase) {
    const auto& p = phases.at(phase);
    Require(p.at("name").get_value<std::string>() == kPhaseNames[phase], "worker phase ordering mismatch");
    result.counts[phase] = number(p, "count"); result.first[phase] = number(p, "first_ns");
    result.last[phase] = number(p, "last_ns");
  }
  const auto& lifecycle = node.at("lifecycle");
  const auto& points = lifecycle.at("points_ns");
  Require(points.is_sequence() && points.size() == 5, "worker lifecycle incomplete");
  result.lifecycle.origin = Clock::time_point{};
  for (std::size_t i = 0; i < 5; ++i) {
    Require(points.at(i).is_integer(), "missing worker lifecycle marker");
    const auto ns = points.at(i).get_value<std::int64_t>();
    Require(ns >= 0, "worker lifecycle time is negative");
    result.lifecycle.points[i] = Clock::time_point(std::chrono::nanoseconds(ns));
    result.lifecycle.present[i] = true;
  }
  result.lifecycle.returned = Clock::time_point(std::chrono::nanoseconds(number(lifecycle, "returned_ns")));
  result.lifecycle.Check();
  const auto& samples = node.at("samples");
  Require(samples.is_sequence() && samples.size() <= 100000, "worker sample count exceeds budget");
  result.samples.reserve(samples.size());
  for (const auto& source : samples.as_seq()) {
    PipelineSample sample;
    sample.begin_ns = number(source, "begin_ns"); sample.end_ns = number(source, "end_ns");
    sample.lateness_ns = number(source, "lateness_ns"); sample.feed = number(source, "feed_messages");
    sample.dropped = number(source, "feed_dropped"); sample.book = number(source, "book_events");
    sample.writer = number(source, "writer_raw_records");
    result.samples.push_back(std::move(sample));
  }
  return result;
}

void CheckProcessAlive(testing::ProcessHarness& process) {
  process.Poll();
  const auto& status = process.Result();
  Require(status.exit_code == -1 && status.term_signal == 0 && status.exec_error == 0 &&
          !status.output_overflow && !status.timed_out,
          "network worker stopped before completion: " + status.output);
}

void WaitSubscribed(zmq::socket_t& publisher, testing::ProcessHarness& process,
                    Clock::time_point deadline) {
  while (Clock::now() < deadline) {
    CheckProcessAlive(process);
    zmq::message_t notice;
    if (publisher.recv(notice, zmq::recv_flags::dontwait)) {
      if (notice.size() == 1 && *static_cast<const unsigned char*>(notice.data()) == 1) return;
    }
    std::this_thread::sleep_for(1ms);
  }
  throw std::runtime_error("XPUB subscription deadline");
}

void SendFrame(zmq::socket_t& publisher, const testing::EncodedFrame& frame) {
  const auto result = publisher.send(zmq::buffer(frame.storage.data(), frame.size), zmq::send_flags::none);
  Require(result && *result == frame.size, "XPUB did not send complete frame");
}

struct ParentOutcome {
  Clock::time_point origin = Clock::now(), begin{}, end{}, tail_begin{}, tail_end{};
  std::uint64_t sent = 0, max_send_lateness_ns = 0;
  std::uint64_t begin_deviation_ns = 0, end_deviation_ns = 0;
  bool measured_before_guard_end = false;
  StageBounds bounds;
  std::vector<PipelineSample> samples;
  std::vector<std::string> errors;
};

void SendSteady(const PipelineInput& input, zmq::socket_t& publisher,
                testing::ProcessHarness& process, ParentOutcome& result,
                Clock::time_point deadline) {
  const auto& spec = input.spec;
  const auto period = std::chrono::milliseconds(spec.sample_period_ms);
  const auto planned = Clock::now() + 20ms;
  const auto warmup_end = planned + std::chrono::seconds(spec.warmup_sec);
  const auto measure_end = warmup_end + std::chrono::seconds(spec.window_sec);
  const auto guard_end = measure_end + std::chrono::seconds(spec.guard_sec);
  result.origin = planned;
  result.bounds = {0, Relative(planned, warmup_end), Relative(planned, warmup_end), Relative(planned, measure_end)};
  auto next_sample = planned;
  result.samples.reserve((spec.warmup_sec + spec.window_sec + spec.guard_sec) * 1000 / spec.sample_period_ms + 4);
  std::uint64_t paced_sent = 0, window_first = 0, window_last = 0, window_late = 0;
  bool window_has_send = false;
  auto sample = [&] {
    const auto now = Clock::now();
    if (now < next_sample) return;
    Require(result.samples.size() < result.samples.capacity(), "parent sample budget exhausted");
    PipelineSample observation;
    observation.begin_ns = observation.end_ns = Relative(planned, now);
    observation.lateness_ns = Relative(next_sample, now);
    observation.feed = observation.book = observation.writer = paced_sent;
    observation.first_send_ns = window_first; observation.last_send_ns = window_last;
    observation.max_send_lateness_ns = window_late;
    result.samples.push_back(std::move(observation));
    window_has_send = false; window_first = window_last = window_late = 0;
    // 不补历史样本；跳过的采样期限体现为 lateness 和实际长区间。
    next_sample += period * (1 + (now - next_sample) / period);
  };
  auto wait_until = [&](Clock::time_point due) {
    while (Clock::now() < due) {
      Require(Clock::now() < deadline, "steady parent deadline");
      CheckProcessAlive(process);
      sample();
      if (process.Result().output.find("bench_measured_ready") != std::string::npos)
        result.measured_before_guard_end = true;
      const auto wake = std::min({due, next_sample, Clock::now() + 2ms});
      if (wake > Clock::now()) std::this_thread::sleep_until(wake);
    }
  };
  const auto prepare_count = input.phase_counts[0];
  const auto measure_index = input.phase_counts[1];
  const auto guard_index = measure_index + input.phase_counts[2];
  const auto count = input.events.size() - prepare_count;
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto offset = CheckedMultiply(i, 1'000'000'000) / spec.input_rate;
    const auto due = planned + std::chrono::nanoseconds(offset);
    wait_until(due);
    sample();
    CheckProcessAlive(process);
    const auto before = Clock::now();
    const auto late = Relative(due, before);
    result.max_send_lateness_ns = std::max(result.max_send_lateness_ns, late);
    if (i == measure_index) {
      result.begin = before;
      result.begin_deviation_ns = Relative(warmup_end, before);
    }
    if (i == guard_index) {
      result.end = before;
      result.end_deviation_ns = Relative(measure_end, before);
    }
    const auto phase_end = i < measure_index ? warmup_end : (i < guard_index ? measure_end : guard_end);
    if (before >= phase_end && result.errors.empty()) result.errors.emplace_back("scheduled send crossed frozen stage boundary");
    SendFrame(publisher, input.frames[prepare_count + i]);
    ++result.sent; ++paced_sent;
    const auto completed = Clock::now();
    const auto completion_late = Relative(due, completed);
    result.max_send_lateness_ns = std::max(result.max_send_lateness_ns, completion_late);
    if (completed >= phase_end && result.errors.empty()) result.errors.emplace_back("send completion crossed frozen stage boundary");
    const auto sent_at = Relative(planned, completed);
    if (!window_has_send) { window_first = sent_at; window_has_send = true; }
    window_last = sent_at; window_late = std::max(window_late, completion_late);
  }
  wait_until(guard_end);
  sample();
  // 到达保护末尾才观察到通知不满足“保护窗内已完成”的条件。
  result.tail_begin = Clock::now();
  Require(result.begin != Clock::time_point{} && result.end != Clock::time_point{}, "missing parent measurement boundary");
  if (result.max_send_lateness_ns > CheckedMultiply(spec.send_lateness_tolerance_ms, 1'000'000))
    result.errors.emplace_back("send lateness exceeded frozen tolerance");
  if (!result.measured_before_guard_end) result.errors.emplace_back("measured completion not notified inside guard window");
}

TrialResult RunNetwork(const PipelineInput& input, const TrialContext& trial) {
  const auto& spec = input.spec;
  const auto work = std::filesystem::absolute(trial.work_dir);
  const auto report = work / "worker.json";
  const auto worker_spec = work / "worker-spec.yaml";
  const auto output = work / "parquet";
  WriteText(worker_spec, WorkerSpecJson(input));
  zmq::context_t context(1);
  zmq::socket_t publisher(context, zmq::socket_type::xpub);
  publisher.set(zmq::sockopt::linger, 0);
  publisher.set(zmq::sockopt::sndtimeo, 100);
  publisher.set(zmq::sockopt::sndhwm, static_cast<int>(std::min<std::size_t>(input.events.size() + 1, 1'000'000)));
  publisher.set(zmq::sockopt::xpub_nodrop, 1);
  publisher.bind("tcp://127.0.0.1:*");
  const auto executable = std::filesystem::canonical("/proc/self/exe");
  testing::ProcessHarness process(executable, {"--worker", "network", worker_spec.string(), report.string(), input.hash});
  const auto config = TrialConfig(spec, output, publisher.get(zmq::sockopt::last_endpoint));
  process.WriteConfig(ConfigJson(config));
  const auto receive_min = WallNow();
  const auto deadline = Clock::now() + std::chrono::seconds(spec.trial_timeout_sec);
  ParentOutcome parent;
  try {
    process.Start();
    process.WaitForOutput("service_control_ready", deadline);
    process.WaitForOutput("writer_opened", deadline);
    process.WaitForOutput("metrics_bound", deadline);
    WaitSubscribed(publisher, process, deadline);
    for (std::size_t i = 0; i < input.phase_counts[0]; ++i) { SendFrame(publisher, input.frames[i]); ++parent.sent; }
    process.WaitForOutput("bench_prepared_ready", deadline);
    if (input.steady) {
      SendSteady(input, publisher, process, parent, deadline);
      process.WaitForOutput("bench_all_input_ready", deadline);
      parent.tail_end = Clock::now();
    } else {
      parent.begin = Clock::now();
      for (std::size_t i = input.phase_counts[0]; i < input.frames.size(); ++i) {
        CheckProcessAlive(process); SendFrame(publisher, input.frames[i]); ++parent.sent;
      }
      process.WaitForOutput("bench_measured_ready", deadline);
      parent.end = Clock::now();
    }
    // 最终状态须由真实周期快照观察；其等待不进入原事件完成窗口。
    process.WaitForOutput("bench_final_snapshots_ready", deadline);
    process.SendSignal(SIGTERM);
    const auto& status = process.Wait(deadline);
    WriteText(work / "worker.log", status.output);
    Require(status.exit_code == 0 && status.term_signal == 0 && status.exec_error == 0 &&
            !status.output_overflow && !status.timed_out, "network worker did not close normally: " + status.output);
    const auto receive_max = WallNow();
    auto worker = ReadWorkerOutcome(report);
    Require(worker.exit_code == 0 && worker.appended == input.events.size() &&
            worker.processed == input.events.size() && worker.feed == input.events.size() && worker.dropped == 0,
            "worker final counts differ from full frozen input");
    Require(parent.sent == input.events.size(), "parent sent count differs from input");
    const auto verify_begin = Clock::now();
    const auto summary = ValidateFiles(input, FindRun(output), receive_min, receive_max);
    const auto verify_ns = CheckedDurationNs(verify_begin, Clock::now());
    Require(summary.books == worker.snapshots, "after_add snapshot count differs from file rows");
    TrialResult result;
    const auto count = worker.counts[2];
    Require(count == input.phase_counts[2] && count >= 2 && worker.last[2] > worker.first[2], "invalid measured completion interval");
    StabilityResult worker_stable, parent_stable;
    if (input.steady) {
      const StageBounds worker_bounds{worker.first[1], worker.last[1], worker.first[2], worker.last[2]};
      worker_stable = ValidateSteadySamples(worker.samples, worker_bounds, spec, false);
      parent_stable = ValidateSteadySamples(parent.samples, parent.bounds, spec, true);
      if (!worker_stable.valid) parent.errors.push_back("worker: " + worker_stable.message);
      if (!parent_stable.valid) parent.errors.push_back("parent: " + parent_stable.message);
      result.measurements = {
        {"steady-input-window", "sent_events", count, CheckedDurationNs(parent.begin, parent.end), count},
        {"steady-completed-window", "completed_raw_intervals", count - 1, worker.last[2] - worker.first[2], count},
        {"steady-tail-completion", "none", 0, CheckedDurationNs(parent.tail_begin, parent.tail_end), 0}
      };
    } else {
      result.measurements = {
        {"completed-window", "completed_raw_intervals", count - 1, worker.last[2] - worker.first[2], count},
        {"finite-completion", "validated_raw_records", count, CheckedDurationNs(parent.begin, parent.end), count}
      };
    }
    AddShutdown(result, worker.lifecycle);
    if (!parent.errors.empty()) { result.validation_status = "invalid"; result.validation_message = JoinErrors(parent.errors); }
    std::ostringstream details;
    details << "{\"accepted_count\":null,\"accepted_count_reason\":\"Feed has no admission observer\","
            << "\"offered_count\":" << input.events.size() << ",\"sent_count\":" << parent.sent
            << ",\"processed_count\":" << worker.processed << ",\"appended_count\":" << worker.appended
            << ",\"decoded_count\":" << worker.feed - worker.dropped << ",\"measured_count\":" << count
            << ",\"feed_queue_capacity_actual\":" << std::bit_ceil(spec.feed_queue_capacity)
            << ",\"record_queue_capacity_actual\":" << std::bit_ceil(spec.record_queue_capacity)
            << ",\"file_rows\":" << FileSummaryJson(summary) << ",\"file_bytes\":" << summary.bytes
            << ",\"verification_ns\":" << verify_ns << ",\"close_success\":true,\"fields_verified\":true,\"input_unchanged\":true"
            << ",\"max_send_lateness_ns\":" << parent.max_send_lateness_ns
            << ",\"measure_begin_deviation_ns\":" << parent.begin_deviation_ns
            << ",\"measure_end_deviation_ns\":" << parent.end_deviation_ns
            << ",\"measured_before_guard_end\":" << (parent.measured_before_guard_end ? "true" : "false")
            << ",\"worker_result\":" << JsonString("worker.json")
            << ",\"lifecycle\":" << LifecycleJson(worker.lifecycle) << ",\"pipeline_samples\":[";
    bool comma = false;
    for (const auto& sample : parent.samples) { if (comma) details << ','; comma = true; details << SampleJson(sample, "parent"); }
    for (const auto& sample : worker.samples) { if (comma) details << ','; comma = true; details << SampleJson(sample, "worker"); }
    details << "],\"stability\":{";
    const auto stability = [&](std::string_view name, const StabilityResult& state) {
      details << JsonString(name) << ":{\"valid\":" << (state.valid ? "true" : "false")
              << ",\"warmup_windows\":" << state.warmup_windows
              << ",\"measure_windows\":" << state.measure_windows
              << ",\"message\":" << JsonString(state.message) << '}';
    };
    stability("parent", parent_stable); details << ','; stability("worker", worker_stable);
    details << "},\"parquet_removed_after_readback\":true}";
    result.details_json = details.str();
    std::filesystem::remove_all(output);
    process.RemoveDirectory();
    return result;
  } catch (...) {
    // ProcessHarness 在自身析构中只取消并回收该 PID；失败工作副本不删除。
    if (!std::filesystem::exists(work / "worker.log")) WriteText(work / "worker.log", process.Result().output);
    throw;
  }
}

}  // namespace

int PipelineWorkerMain(int argc, char** argv) {
  try {
    Require(argc == 6 && std::string_view(argv[1]) == "--worker" &&
            std::string_view(argv[2]) == "network", "invalid private worker invocation");
    const auto experiment = LoadExperiment(argv[3]);
    Require(experiment.cases.size() == 1, "worker requires one frozen case");
    auto input = PrepareInput(experiment.cases.front(), experiment.seed);
    input.max_input_bytes = experiment.max_input_bytes; input.max_output_bytes = experiment.max_output_bytes;
    Require(input.network && input.hash == argv[5], "worker workload identity mismatch");
    const auto config = LoadConfig("config/config.yaml");
    RawCompletionObserver observer(input.phase_counts, input.last_event);
    WorkerOutcome outcome;
    const auto deadline = Clock::now() + std::chrono::seconds(input.spec.trial_timeout_sec);
    const auto period = std::chrono::milliseconds(input.spec.sample_period_ms);
    auto next_sample = Clock::now();
    bool sampling_started = false;
    const auto sample_capacity = CheckedAdd(CheckedMultiply(input.spec.trial_timeout_sec, 1000) /
                                            input.spec.sample_period_ms, 4);
    outcome.samples.reserve(sample_capacity);
    std::array<bool, 4> announced{};
    detail::WriterHooks writer_hooks;
    writer_hooks.after_add = [&](const qlib::record& record) { observer.Observe(record); };
    detail::ServiceHooks service_hooks;
    service_hooks.writer_hooks = &writer_hooks;
    service_hooks.lifecycle_context = &outcome.lifecycle;
    service_hooks.on_lifecycle = [](void* context, detail::ServicePhase phase) noexcept {
      static_cast<LifecycleTrace*>(context)->Record(static_cast<std::size_t>(phase));
    };
    service_hooks.coordinator_poll = [&](const Metrics& metrics) {
      Require(Clock::now() < deadline, "network worker deadline");
      if (!sampling_started) { next_sample = Clock::now(); sampling_started = true; }
      const std::array ready{observer.Reached(InputPhase::Prepare), observer.Reached(InputPhase::Measure),
                             observer.AllInputReached(), observer.FinalSnapshotsReached()};
      constexpr std::array markers{"bench_prepared_ready", "bench_measured_ready",
                                  "bench_all_input_ready", "bench_final_snapshots_ready"};
      // acquire 完成位后读取阶段计数；这里不读取 Writer 的普通字段。
      outcome.appended = CheckedAdd(CheckedAdd(metrics.writer_orders.Read(), metrics.writer_trades.Read()),
                                    metrics.writer_levels.Read());
      outcome.processed = metrics.book_events.Read();
      outcome.dropped = metrics.feed_dropped.Read();
      outcome.feed = metrics.feed_messages.Read();
      if (input.steady && Clock::now() >= next_sample) {
        Require(outcome.samples.size() < sample_capacity, "worker sample budget exhausted");
        const auto begin = Clock::now();
        PipelineSample sample;
        sample.begin_ns = Relative(outcome.lifecycle.origin, begin);
        sample.lateness_ns = CheckedDurationNs(next_sample, begin);
        // 下游先读仍非事务快照；有符号小负估计按冻结容差保留。
        sample.writer = CheckedAdd(CheckedAdd(metrics.writer_orders.Read(), metrics.writer_trades.Read()),
                                   metrics.writer_levels.Read());
        sample.book = metrics.book_events.Read();
        sample.dropped = metrics.feed_dropped.Read();
        sample.feed = metrics.feed_messages.Read();
        const auto end = Clock::now();
        sample.end_ns = Relative(outcome.lifecycle.origin, end);
        outcome.samples.push_back(sample);
        next_sample += (1 + (end - next_sample) / period) * period;
      }
      for (std::size_t i = 0; i < ready.size(); ++i) {
        if (ready[i] && !announced[i]) { announced[i] = true; std::cout << markers[i] << '\n' << std::flush; }
      }
    };
    outcome.exit_code = detail::RunServiceImpl(config, &service_hooks);
    outcome.lifecycle.returned = Clock::now();
    // malformed 不能在序列化后丢失；成功服务的完整时间表在本进程先校验。
    if (outcome.exit_code == 0) outcome.lifecycle.Check();
    for (std::size_t i = 0; i < kInputPhases; ++i) {
      const auto& phase = observer.Phases()[i];
      outcome.counts[i] = phase.count;
      if (phase.count) {
        outcome.first[i] = Relative(outcome.lifecycle.origin, phase.first);
        outcome.last[i] = Relative(outcome.lifecycle.origin, phase.last);
      }
    }
    outcome.snapshots = observer.SnapshotCount();
    // 真正 join 后的精确观察值；原子样本仍保留原来的逐字段观察语义。
    outcome.appended = observer.Count();
    WriteText(argv[4], WorkerOutcomeJson(outcome));
    return outcome.exit_code;
  } catch (const std::exception& error) {
    std::cerr << "pipeline worker failed: " << error.what() << '\n';
    return 2;
  }
}

std::vector<std::string> PipelineOperations() {
  return {"pipeline/controlled-queue/mbo", "pipeline/controlled-queue/l2",
          "pipeline/network-finite/mbo", "pipeline/network-finite/l2",
          "pipeline/network-steady/mbo", "pipeline/network-steady/l2"};
}

std::string PipelineWorkerConfiguration(const CaseSpec& spec, const ExperimentSpec& experiment) {
  std::ostringstream json;
  json << "{\"schema_version\":1,\"experiment_id\":\"pipeline-worker\",\"measurement_kind\":\"smoke\","
       << "\"seed\":" << experiment.seed << ",\"warmup_rounds\":0,\"repetitions\":1,"
       << "\"max_input_bytes\":" << experiment.max_input_bytes
       << ",\"max_output_bytes\":" << experiment.max_output_bytes << ",\"cases\":["
       << CanonicalCaseJson(spec) << "]}\n";
  auto document = fkyaml::node::deserialize(json.str());
  auto& parameters = document.at("cases").at(0).as_map();
  // manifest 的派生观测字段不是配置输入；worker 从原始容量重新计算并用 workload hash 核对。
  parameters.erase(fkyaml::node("actual_feed_queue_capacity"));
  parameters.erase(fkyaml::node("actual_record_queue_capacity"));
  return fkyaml::node::serialize(document);
}

PreparedBenchmark PreparePipelineBenchmark(const CaseSpec& spec, const ExperimentSpec& experiment) {
  ValidateCase(spec, experiment);
  const auto operations = PipelineOperations();
  Require(std::find(operations.begin(), operations.end(), spec.operation) != operations.end(),
          "unknown pipeline operation");
  Require((spec.mode == "Mbo" && spec.operation.ends_with("/mbo")) ||
          (spec.mode == "L2" && spec.operation.ends_with("/l2")), "operation and mode mismatch");
  if (spec.operation.find("/network-steady/") == std::string::npos)
    Require(spec.events_per_trial >= 2, "pipeline measured input needs two events");
  const auto slots = spec.mode == "L2" ? CheckedMultiply(2, CheckedMultiply(spec.instruments, spec.levels)) :
                                      spec.initial_orders;
  auto total = CheckedAdd(slots, spec.events_per_trial);
  if (spec.operation.find("/network-steady/") != std::string::npos)
    total = CheckedAdd(slots, CheckedMultiply(spec.input_rate,
        CheckedAdd(CheckedAdd(spec.warmup_sec, spec.window_sec), spec.guard_sec)));
  const auto memory = CheckedMultiply(total, sizeof(qlib::feed_event) + sizeof(testing::EncodedFrame) +
                                            sizeof(qlib::book) + sizeof(std::uint64_t));
  Require(memory <= experiment.max_input_bytes, "frozen input exceeds memory budget");
  // 未压缩列量与快照的保守预算；它不冒充实际 Parquet 大小。
  const auto snapshots = CheckedMultiply(CheckedAdd(
      CheckedMultiply(spec.trial_timeout_sec, 1000) / spec.snapshot_period_ms, 2), spec.instruments);
  Require(CheckedAdd(CheckedMultiply(total, 256), CheckedMultiply(snapshots, 256)) <= experiment.max_output_bytes,
          "frozen file workload exceeds output budget");
  auto input = std::make_shared<PipelineInput>(PrepareInput(spec, experiment.seed));
  input->max_input_bytes = experiment.max_input_bytes; input->max_output_bytes = experiment.max_output_bytes;
  PreparedBenchmark prepared;
  prepared.spec = spec; prepared.workload_hash = input->hash;
  prepared.instrumentation = "writer-completion-observer";
  if (!input->network) {
    prepared.phases = {
      {"input-admission", "accepted_events", "first measured push attempt through last successful measured push"},
      {"completed-window", "completed_raw_intervals", "Writer first measured Add through last measured Add, N-1"},
      {"completed-cycle", "validated_raw_records", "first measured push attempt through last measured Add"}};
  } else if (input->steady) {
    prepared.phases = {
      {"steady-input-window", "sent_events", "parent actual measure entry through guard entry"},
      {"steady-completed-window", "completed_raw_intervals", "worker first/last measured Add, N-1"},
      {"steady-tail-completion", "none", "parent guard end through all-input completion notification"}};
  } else {
    prepared.phases = {
      {"completed-window", "completed_raw_intervals", "worker first measured Add through last measured Add, N-1"},
      {"finite-completion", "validated_raw_records", "parent first measured send through completion notification"}};
  }
  prepared.phases.push_back({"drain-book", "none", "feed joined through Book joined"});
  prepared.phases.push_back({"drain-writer-close", "none", "Book joined through Writer joined including Close"});
  prepared.phases.push_back({"shutdown-total", "none", "stop observed through service return"});
  prepared.run = [input](const TrialContext& trial) {
    return input->network ? RunNetwork(*input, trial) : RunControlled(*input, trial);
  };
  return prepared;
}

PreparedBenchmark PrepareControlledPipelineBenchmark(const CaseSpec& spec,
    const ExperimentSpec& experiment, const detail::WriterHooks& hooks) {
  Require(spec.operation.find("/controlled-queue/") != std::string::npos,
          "fault seam is restricted to the controlled pipeline");
  auto prepared = PreparePipelineBenchmark(spec, experiment);
  auto input = std::make_shared<PipelineInput>(PrepareInput(spec, experiment.seed));
  input->max_input_bytes = experiment.max_input_bytes; input->max_output_bytes = experiment.max_output_bytes;
  prepared.run = [input, hooks](const TrialContext& trial) { return RunControlled(*input, trial, &hooks); };
  return prepared;
}
}  // namespace qbook::bench
