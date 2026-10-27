#pragma once

#include <qlib/common.h>

#include <bit>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// 监控传输只支持当前冻结的小端本机 ABI；不发送原子对象或 variant 布局。
static_assert(CHAR_BIT == 8 && sizeof(std::int64_t) == 8 && sizeof(std::uint64_t) == 8);
static_assert(std::endian::native == std::endian::little);
static_assert(std::is_trivially_copyable_v<qlib::metrics>);
static_assert(std::is_standard_layout_v<qlib::metrics>);
static_assert(sizeof(qlib::metrics) == 136);
static_assert(std::is_same_v<decltype(qlib::metrics::ts_ns), std::int64_t>);
static_assert(offsetof(qlib::metrics, ts_ns) == 0);
static_assert(std::is_same_v<decltype(qlib::metrics::feed_messages), std::uint64_t>);
static_assert(offsetof(qlib::metrics, feed_messages) == 8);
static_assert(std::is_same_v<decltype(qlib::metrics::feed_bytes), std::uint64_t>);
static_assert(offsetof(qlib::metrics, feed_bytes) == 16);
static_assert(std::is_same_v<decltype(qlib::metrics::feed_orders), std::uint64_t>);
static_assert(offsetof(qlib::metrics, feed_orders) == 24);
static_assert(std::is_same_v<decltype(qlib::metrics::feed_trades), std::uint64_t>);
static_assert(offsetof(qlib::metrics, feed_trades) == 32);
static_assert(std::is_same_v<decltype(qlib::metrics::feed_levels), std::uint64_t>);
static_assert(offsetof(qlib::metrics, feed_levels) == 40);
static_assert(std::is_same_v<decltype(qlib::metrics::feed_dropped), std::uint64_t>);
static_assert(offsetof(qlib::metrics, feed_dropped) == 48);
static_assert(std::is_same_v<decltype(qlib::metrics::book_events), std::uint64_t>);
static_assert(offsetof(qlib::metrics, book_events) == 56);
static_assert(std::is_same_v<decltype(qlib::metrics::book_apply_ns), std::uint64_t>);
static_assert(offsetof(qlib::metrics, book_apply_ns) == 64);
static_assert(std::is_same_v<decltype(qlib::metrics::book_samples), std::uint64_t>);
static_assert(offsetof(qlib::metrics, book_samples) == 72);
static_assert(std::is_same_v<decltype(qlib::metrics::book_instruments), std::uint64_t>);
static_assert(offsetof(qlib::metrics, book_instruments) == 80);
static_assert(std::is_same_v<decltype(qlib::metrics::book_resting_orders), std::uint64_t>);
static_assert(offsetof(qlib::metrics, book_resting_orders) == 88);
static_assert(std::is_same_v<decltype(qlib::metrics::book_memory_bytes), std::uint64_t>);
static_assert(offsetof(qlib::metrics, book_memory_bytes) == 96);
static_assert(std::is_same_v<decltype(qlib::metrics::writer_orders), std::uint64_t>);
static_assert(offsetof(qlib::metrics, writer_orders) == 104);
static_assert(std::is_same_v<decltype(qlib::metrics::writer_trades), std::uint64_t>);
static_assert(offsetof(qlib::metrics, writer_trades) == 112);
static_assert(std::is_same_v<decltype(qlib::metrics::writer_levels), std::uint64_t>);
static_assert(offsetof(qlib::metrics, writer_levels) == 120);
static_assert(std::is_same_v<decltype(qlib::metrics::writer_books), std::uint64_t>);
static_assert(offsetof(qlib::metrics, writer_books) == 128);
