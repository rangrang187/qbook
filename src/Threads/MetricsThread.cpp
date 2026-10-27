#include "Threads/MetricsThread.h"

#include "Runtime.h"
#include "../ConfigChecks.h"
#include "../detail/MetricsLayout.h"
#include "../detail/MetricsSend.h"

#include <spdlog/spdlog.h>
#include <zmq.hpp>

#include <cerrno>
#include <limits>
#include <ratio>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace qbook {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr auto kStopCheck = 100ms;

std::int64_t SampleTime() {
  using SystemClock = std::chrono::system_clock;
  using Ratio = std::ratio_divide<SystemClock::period, std::nano>;
  static_assert(std::is_integral_v<SystemClock::rep> && std::is_signed_v<SystemClock::rep>);
  static_assert(Ratio::den == 1, "采样时钟须由整数纳秒 tick 表示");
  const auto ticks = SystemClock::now().time_since_epoch().count();
  if (!std::in_range<std::int64_t>(ticks) ||
      ticks > std::numeric_limits<std::int64_t>::max() / Ratio::num ||
      ticks < std::numeric_limits<std::int64_t>::min() / Ratio::num)
    throw std::overflow_error("本机采样时间超出 int64 纳秒范围");
  return static_cast<std::int64_t>(ticks) * Ratio::num;
}
}  // namespace

MetricsThread::MetricsThread(std::string endpoint, std::chrono::milliseconds sample_period)
    : endpoint_(std::move(endpoint)), sample_period_(sample_period) {
  if (endpoint_.empty()) throw std::invalid_argument("metrics_endpoint 不能为空");
  (void)detail::CheckedSteadyPeriod(sample_period_, "sample_period_ms");
}

void MetricsThread::Bind(zmq::socket_t& pub) {
  pub.set(zmq::sockopt::conflate, true);
  pub.set(zmq::sockopt::linger, 0);
  pub.bind(endpoint_);
  spdlog::info("metrics_bound Metrics 发布资源已初始化：{}",
               pub.get(zmq::sockopt::last_endpoint));
}

bool MetricsThread::Publish(zmq::socket_t& pub, const Metrics& metrics) {
  const auto sample = SnapshotMetrics(metrics, SampleTime());
  // const_buffer 的 send 在返回前复制内容；不让栈对象成为异步无复制缓冲。
  return detail::MetricsSendAccepted(
      pub.send(zmq::buffer(&sample, sizeof sample), zmq::send_flags::dontwait));
}

void MetricsThread::Run(const Metrics& metrics, std::stop_token stop, const RunControl& control) {
  if (stop.stop_requested() || control.Failed()) return;
  zmq::context_t context(1);
  zmq::socket_t pub(context, zmq::socket_type::pub);
  Bind(pub);
  const auto period = detail::CheckedSteadyPeriod(sample_period_, "sample_period_ms");
  const auto check = detail::CheckedSteadyPeriod(kStopCheck, "metrics_stop_check_ms");
  auto next_due = detail::CheckedDeadline(Clock::now(), period, "sample_period_ms");
  bool diagnosed_unavailable = false;
  while (!stop.stop_requested() && !control.Failed()) {
    const auto now = Clock::now();
    if (now < next_due) {
      // 当剩余量不足 100ms 时直接用已检查的期限，避免额外加法越界。
      const auto remaining = next_due - now;
      std::this_thread::sleep_until(remaining <= check ? next_due :
          detail::CheckedDeadline(now, check, "metrics_stop_check_ms"));
      continue;
    }
    try {
      if (!Publish(pub, metrics) && !diagnosed_unavailable) {
        diagnosed_unavailable = true;
        spdlog::warn("Metrics 非阻塞发布暂不可用，跳过样本；后续同类诊断限为本次运行一次");
      }
    } catch (const zmq::error_t& error) {
      if (error.num() != EINTR) throw;
      if (stop.stop_requested() || control.Failed()) return;
    }
    if (stop.stop_requested() || control.Failed()) break;
    // 从本轮完成时间安排下一轮；调度迟到不会补发已经错过的历史周期。
    next_due = detail::CheckedDeadline(Clock::now(), period, "sample_period_ms");
  }
}
}  // namespace qbook
