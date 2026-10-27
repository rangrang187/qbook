#include "Threads/MetricsThread.h"

#include <limits>
#include <stdexcept>

namespace qbook {

void Metric::Add(std::uint64_t n) {
  const auto old = value.load(std::memory_order_relaxed);
  if (n > std::numeric_limits<std::uint64_t>::max() - old) {
    throw std::overflow_error("指标累计值超出 uint64 范围");
  }
  value.store(old + n, std::memory_order_relaxed);
}

void Metric::Set(std::uint64_t n) noexcept {
  value.store(n, std::memory_order_relaxed);
}

std::uint64_t Metric::Read() const noexcept {
  return value.load(std::memory_order_relaxed);
}

qlib::metrics SnapshotMetrics(const Metrics& source, std::int64_t sample_ns) noexcept {
  return {
      .ts_ns = sample_ns,
      .feed_messages = source.feed_messages.Read(),
      .feed_bytes = source.feed_bytes.Read(),
      .feed_orders = source.feed_orders.Read(),
      .feed_trades = source.feed_trades.Read(),
      .feed_levels = source.feed_levels.Read(),
      .feed_dropped = source.feed_dropped.Read(),
      .book_events = source.book_events.Read(),
      .book_apply_ns = source.book_apply_ns.Read(),
      .book_samples = source.book_samples.Read(),
      .book_instruments = source.book_instruments.Read(),
      .book_resting_orders = source.book_resting_orders.Read(),
      .book_memory_bytes = source.book_memory_bytes.Read(),
      .writer_orders = source.writer_orders.Read(),
      .writer_trades = source.writer_trades.Read(),
      .writer_levels = source.writer_levels.Read(),
      .writer_books = source.writer_books.Read(),
  };
}

}  // namespace qbook
