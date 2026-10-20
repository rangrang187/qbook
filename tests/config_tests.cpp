#include "Config.h"
#include "PipelineTypes.h"
#include "../src/ConfigChecks.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;

class ConfigFiles {
 public:
  ConfigFiles() {
    static std::atomic<unsigned> serial{0};
    path_ = std::filesystem::temp_directory_path() /
            ("qbook-config-" + std::to_string(::getpid()) + "-" +
             std::to_string(serial.fetch_add(1)));
    if (!std::filesystem::create_directory(path_)) {
      throw std::runtime_error("配置测试临时目录已存在");
    }
  }
  ~ConfigFiles() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  std::filesystem::path Write(std::string_view text) const {
    const auto file = path_ / "config.yaml";
    std::ofstream output(file);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output << text;
    output.close();
    return file;
  }
  const std::filesystem::path& Path() const { return path_; }

 private:
  std::filesystem::path path_;
};

class CaptureLog {
 public:
  CaptureLog() : previous_(spdlog::default_logger()) {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(stream_);
    auto logger = std::make_shared<spdlog::logger>("config-test", sink);
    logger->set_pattern("%l %v");
    spdlog::set_default_logger(std::move(logger));
  }
  ~CaptureLog() { spdlog::set_default_logger(std::move(previous_)); }
  std::string Text() const { return stream_.str(); }

 private:
  std::ostringstream stream_;
  std::shared_ptr<spdlog::logger> previous_;
};

void CheckDefaults(const qbook::Config& config) {
  CHECK(config.feed_endpoint == "tcp://127.0.0.1:5555");
  CHECK_FALSE(config.l2);
  CHECK(config.feed_queue_capacity == 16384);
  CHECK(config.snapshot_period == 3000ms);
  CHECK(config.record_queue_capacity == 16384);
  CHECK(config.out_dir == "data_out");
  CHECK(config.batch_rows == 1024);
  CHECK(config.metrics_endpoint == "tcp://127.0.0.1:5556");
  CHECK(config.sample_period == 100ms);
}
}  // namespace

TEST_CASE("config defaults and empty mapping preserve nine defaults", "[config]") {
  ConfigFiles files;
  CheckDefaults(qbook::Config{});
  CHECK_NOTHROW(qbook::ValidateConfig(qbook::Config{}));
  CheckDefaults(qbook::LoadConfig(files.Write("{}")));
}

TEST_CASE("config missing file warns and returns validated defaults", "[config]") {
  ConfigFiles files;
  const auto missing = files.Path() / "missing.yaml";
  CaptureLog log;
  CheckDefaults(qbook::LoadConfig(missing));
  CHECK_THAT(log.Text(), ContainsSubstring("warning"));
  CHECK_THAT(log.Text(), ContainsSubstring(missing.string()));
  CHECK_FALSE(std::filesystem::exists(missing));
}

TEST_CASE("config repository example loads documented defaults", "[config]") {
  const auto example = std::filesystem::path(__FILE__).parent_path().parent_path() /
                       "config/config.yaml";
  REQUIRE(std::filesystem::is_regular_file(example));
  CheckDefaults(qbook::LoadConfig(example));
}

TEST_CASE("config partial override retains request and independent values", "[config]") {
  ConfigFiles files;
  auto first = qbook::LoadConfig(files.Write("feed_queue_capacity: 3"));
  CHECK(first.feed_queue_capacity == 3);
  CHECK(first.record_queue_capacity == 16384);
  CHECK(first.snapshot_period == 3000ms);
  first.feed_endpoint = "changed";
  CheckDefaults(qbook::LoadConfig(files.Write("{}")));
  CHECK(first.feed_endpoint == "changed");
}

TEST_CASE("config complete mapping returns all overrides without output resources", "[config]") {
  ConfigFiles files;
  const auto output = files.Path() / "output";
  const auto config = qbook::LoadConfig(files.Write(
      "feed_endpoint: 'ipc:///tmp/qbook-feed'\n"
      "l2: true\nfeed_queue_capacity: 3\nsnapshot_period: 17\n"
      "record_queue_capacity: 4\nout_dir: '" + output.string() + "'\n"
      "batch_rows: 7\nmetrics_endpoint: 'tcp://127.0.0.1:6000'\nsample_period: 2\n"));
  CHECK(config.feed_endpoint == "ipc:///tmp/qbook-feed");
  CHECK(config.l2);
  CHECK(config.feed_queue_capacity == 3);
  CHECK(config.snapshot_period == 17ms);
  CHECK(config.record_queue_capacity == 4);
  CHECK(config.out_dir == output);
  CHECK(config.batch_rows == 7);
  CHECK(config.metrics_endpoint == "tcp://127.0.0.1:6000");
  CHECK(config.sample_period == 2ms);
  CHECK_FALSE(std::filesystem::exists(output));
}

TEST_CASE("config empty path and existing nonfiles are errors", "[config]") {
  ConfigFiles files;
  CHECK_THROWS_AS(qbook::LoadConfig({}), std::invalid_argument);
  CHECK_THROWS_WITH(qbook::LoadConfig(files.Path()), ContainsSubstring(files.Path().string()));
  const auto normal = files.Write("{}");
  CHECK_THROWS_WITH(qbook::LoadConfig(normal / "child"), ContainsSubstring("child"));
}

TEST_CASE("config actual read failure does not become missing defaults", "[config]") {
  // Linux 的进程内存文件在偏移 0 读取返回 EIO，读取结果与运行身份无关。
  const std::filesystem::path unreadable("/proc/self/mem");
  REQUIRE(std::filesystem::is_regular_file(unreadable));
  CHECK_THROWS_WITH(qbook::LoadConfig(unreadable), ContainsSubstring(unreadable.string()));
}

TEST_CASE("config invalid root and mapping structure are rejected", "[config]") {
  ConfigFiles files;
  for (const auto text : {"", "\n  \n", "null", "[]", "12", "true", "[1, 2]",
                          "1: value", "unknown: 1", "feed:\n  endpoint: x",
                          "l2: true\nl2: false", "feed_queue_capacity: 3\nfeed_queue_capacity: 4",
                          "feed_queue_capacity: [1", "{}\n---\n{l2: true}"}) {
    DYNAMIC_SECTION("YAML: " << text) {
      CHECK_THROWS_AS(qbook::LoadConfig(files.Write(text)), std::invalid_argument);
    }
  }
}

TEST_CASE("config null and incompatible scalar types name the field", "[config]") {
  ConfigFiles files;
  for (const auto* field : {"feed_endpoint", "l2", "feed_queue_capacity", "snapshot_period",
                            "record_queue_capacity", "out_dir", "batch_rows",
                            "metrics_endpoint", "sample_period"}) {
    DYNAMIC_SECTION("null: " << field) {
      CHECK_THROWS_WITH(qbook::LoadConfig(files.Write(std::string(field) + ": null")),
                        ContainsSubstring(field));
    }
  }
  for (const auto text : {"feed_endpoint: 4", "feed_endpoint: true", "out_dir: false",
                          "metrics_endpoint: 4", "l2: 'false'", "l2: 0", "l2: []",
                          "feed_queue_capacity: '3'", "feed_queue_capacity: 3.0",
                          "record_queue_capacity: true", "snapshot_period: '3'",
                          "sample_period: 1.5", "batch_rows: false", "batch_rows: {}"}) {
    const std::string value(text);
    DYNAMIC_SECTION("YAML: " << value) {
      CHECK_THROWS_WITH(qbook::LoadConfig(files.Write(value)),
                        ContainsSubstring(value.substr(0, value.find(':'))));
    }
  }
}

TEST_CASE("config integers reject nonpositive and parser overflow", "[config]") {
  ConfigFiles files;
  for (const auto* field : {"feed_queue_capacity", "record_queue_capacity", "snapshot_period",
                            "sample_period", "batch_rows"}) {
    for (const auto value : {"0", "-1", "9223372036854775808", "18446744073709551616"}) {
      const auto yaml = std::string(field) + ": " + value;
      DYNAMIC_SECTION(yaml) {
        CHECK_THROWS_AS(qbook::LoadConfig(files.Write(yaml)), std::invalid_argument);
      }
    }
  }
}

TEST_CASE("config failed load does not change later successful loads", "[config]") {
  ConfigFiles files;
  CHECK_THROWS_AS(qbook::LoadConfig(files.Write("l2: 'false'")), std::invalid_argument);
  CheckDefaults(qbook::LoadConfig(files.Write("{}")));
  CHECK(qbook::LoadConfig(files.Write("l2: true")).l2);
}

TEST_CASE("config direct empty fields identify invalid member", "[config]") {
  for (const auto* field : {"feed_endpoint", "metrics_endpoint", "out_dir"}) {
    qbook::Config config;
    if (std::string_view(field) == "feed_endpoint") config.feed_endpoint.clear();
    if (std::string_view(field) == "metrics_endpoint") config.metrics_endpoint.clear();
    if (std::string_view(field) == "out_dir") config.out_dir.clear();
    CHECK_THROWS_WITH(qbook::ValidateConfig(config), ContainsSubstring(field));
  }
}

TEST_CASE("config capacities reject rounding and storage overflow before allocation", "[config]") {
  const auto max = std::numeric_limits<std::size_t>::max();
  for (const bool feed : {true, false}) {
    const auto field = feed ? "feed_queue_capacity" : "record_queue_capacity";
    const auto value_size = feed ? sizeof(qlib::feed_event) : sizeof(qlib::record);
    const auto safe_capacity = std::bit_floor(max / value_size);
    qbook::Config config;
    auto& capacity = feed ? config.feed_queue_capacity : config.record_queue_capacity;
    capacity = safe_capacity;
    CHECK_NOTHROW(qbook::ValidateConfig(config));
    CHECK(capacity == safe_capacity);
    for (const auto bad : {std::size_t{0}, safe_capacity + 1, std::bit_floor(max),
                           std::bit_floor(max) + 1, max}) {
      capacity = bad;
      CHECK_THROWS_WITH(qbook::ValidateConfig(config), ContainsSubstring(field));
    }
  }
}

TEST_CASE("config valid small capacities construct actual queues", "[config]") {
  for (const auto requested : {std::size_t{1}, std::size_t{3}, std::size_t{4}}) {
    qbook::Config config;
    config.feed_queue_capacity = requested;
    config.record_queue_capacity = requested;
    qbook::ValidateConfig(config);
    qbook::FeedQueue feed(config.feed_queue_capacity);
    qbook::RecordQueue record(config.record_queue_capacity);
    CHECK(feed.capacity() == (requested == 1 ? 1 : 4));
    CHECK(record.capacity() == (requested == 1 ? 1 : 4));
    CHECK(config.feed_queue_capacity == requested);
    CHECK(config.record_queue_capacity == requested);
  }
}

TEST_CASE("config direct periods reject nonpositive and conversion overflow", "[config]") {
  using Clock = std::chrono::steady_clock;
  static_assert(std::ratio_equal_v<Clock::period, std::nano>);
  const auto largest_ms = std::numeric_limits<Clock::rep>::max() / 1'000'000;
  for (const bool snapshot : {true, false}) {
    qbook::Config config;
    auto& period = snapshot ? config.snapshot_period : config.sample_period;
    const auto field = snapshot ? "snapshot_period" : "sample_period";
    period = 1ms;
    CHECK_NOTHROW(qbook::ValidateConfig(config));
    period = std::chrono::milliseconds(largest_ms / 2);
    CHECK_NOTHROW(qbook::ValidateConfig(config));
    for (const auto count : {std::int64_t{0}, std::int64_t{-1}, largest_ms + 1,
                             std::numeric_limits<std::int64_t>::max()}) {
      period = std::chrono::milliseconds(count);
      CHECK_THROWS_WITH(qbook::ValidateConfig(config), ContainsSubstring(field));
    }
    period = std::chrono::milliseconds(largest_ms);
    CHECK_THROWS_WITH(qbook::ValidateConfig(config), ContainsSubstring(field));
  }
}

TEST_CASE("config deadline checks exact representable addition boundary", "[config]") {
  using Clock = std::chrono::steady_clock;
  const auto max = std::numeric_limits<Clock::rep>::max();
  const auto min = std::numeric_limits<Clock::rep>::min();
  const auto tick = Clock::duration(1);
  CHECK(qbook::detail::CheckedDeadline(Clock::time_point(Clock::duration(max - 1)), tick,
                                       "sample_period").time_since_epoch().count() == max);
  CHECK_THROWS_WITH(qbook::detail::CheckedDeadline(Clock::time_point(Clock::duration(max)), tick,
                                                  "sample_period"),
                    ContainsSubstring("sample_period"));
  CHECK(qbook::detail::CheckedDeadline(Clock::time_point(Clock::duration(min)), tick,
                                       "snapshot_period").time_since_epoch().count() == min + 1);
}

TEST_CASE("config steady period converts exact maximum before deadline validation", "[config]") {
  using Clock = std::chrono::steady_clock;
  static_assert(std::ratio_equal_v<Clock::period, std::nano>);
  const auto max_ms = std::numeric_limits<Clock::rep>::max() / 1'000'000;
  const auto converted = qbook::detail::CheckedSteadyPeriod(
      std::chrono::milliseconds(max_ms), "sample_period");
  CHECK(converted.count() == max_ms * 1'000'000);
  CHECK(qbook::detail::CheckedSteadyPeriod(1ms, "sample_period").count() == 1'000'000);
  CHECK_THROWS_WITH(qbook::detail::CheckedSteadyPeriod(
                        std::chrono::milliseconds(max_ms + 1), "sample_period"),
                    ContainsSubstring("sample_period"));
}

TEST_CASE("config batch rows reject scalar and list byte overflow", "[config]") {
  qbook::Config config;
  for (const auto rows : {std::int64_t{0}, std::int64_t{-1},
                          std::numeric_limits<std::int64_t>::max()}) {
    config.batch_rows = rows;
    CHECK_THROWS_WITH(qbook::ValidateConfig(config), ContainsSubstring("batch_rows"));
  }
  config.batch_rows = static_cast<std::int64_t>(
      std::numeric_limits<std::size_t>::max() / sizeof(std::int64_t) / qlib::snapshot_depth);
  CHECK_NOTHROW(qbook::ValidateConfig(config));
  ++config.batch_rows;
  CHECK_THROWS_WITH(qbook::ValidateConfig(config), ContainsSubstring("batch_rows"));
}
