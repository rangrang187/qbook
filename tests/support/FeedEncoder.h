#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <variant>

#include "PipelineTypes.h"

namespace qbook::testing {

struct EncodedFrame {
  std::array<std::byte, 81> storage{};
  std::size_t size = 0;

  // 借用当前对象的有效帧字节；对象须活到发送完成。
  std::span<const std::byte> bytes() const noexcept { return {storage.data(), size}; }
};

static_assert(1 + sizeof(qlib::order) == 81);
static_assert(std::is_nothrow_copy_constructible_v<qlib::feed_event>);

namespace encoder_detail {
template <class Scalar>
void WriteScalar(EncodedFrame& frame, std::size_t offset, Scalar value) noexcept {
  static_assert(std::is_integral_v<Scalar>);
  std::memcpy(frame.storage.data() + 1 + offset, &value, sizeof(value));
}
}  // namespace encoder_detail

// 按命名标量编码测试事件，输入须持有有效 variant 候选类型。
// 字段值可以非法；指针和 padding 区域保持零，recv_ns 保留输入值。
inline EncodedFrame EncodeFeedEvent(const qlib::feed_event& event) noexcept {
  EncodedFrame frame{};
  std::visit([&](const auto& value) noexcept {
    using T = std::remove_cvref_t<decltype(value)>;
    frame.size = 1 + sizeof(T);
#define QBOOK_ENCODE_FIELD(field) \
    encoder_detail::WriteScalar(frame, offsetof(T, field), value.field)
    QBOOK_ENCODE_FIELD(seq);
    QBOOK_ENCODE_FIELD(instrument_id);
    QBOOK_ENCODE_FIELD(event_ns);
    QBOOK_ENCODE_FIELD(recv_ns);
    encoder_detail::WriteScalar(frame, offsetof(T, side), static_cast<std::uint8_t>(value.side));
    if constexpr (std::is_same_v<T, qlib::order>) {
      frame.storage[0] = std::byte{qlib::order_tag};
      QBOOK_ENCODE_FIELD(order_id);
      QBOOK_ENCODE_FIELD(price);
      QBOOK_ENCODE_FIELD(qty);
      QBOOK_ENCODE_FIELD(new_qty);
      encoder_detail::WriteScalar(frame, offsetof(T, type), static_cast<std::uint8_t>(value.type));
      encoder_detail::WriteScalar(frame, offsetof(T, action), static_cast<std::uint8_t>(value.action));
    } else if constexpr (std::is_same_v<T, qlib::trade>) {
      frame.storage[0] = std::byte{qlib::trade_tag};
      QBOOK_ENCODE_FIELD(buy_order_id);
      QBOOK_ENCODE_FIELD(sell_order_id);
      QBOOK_ENCODE_FIELD(price);
      QBOOK_ENCODE_FIELD(qty);
    } else if constexpr (std::is_same_v<T, qlib::level>) {
      frame.storage[0] = std::byte{qlib::level_tag};
      QBOOK_ENCODE_FIELD(price);
      QBOOK_ENCODE_FIELD(qty);
    } else {
      static_assert(std::is_same_v<T, qlib::cancel>);
      frame.storage[0] = std::byte{qlib::cancel_tag};
      QBOOK_ENCODE_FIELD(order_id);
      QBOOK_ENCODE_FIELD(qty);
    }
#undef QBOOK_ENCODE_FIELD
  }, event);
  return frame;
}

}  // namespace qbook::testing
