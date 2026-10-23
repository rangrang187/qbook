#include "Threads/FeedThread.h"

#include "Protocol.h"
#include "Runtime.h"
#include "Threads/MetricsThread.h"

#include <spdlog/spdlog.h>
#include <zmq.hpp>

#include <cerrno>
#include <chrono>
#include <limits>
#include <ratio>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace qbook {
namespace {
using namespace std::chrono_literals;
constexpr auto kReceiveTimeout = 100ms;
constexpr auto kQueueRetryPause = 50us;

std::int64_t ReceiveTime() {
  using Clock = std::chrono::system_clock;
  using Ratio = std::ratio_divide<Clock::period, std::nano>;
  static_assert(std::is_integral_v<Clock::rep> && std::is_signed_v<Clock::rep>);
  static_assert(Ratio::den == 1, "接收时钟须由整数纳秒 tick 表示");
  const auto ticks = Clock::now().time_since_epoch().count();
  if (!std::in_range<std::int64_t>(ticks) ||
      ticks > std::numeric_limits<std::int64_t>::max() / Ratio::num ||
      ticks < std::numeric_limits<std::int64_t>::min() / Ratio::num) {
    throw std::overflow_error("本机接收时间超出 int64 纳秒范围");
  }
  return static_cast<std::int64_t>(ticks) * Ratio::num;
}

std::uint64_t ByteCount(std::size_t size) {
  if (!std::in_range<std::uint64_t>(size)) throw std::overflow_error("行情帧长度超出 uint64 范围");
  return static_cast<std::uint64_t>(size);
}

const char* Reason(DecodeError error) noexcept {
  switch (error) {
    case DecodeError::EmptyFrame: return "空帧";
    case DecodeError::UnknownTag: return "未知标签";
    case DecodeError::WrongSize: return "长度不符";
    case DecodeError::ModeMismatch: return "模式不符";
    case DecodeError::InvalidField: return "字段不合法";
    case DecodeError::None: return "无错误";
  }
  return "未知解码错误";
}

void Reject(Metrics& metrics, const char* reason) {
  metrics.feed_dropped.Add();
  const auto count = metrics.feed_dropped.Read();
  if (count == 1 || count % 1024 == 0)
    spdlog::warn("拒绝行情：{}，累计拒绝 {} 条", reason, count);
}
}  // namespace

FeedThread::FeedThread(std::string endpoint, BookMode mode, FeedQueue& output)
    : endpoint_(std::move(endpoint)), mode_(mode), feed_queue_(output) {}

void FeedThread::Connect(zmq::socket_t& sub) {
  sub.set(zmq::sockopt::rcvtimeo, static_cast<int>(kReceiveTimeout.count()));
  sub.set(zmq::sockopt::linger, 0);
  sub.set(zmq::sockopt::subscribe, "");
  sub.connect(endpoint_);
}

bool FeedThread::Receive(zmq::socket_t& sub, zmq::message_t& message) {
  try {
    // 零字节帧也已收到，optional 的存在性区分它与超时。
    return sub.recv(message, zmq::recv_flags::none).has_value();
  } catch (const zmq::error_t& error) {
    if (error.num() == EINTR) return false;
    throw;
  }
}

bool FeedThread::Enqueue(qlib::feed_event&& event, std::stop_token stop,
                         const RunControl& control) {
  while (!control.Failed() && !stop.stop_requested()) {
    if (feed_queue_.try_push(std::move(event))) return true;
    std::this_thread::sleep_for(kQueueRetryPause);
  }
  return false;
}

void FeedThread::Consume(std::span<const std::byte> bytes, std::int64_t recv_ns,
                         Metrics& metrics, std::stop_token stop, const RunControl& control) {
  metrics.feed_messages.Add();
  metrics.feed_bytes.Add(ByteCount(bytes.size()));
  auto decoded = DecodeFrame(bytes, recv_ns, mode_);
  if (!decoded.event) {
    Reject(metrics, Reason(decoded.error));
    return;
  }
  std::visit(qlib::overloaded{
      [&](const qlib::order&) { metrics.feed_orders.Add(); },
      [&](const qlib::cancel&) { metrics.feed_orders.Add(); },
      [&](const qlib::trade&) { metrics.feed_trades.Add(); },
      [&](const qlib::level&) { metrics.feed_levels.Add(); }}, *decoded.event);
  Enqueue(std::move(*decoded.event), stop, control);
}

void FeedThread::Run(Metrics& metrics, std::stop_token stop, const RunControl& control) {
  if (stop.stop_requested() || control.Failed()) return;
  zmq::context_t context(1);
  zmq::socket_t sub(context, zmq::socket_type::sub);
  Connect(sub);
  spdlog::info("Feed 接收资源已初始化：{}", endpoint_);
  zmq::message_t message;
  while (!stop.stop_requested() && !control.Failed()) {
    if (!Receive(sub, message)) continue;
    const auto recv_ns = ReceiveTime();
    if (!message.more()) {
      Consume({static_cast<const std::byte*>(message.data()), message.size()},
              recv_ns, metrics, stop, control);
      continue;
    }
    auto bytes = ByteCount(message.size());
    // 一直排到同一消息的末段；超时或 EINTR 不改变当前 multipart 边界。
    bool more = true;
    while (more) {
      if (stop.stop_requested() || control.Failed()) return;
      if (!Receive(sub, message)) continue;
      const auto part = ByteCount(message.size());
      if (part > std::numeric_limits<std::uint64_t>::max() - bytes)
        throw std::overflow_error("多段行情总长度超出 uint64 范围");
      bytes += part;
      more = message.more();
    }
    metrics.feed_messages.Add();
    metrics.feed_bytes.Add(bytes);
    Reject(metrics, "不支持多段消息");
  }
  spdlog::info("Feed 接收阶段结束");
}
}  // namespace qbook
