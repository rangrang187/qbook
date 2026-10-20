#include "Config.h"

#include "ConfigChecks.h"
#include "PipelineTypes.h"

#include <fkYAML/node.hpp>
#include <spdlog/spdlog.h>

#include <bit>
#include <cerrno>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace qbook {
namespace {

[[noreturn]] void Invalid(std::string_view field, std::string_view reason) {
  throw std::invalid_argument(std::string(field) + ": " + std::string(reason));
}

std::size_t CheckedQueueCapacity(std::size_t requested, std::size_t value_size,
                                 std::string_view field) {
  const auto max = std::numeric_limits<std::size_t>::max();
  if (requested == 0 || requested > std::bit_floor(max)) {
    Invalid(field, "容量应为可向上取整为 2 的幂的正整数");
  }
  const auto capacity = std::bit_ceil(requested);
  if (!std::in_range<std::uint64_t>(capacity) || capacity > max / value_size) {
    Invalid(field, "取整后的容量或槽存储字节数超出范围");
  }
  return capacity;
}

template <typename Target>
Target PositiveInteger(const fkyaml::node& node, std::string_view field) {
  if (!node.is_integer()) Invalid(field, "应为正整数");
  fkyaml::node::integer_type value;
  try {
    value = node.as_int();
  } catch (const fkyaml::exception&) {
    Invalid(field, "整数超出可读取范围");
  }
  if (value <= 0 || !std::in_range<Target>(value)) {
    Invalid(field, "应为目标类型可表示的正整数");
  }
  return static_cast<Target>(value);
}

std::string StringValue(const fkyaml::node& node, std::string_view field) {
  if (!node.is_string()) Invalid(field, "应为字符串");
  return node.as_str();
}

std::error_code ReadError() {
  return errno != 0 ? std::error_code(errno, std::generic_category())
                    : std::make_error_code(std::errc::io_error);
}

std::string ReadFile(const std::filesystem::path& path) {
  errno = 0;
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    throw std::filesystem::filesystem_error("打开配置文件失败", path, ReadError());
  }
  std::string text;
  char buffer[4096];
  for (;;) {
    errno = 0;
    input.read(buffer, sizeof(buffer));
    const auto error = ReadError();
    text.append(buffer, static_cast<std::size_t>(input.gcount()));
    if (input.bad() || (input.fail() && !input.eof())) {
      throw std::filesystem::filesystem_error("读取配置文件失败", path, error);
    }
    if (input.eof()) break;
  }
  return text;
}

void ApplyField(Config& config, std::string_view field, const fkyaml::node& value) {
  if (field == "feed_endpoint") config.feed_endpoint = StringValue(value, field);
  else if (field == "l2") {
    if (!value.is_boolean()) Invalid(field, "应为布尔值");
    config.l2 = value.as_bool();
  } else if (field == "feed_queue_capacity") {
    config.feed_queue_capacity = PositiveInteger<std::size_t>(value, field);
  } else if (field == "snapshot_period") {
    config.snapshot_period = std::chrono::milliseconds(
        PositiveInteger<std::chrono::milliseconds::rep>(value, field));
  } else if (field == "record_queue_capacity") {
    config.record_queue_capacity = PositiveInteger<std::size_t>(value, field);
  } else if (field == "out_dir") config.out_dir = StringValue(value, field);
  else if (field == "batch_rows") config.batch_rows = PositiveInteger<std::int64_t>(value, field);
  else if (field == "metrics_endpoint") config.metrics_endpoint = StringValue(value, field);
  else if (field == "sample_period") {
    config.sample_period = std::chrono::milliseconds(
        PositiveInteger<std::chrono::milliseconds::rep>(value, field));
  } else Invalid(field, "未知配置项");
}

}  // namespace

void ValidateConfig(const Config& config) {
  if (config.feed_endpoint.empty()) Invalid("feed_endpoint", "端点应为非空字符串");
  if (config.metrics_endpoint.empty()) Invalid("metrics_endpoint", "端点应为非空字符串");
  if (config.out_dir.empty()) Invalid("out_dir", "路径应为非空字符串");
  CheckedQueueCapacity(config.feed_queue_capacity, sizeof(qlib::feed_event), "feed_queue_capacity");
  CheckedQueueCapacity(config.record_queue_capacity, sizeof(qlib::record), "record_queue_capacity");

  const auto snapshot = detail::CheckedSteadyPeriod(config.snapshot_period, "snapshot_period");
  const auto sample = detail::CheckedSteadyPeriod(config.sample_period, "sample_period");
  const auto now = std::chrono::steady_clock::now();
  detail::CheckedDeadline(now, snapshot, "snapshot_period");
  detail::CheckedDeadline(now, sample, "sample_period");

  constexpr auto depth = qlib::snapshot_depth;
  static_assert(depth > 0);
  if (config.batch_rows <= 0 ||
      config.batch_rows > std::numeric_limits<std::int64_t>::max() / depth) {
    Invalid("batch_rows", "行数与快照列表元素数应为可表示的正整数");
  }
  const auto elements = config.batch_rows * depth;
  const auto max_elements = std::numeric_limits<std::size_t>::max() / sizeof(std::int64_t);
  if (!std::in_range<std::size_t>(config.batch_rows) ||
      !std::in_range<std::size_t>(elements) ||
      static_cast<std::size_t>(config.batch_rows) > max_elements ||
      static_cast<std::size_t>(elements) > max_elements) {
    Invalid("batch_rows", "标量列或快照列表字节数超出范围");
  }
}

Config LoadConfig(const std::filesystem::path& path) {
  if (path.empty()) Invalid("path", "配置路径应为非空路径");
  Config config;
  std::error_code error;
  const auto link_status = std::filesystem::symlink_status(path, error);
  if (link_status.type() == std::filesystem::file_type::not_found &&
      (!error || error == std::errc::no_such_file_or_directory)) {
    spdlog::warn("配置文件 {} 不存在，使用默认值", path.string());
    ValidateConfig(config);
    return config;
  }
  if (error) throw std::filesystem::filesystem_error("查询配置路径失败", path, error);
  const auto status = std::filesystem::status(path, error);
  if (error) throw std::filesystem::filesystem_error("查询配置文件失败", path, error);
  if (!std::filesystem::is_regular_file(status)) {
    throw std::runtime_error("配置路径应为普通文件: " + path.string());
  }

  const auto text = ReadFile(path);
  try {
    const auto documents = fkyaml::node::deserialize_docs(text);
    if (documents.size() != 1 || !documents.front().is_mapping()) {
      Invalid("root", "配置应包含一个平坦映射文档");
    }
    for (const auto& [key, value] : documents.front().as_map()) {
      if (!key.is_string()) Invalid("root", "映射键应为字符串");
      ApplyField(config, key.as_str(), value);
    }
  } catch (const fkyaml::exception& exception) {
    throw std::invalid_argument("配置解析失败 [" + path.string() + "]: " + exception.what());
  }
  ValidateConfig(config);
  return config;
}
}  // namespace qbook
