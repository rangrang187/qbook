#include "Threads/BookThread.h"

#include "../ConfigChecks.h"

#include <limits>
#include <optional>
#include <ratio>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#include <spdlog/spdlog.h>

namespace qbook {
namespace {
using Clock = std::chrono::steady_clock;
using Tick = Clock::duration::rep;
using NanosecondRatio = std::ratio_divide<Clock::period, std::nano>;
constexpr auto count_max = std::numeric_limits<std::uint64_t>::max();
constexpr auto size_max = std::numeric_limits<std::size_t>::max();
constexpr auto tick_min = std::numeric_limits<Tick>::min();
constexpr auto tick_max = std::numeric_limits<Tick>::max();
constexpr auto book_slot_bytes =
    sizeof(qlib::map<std::uint32_t, Orderbook>::value_type) + sizeof(std::uint8_t);
static_assert(std::is_integral_v<Tick> && std::is_signed_v<Tick>);
static_assert(std::numeric_limits<Tick>::digits <= 63);
static_assert(NanosecondRatio::den == 1, "当前平台要求 steady_clock tick 为整数纳秒");
static_assert(std::is_nothrow_move_constructible_v<
    qlib::map<std::uint32_t, Orderbook>::value_type>);

constexpr bool CanGrowBooks(std::size_t capacity) noexcept {
  if (capacity > size_max / 2) return false;
  const auto next = capacity == 0 ? 8 : capacity * 2;
  return next <= (size_max - 1) / book_slot_bytes;
}

constexpr std::uint64_t SaturatingAdd(std::uint64_t a, std::uint64_t b) noexcept {
  return a > count_max - b ? count_max : a + b;
}

// 先检查有符号差，再检查纳秒倍率；任何一步失败都不执行越界运算。
constexpr std::optional<std::uint64_t> ElapsedNanoseconds(Tick begin, Tick end) noexcept {
  if (end < begin || (begin < 0 && end > tick_max + begin)) return std::nullopt;
  const auto ticks = static_cast<std::uint64_t>(end - begin);
  if (ticks > count_max / NanosecondRatio::num) return std::nullopt;
  return ticks * NanosecondRatio::num;
}

std::uint64_t CheckedElapsed(Clock::time_point begin, Clock::time_point end) {
  const auto value = ElapsedNanoseconds(begin.time_since_epoch().count(),
                                        end.time_since_epoch().count());
  if (!value) throw std::overflow_error("Book Apply 耗时超出可表示范围");
  return *value;
}

std::uint64_t CheckedCount(std::size_t value) {
  if (!std::in_range<std::uint64_t>(value))
    throw std::overflow_error("Book 状态计数超出 uint64 范围");
  return static_cast<std::uint64_t>(value);
}

std::uint64_t AddCount(std::uint64_t total, std::uint64_t value) {
  if (value > count_max - total)
    throw std::overflow_error("Book 状态计数合计超出 uint64 范围");
  return total + value;
}

constexpr std::uint64_t MemoryCount(std::size_t value) noexcept {
  return std::in_range<std::uint64_t>(value)
      ? static_cast<std::uint64_t>(value) : count_max;
}

constexpr auto largest_capacity = (size_max - 1) / book_slot_bytes / 2;
static_assert(CanGrowBooks(0) && CanGrowBooks(8));
static_assert(CanGrowBooks(largest_capacity));
static_assert(!CanGrowBooks(largest_capacity + 1));
static_assert(!CanGrowBooks(size_max / 2 + 1));
static_assert(SaturatingAdd(0, 0) == 0);
static_assert(SaturatingAdd(count_max - 1, 1) == count_max);
static_assert(SaturatingAdd(count_max, 1) == count_max);
static_assert(ElapsedNanoseconds(0, 0) == 0);
static_assert(ElapsedNanoseconds(-1, 0) == NanosecondRatio::num);
static_assert(!ElapsedNanoseconds(1, 0));
static_assert(!ElapsedNanoseconds(tick_min, 0));
static_assert(!ElapsedNanoseconds(-1, tick_max));
static_assert(ElapsedNanoseconds(tick_min, tick_min) == 0);
static_assert(ElapsedNanoseconds(tick_max, tick_max) == 0);
}  // namespace

BookThread::BookThread(FeedQueue& input, RecordQueue& output, BookMode mode,
                       std::chrono::milliseconds snapshot_period)
    : feed_queue_(input), record_queue_(output), mode_(mode), snapshot_period_(snapshot_period) {
  if (mode != BookMode::Mbo && mode != BookMode::L2)
    throw std::invalid_argument("Book 建簿模式无效");
  (void)detail::CheckedSteadyPeriod(snapshot_period_, "snapshot_period_ms");
}

void BookThread::Run(Metrics& metrics, std::stop_token stop, const RunControl& control) {
  if (control.Failed()) return;
  const auto period = detail::CheckedSteadyPeriod(snapshot_period_, "snapshot_period_ms");
  auto next_due = detail::CheckedDeadline(Clock::now(), period, "snapshot_period_ms");
  for (;;) {
    if (control.Failed()) return;
    // 先接收上游 join 后发布的停止请求，再确认输入为空，保留最后一次成功入队。
    const bool draining = stop.stop_requested();
    auto event = feed_queue_.try_pop();
    if (event) {
      if (!Consume(std::move(*event), metrics, control)) return;
    } else if (draining) {
      return;
    }
    if (control.Failed()) return;
    if (!stop.stop_requested() && Clock::now() >= next_due) {
      if (!Snapshot(metrics, control)) return;
      if (!stop.stop_requested())
        next_due = detail::CheckedDeadline(Clock::now(), period, "snapshot_period_ms");
    }
    if (!event) std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}

bool BookThread::Consume(qlib::feed_event&& event, Metrics& metrics,
                          const RunControl& control) {
  if (control.Failed()) return false;
  if (events_ == count_max) throw std::overflow_error("Book 事件序号超出 uint64 范围");
  const bool sample = events_ % 1024 == 0;
  const auto begin = sample ? Clock::now() : Clock::time_point{};
  const auto result = Apply(event);
  const auto end = sample ? Clock::now() : Clock::time_point{};
  ++events_;
  metrics.book_events.Add();
  if (sample) {
    metrics.book_apply_ns.Add(CheckedElapsed(begin, end));
    metrics.book_samples.Add();
  }

  if (result.reason == IgnoreReason::SideMismatch || result.reason == IgnoreReason::QuantityOverflow) {
    const auto index = result.reason == IgnoreReason::SideMismatch ? 0 : 1;
    auto& count = diagnostic_counts_[index];
    if (count != count_max) {
      ++count;
      if ((count & (count - 1)) == 0) {
        std::visit([&](const auto& value) {
          spdlog::warn("Book 忽略事件：原因={}，累计={}，品种={}，seq={}",
              index == 0 ? "改单方向冲突" : "价位数量溢出", count, value.instrument_id, value.seq);
        }, event);
      }
    }
  }
  auto record = std::visit([](const auto& value) -> qlib::record { return value; }, event);
  return Forward(std::move(record), control);
}

ApplyResult BookThread::Apply(const qlib::feed_event& event) {
  const auto id = std::visit([](const auto& value) { return value.instrument_id; }, event);
  if (books_.find(id) == books_.end()) {
    if (!CanGrowBooks(books_.capacity()))
      throw std::length_error("Book 品种表增长超出可表示范围");
    books_.try_emplace(std::uint32_t{id}, id, mode_);
  }
  // 插入可能搬移所有订单簿，每次重新查找后只在当前 Apply 内借用。
  auto& book = books_.find(id)->second;
  return std::visit([&](const auto& value) -> ApplyResult {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, qlib::order>) return book.OnOrder(value);
    else if constexpr (std::is_same_v<T, qlib::trade>) return book.OnTrade(value);
    else if constexpr (std::is_same_v<T, qlib::level>) return book.OnLevel(value);
    else return book.OnCancel(value);
  }, event);
}

bool BookThread::Forward(qlib::record&& record, const RunControl& control) {
  for (;;) {
    if (control.Failed()) return false;
    if (record_queue_.try_push(std::move(record))) return true;
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
}

bool BookThread::Snapshot(Metrics& metrics, const RunControl& control) {
  if (control.Failed()) return false;
  std::uint64_t instruments = 0;
  std::uint64_t resting_orders = 0;
  std::uint64_t memory_bytes = 0;
  for (const auto& [id, book] : books_) {
    if (control.Failed()) return false;
    auto snapshot = book.OnSnapshot();
    instruments = AddCount(instruments, 1);
    resting_orders = AddCount(resting_orders, CheckedCount(book.size()));
    memory_bytes = SaturatingAdd(memory_bytes, MemoryCount(book.MemoryBytes()));
    qlib::record record{snapshot};
    if (!Forward(std::move(record), control)) return false;
  }
  metrics.book_instruments.Set(instruments);
  metrics.book_resting_orders.Set(resting_orders);
  metrics.book_memory_bytes.Set(memory_bytes);
  return true;
}

}  // namespace qbook
