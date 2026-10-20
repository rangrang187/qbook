#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <variant>

#include <qlib/common.h>
#include <qlib/single_queue.h>

namespace qbook {

// 每次运行固定一种建簿模式，逐笔事件与 L2 价位更新分开处理。
enum class BookMode : std::uint8_t { Mbo, L2 };

// Feed 生产、Book 消费；所有访问结束并同步后才可销毁队列。
using FeedQueue = qlib::single_queue<qlib::feed_event>;
// Book 生产、Writer 消费；快照按值交接，与活跃订单状态独立。
using RecordQueue = qlib::single_queue<qlib::record>;

// 冻结受控协议的本机布局；网络解码仍须逐字段检查输入并重建本地链接。
static_assert(sizeof(void*) == 8, "qbook ABI 要求 8 字节指针");
static_assert(std::endian::native == std::endian::little, "qbook ABI 要求小端字节序");
static_assert(QLIB_CACHE_LINE == 64, "qbook 要求所有翻译单元统一使用 QLIB_CACHE_LINE == 64");

static_assert(std::is_same_v<std::underlying_type_t<BookMode>, std::uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<qlib::side>, std::uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<qlib::order_type>, std::uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<qlib::order_action>, std::uint8_t>);
static_assert(static_cast<std::uint8_t>(qlib::side::buy) == 0);
static_assert(static_cast<std::uint8_t>(qlib::side::sell) == 1);
static_assert(static_cast<std::uint8_t>(qlib::order_type::limit) == 0);
static_assert(static_cast<std::uint8_t>(qlib::order_type::market) == 1);
static_assert(static_cast<std::uint8_t>(qlib::order_action::add) == 0);
static_assert(static_cast<std::uint8_t>(qlib::order_action::modify) == 1);

static_assert(std::is_same_v<std::remove_cv_t<decltype(qlib::price_scale)>, std::int64_t>);
static_assert(std::is_same_v<std::remove_cv_t<decltype(qlib::qty_scale)>, std::int64_t>);
static_assert(qlib::price_scale == 10'000'000'000);
static_assert(qlib::qty_scale == 100'000'000);
static_assert(std::is_same_v<std::remove_cv_t<decltype(qlib::snapshot_depth)>, std::uint8_t>);
static_assert(qlib::snapshot_depth == 1);
static_assert(std::is_same_v<std::remove_cv_t<decltype(qlib::order_tag)>, std::uint8_t>);
static_assert(std::is_same_v<std::remove_cv_t<decltype(qlib::trade_tag)>, std::uint8_t>);
static_assert(std::is_same_v<std::remove_cv_t<decltype(qlib::level_tag)>, std::uint8_t>);
static_assert(std::is_same_v<std::remove_cv_t<decltype(qlib::cancel_tag)>, std::uint8_t>);
static_assert(qlib::order_tag == 0);
static_assert(qlib::trade_tag == 1);
static_assert(qlib::level_tag == 2);
static_assert(qlib::cancel_tag == 3);

#define QBOOK_ASSERT_RECORD(record_type, expected_size)                                    \
  static_assert(std::is_standard_layout_v<record_type>, #record_type " 必须采用标准布局");   \
  static_assert(std::is_trivially_copyable_v<record_type>, #record_type " 必须可平凡复制");   \
  static_assert(sizeof(record_type) == expected_size, #record_type " ABI 尺寸不符")

#define QBOOK_ASSERT_FIELD(record_type, field, field_type, expected_offset)                  \
  static_assert(std::is_same_v<decltype(record_type::field), field_type>,                    \
                #record_type "::" #field " 类型不符");                                      \
  static_assert(offsetof(record_type, field) == expected_offset,                            \
                #record_type "::" #field " ABI 偏移不符")

QBOOK_ASSERT_RECORD(qlib::order, 80);
QBOOK_ASSERT_FIELD(qlib::order, seq, std::int64_t, 0);
QBOOK_ASSERT_FIELD(qlib::order, order_id, std::int64_t, 8);
QBOOK_ASSERT_FIELD(qlib::order, price, std::int64_t, 16);
QBOOK_ASSERT_FIELD(qlib::order, qty, std::int64_t, 24);
QBOOK_ASSERT_FIELD(qlib::order, new_qty, std::int64_t, 32);
QBOOK_ASSERT_FIELD(qlib::order, prev, qlib::order*, 40);
QBOOK_ASSERT_FIELD(qlib::order, next, qlib::order*, 48);
QBOOK_ASSERT_FIELD(qlib::order, instrument_id, std::uint32_t, 56);
QBOOK_ASSERT_FIELD(qlib::order, side, qlib::side, 60);
QBOOK_ASSERT_FIELD(qlib::order, type, qlib::order_type, 61);
QBOOK_ASSERT_FIELD(qlib::order, action, qlib::order_action, 62);
QBOOK_ASSERT_FIELD(qlib::order, event_ns, std::int64_t, 64);
QBOOK_ASSERT_FIELD(qlib::order, recv_ns, std::int64_t, 72);

QBOOK_ASSERT_RECORD(qlib::trade, 64);
QBOOK_ASSERT_FIELD(qlib::trade, seq, std::int64_t, 0);
QBOOK_ASSERT_FIELD(qlib::trade, buy_order_id, std::int64_t, 8);
QBOOK_ASSERT_FIELD(qlib::trade, sell_order_id, std::int64_t, 16);
QBOOK_ASSERT_FIELD(qlib::trade, price, std::int64_t, 24);
QBOOK_ASSERT_FIELD(qlib::trade, qty, std::int64_t, 32);
QBOOK_ASSERT_FIELD(qlib::trade, event_ns, std::int64_t, 40);
QBOOK_ASSERT_FIELD(qlib::trade, instrument_id, std::uint32_t, 48);
QBOOK_ASSERT_FIELD(qlib::trade, side, qlib::side, 52);
QBOOK_ASSERT_FIELD(qlib::trade, recv_ns, std::int64_t, 56);

QBOOK_ASSERT_RECORD(qlib::cancel, 48);
QBOOK_ASSERT_FIELD(qlib::cancel, seq, std::int64_t, 0);
QBOOK_ASSERT_FIELD(qlib::cancel, order_id, std::int64_t, 8);
QBOOK_ASSERT_FIELD(qlib::cancel, qty, std::int64_t, 16);
QBOOK_ASSERT_FIELD(qlib::cancel, instrument_id, std::uint32_t, 24);
QBOOK_ASSERT_FIELD(qlib::cancel, side, qlib::side, 28);
QBOOK_ASSERT_FIELD(qlib::cancel, event_ns, std::int64_t, 32);
QBOOK_ASSERT_FIELD(qlib::cancel, recv_ns, std::int64_t, 40);

QBOOK_ASSERT_RECORD(qlib::level, 48);
QBOOK_ASSERT_FIELD(qlib::level, seq, std::int64_t, 0);
QBOOK_ASSERT_FIELD(qlib::level, price, std::int64_t, 8);
QBOOK_ASSERT_FIELD(qlib::level, qty, std::int64_t, 16);
QBOOK_ASSERT_FIELD(qlib::level, event_ns, std::int64_t, 24);
QBOOK_ASSERT_FIELD(qlib::level, instrument_id, std::uint32_t, 32);
QBOOK_ASSERT_FIELD(qlib::level, side, qlib::side, 36);
QBOOK_ASSERT_FIELD(qlib::level, recv_ns, std::int64_t, 40);

QBOOK_ASSERT_RECORD(qlib::book, 56);
QBOOK_ASSERT_FIELD(qlib::book, event_ns, std::int64_t, 0);
QBOOK_ASSERT_FIELD(qlib::book, bid_price, std::int64_t[1], 8);
QBOOK_ASSERT_FIELD(qlib::book, bid_qty, std::int64_t[1], 16);
QBOOK_ASSERT_FIELD(qlib::book, ask_price, std::int64_t[1], 24);
QBOOK_ASSERT_FIELD(qlib::book, ask_qty, std::int64_t[1], 32);
QBOOK_ASSERT_FIELD(qlib::book, instrument_id, std::uint32_t, 40);
QBOOK_ASSERT_FIELD(qlib::book, recv_ns, std::int64_t, 48);

QBOOK_ASSERT_RECORD(qlib::metrics, 136);
QBOOK_ASSERT_FIELD(qlib::metrics, ts_ns, std::int64_t, 0);
QBOOK_ASSERT_FIELD(qlib::metrics, feed_messages, std::uint64_t, 8);
QBOOK_ASSERT_FIELD(qlib::metrics, feed_bytes, std::uint64_t, 16);
QBOOK_ASSERT_FIELD(qlib::metrics, feed_orders, std::uint64_t, 24);
QBOOK_ASSERT_FIELD(qlib::metrics, feed_trades, std::uint64_t, 32);
QBOOK_ASSERT_FIELD(qlib::metrics, feed_levels, std::uint64_t, 40);
QBOOK_ASSERT_FIELD(qlib::metrics, feed_dropped, std::uint64_t, 48);
QBOOK_ASSERT_FIELD(qlib::metrics, book_events, std::uint64_t, 56);
QBOOK_ASSERT_FIELD(qlib::metrics, book_apply_ns, std::uint64_t, 64);
QBOOK_ASSERT_FIELD(qlib::metrics, book_samples, std::uint64_t, 72);
QBOOK_ASSERT_FIELD(qlib::metrics, book_instruments, std::uint64_t, 80);
QBOOK_ASSERT_FIELD(qlib::metrics, book_resting_orders, std::uint64_t, 88);
QBOOK_ASSERT_FIELD(qlib::metrics, book_memory_bytes, std::uint64_t, 96);
QBOOK_ASSERT_FIELD(qlib::metrics, writer_orders, std::uint64_t, 104);
QBOOK_ASSERT_FIELD(qlib::metrics, writer_trades, std::uint64_t, 112);
QBOOK_ASSERT_FIELD(qlib::metrics, writer_levels, std::uint64_t, 120);
QBOOK_ASSERT_FIELD(qlib::metrics, writer_books, std::uint64_t, 128);

#undef QBOOK_ASSERT_FIELD
#undef QBOOK_ASSERT_RECORD

static_assert(std::is_same_v<qlib::feed_event,
                             std::variant<qlib::order, qlib::trade, qlib::level, qlib::cancel>>,
              "qlib::feed_event 候选类型或顺序不符");
static_assert(std::is_same_v<qlib::record,
                             std::variant<qlib::order, qlib::trade, qlib::level, qlib::cancel, qlib::book>>,
              "qlib::record 候选类型或顺序不符");
static_assert(std::is_nothrow_move_constructible_v<qlib::feed_event>);
static_assert(std::is_nothrow_destructible_v<qlib::feed_event>);
static_assert(std::is_nothrow_move_constructible_v<qlib::record>);
static_assert(std::is_nothrow_destructible_v<qlib::record>);

}  // namespace qbook
