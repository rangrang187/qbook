#pragma once

#include <chrono>
#include <limits>
#include <ratio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace qbook::detail {

// 按实际时钟比例受检转换正毫秒数，转换溢出时指出配置字段。
inline std::chrono::steady_clock::duration CheckedSteadyPeriod(
    std::chrono::milliseconds period, std::string_view field) {
  using Duration = std::chrono::steady_clock::duration;
  using Rep = Duration::rep;
  using Ratio = std::ratio_divide<std::chrono::milliseconds::period, Duration::period>;
  static_assert(std::is_integral_v<Rep> && std::is_signed_v<Rep>);
  static_assert(Ratio::den == 1, "当前配置要求每毫秒包含整数个 steady_clock tick");
  const auto count = period.count();
  if (count <= 0 || !std::in_range<Rep>(count) ||
      count > std::numeric_limits<Rep>::max() / Ratio::num) {
    throw std::invalid_argument(std::string(field) + ": 周期应为可转换为调度时钟的正毫秒数");
  }
  return Duration(static_cast<Rep>(count) * Ratio::num);
}

// 将已验证的正时长加到观察时刻；结果越界时保留字段名并抛异常。
inline std::chrono::steady_clock::time_point CheckedDeadline(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration period, std::string_view field) {
  using Clock = std::chrono::steady_clock;
  const auto current = now.time_since_epoch().count();
  const auto delta = period.count();
  if (delta <= 0 || current > std::numeric_limits<Clock::rep>::max() - delta) {
    throw std::invalid_argument(std::string(field) + ": 下一到期时间超出调度时钟范围");
  }
  return Clock::time_point(Clock::duration(current + delta));
}

}  // namespace qbook::detail
