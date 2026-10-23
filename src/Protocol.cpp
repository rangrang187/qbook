#include "Protocol.h"

#include <cstring>
#include <type_traits>
#include <utility>
#include <variant>

namespace qbook {
namespace {

bool ValidSide(std::uint8_t value) noexcept {
  return value == static_cast<std::uint8_t>(qlib::side::buy) ||
         value == static_cast<std::uint8_t>(qlib::side::sell);
}

bool ValidType(std::uint8_t value) noexcept {
  return value == static_cast<std::uint8_t>(qlib::order_type::limit) ||
         value == static_cast<std::uint8_t>(qlib::order_type::market);
}

bool ValidAction(std::uint8_t value) noexcept {
  return value == static_cast<std::uint8_t>(qlib::order_action::add) ||
         value == static_cast<std::uint8_t>(qlib::order_action::modify);
}

// 调用方已验证完整载荷长度，偏移来自冻结布局的 offsetof。
template <class Scalar>
Scalar ReadScalar(std::span<const std::byte> payload, std::size_t offset) noexcept {
  static_assert(std::is_same_v<Scalar, std::int64_t> ||
                std::is_same_v<Scalar, std::uint32_t> ||
                std::is_same_v<Scalar, std::uint8_t>);
  Scalar value{};
  std::memcpy(&value, payload.data() + offset, sizeof(value));
  return value;
}

template <class T>
DecodeResult ReadEvent(std::span<const std::byte> payload, std::int64_t recv_ns) noexcept {
  static_assert(std::is_nothrow_constructible_v<qlib::feed_event, T>);
  static_assert(std::is_nothrow_constructible_v<std::optional<qlib::feed_event>,
                                               qlib::feed_event&&>);
  const auto raw_side = ReadScalar<std::uint8_t>(payload, offsetof(T, side));
  if (!ValidSide(raw_side)) return {std::nullopt, DecodeError::InvalidField};

  T value{};
  value.seq = ReadScalar<std::int64_t>(payload, offsetof(T, seq));
  value.instrument_id = ReadScalar<std::uint32_t>(payload, offsetof(T, instrument_id));
  value.event_ns = ReadScalar<std::int64_t>(payload, offsetof(T, event_ns));
  value.side = static_cast<qlib::side>(raw_side);
  value.recv_ns = recv_ns;
  value.qty = ReadScalar<std::int64_t>(payload, offsetof(T, qty));

  if constexpr (std::is_same_v<T, qlib::order>) {
    const auto raw_type = ReadScalar<std::uint8_t>(payload, offsetof(T, type));
    const auto raw_action = ReadScalar<std::uint8_t>(payload, offsetof(T, action));
    if (!ValidType(raw_type) || !ValidAction(raw_action))
      return {std::nullopt, DecodeError::InvalidField};
    value.order_id = ReadScalar<std::int64_t>(payload, offsetof(T, order_id));
    value.price = ReadScalar<std::int64_t>(payload, offsetof(T, price));
    value.new_qty = ReadScalar<std::int64_t>(payload, offsetof(T, new_qty));
    value.type = static_cast<qlib::order_type>(raw_type);
    value.action = static_cast<qlib::order_action>(raw_action);
    // 本地链接由订单簿建立；网络指针区和 padding 始终不读取。
    value.prev = nullptr;
    value.next = nullptr;
  } else if constexpr (std::is_same_v<T, qlib::trade>) {
    value.buy_order_id = ReadScalar<std::int64_t>(payload, offsetof(T, buy_order_id));
    value.sell_order_id = ReadScalar<std::int64_t>(payload, offsetof(T, sell_order_id));
    value.price = ReadScalar<std::int64_t>(payload, offsetof(T, price));
  } else if constexpr (std::is_same_v<T, qlib::level>) {
    value.price = ReadScalar<std::int64_t>(payload, offsetof(T, price));
  } else {
    static_assert(std::is_same_v<T, qlib::cancel>);
    value.order_id = ReadScalar<std::int64_t>(payload, offsetof(T, order_id));
  }

  qlib::feed_event event{value};
  const auto error = ValidateEvent(event);
  if (error != DecodeError::None) return {std::nullopt, error};
  return {std::move(event), DecodeError::None};
}

}  // namespace

DecodeError ValidateEvent(const qlib::feed_event& event) noexcept {
  if (event.valueless_by_exception()) return DecodeError::InvalidField;
  const bool valid = std::visit([](const auto& value) noexcept {
    using T = std::remove_cvref_t<decltype(value)>;
    if (!ValidSide(static_cast<std::uint8_t>(value.side)) ||
        value.seq < 0 || value.event_ns < 0) return false;

    if constexpr (std::is_same_v<T, qlib::order>) {
      if (value.order_id <= 0 || !ValidType(static_cast<std::uint8_t>(value.type)) ||
          !ValidAction(static_cast<std::uint8_t>(value.action))) return false;
      if (value.action == qlib::order_action::add) {
        return value.qty > 0 &&
               (value.type == qlib::order_type::market || value.price > 0);
      }
      return value.new_qty <= 0 || value.price > 0;
    } else if constexpr (std::is_same_v<T, qlib::trade>) {
      return value.qty > 0 && value.price > 0 && value.buy_order_id >= 0 &&
             value.sell_order_id >= 0 &&
             (value.buy_order_id == 0 || value.sell_order_id == 0 ||
              value.buy_order_id != value.sell_order_id);
    } else if constexpr (std::is_same_v<T, qlib::level>) {
      return value.price > 0;
    } else {
      static_assert(std::is_same_v<T, qlib::cancel>);
      return value.order_id > 0 && value.qty > 0;
    }
  }, event);
  return valid ? DecodeError::None : DecodeError::InvalidField;
}

DecodeResult DecodeFrame(std::span<const std::byte> bytes, std::int64_t recv_ns,
                         BookMode mode) noexcept {
  if (bytes.empty()) return {std::nullopt, DecodeError::EmptyFrame};
  const auto tag = std::to_integer<std::uint8_t>(bytes.front());
  std::size_t expected_size = 0;
  switch (tag) {
    case qlib::order_tag: expected_size = 1 + sizeof(qlib::order); break;
    case qlib::trade_tag: expected_size = 1 + sizeof(qlib::trade); break;
    case qlib::level_tag: expected_size = 1 + sizeof(qlib::level); break;
    case qlib::cancel_tag: expected_size = 1 + sizeof(qlib::cancel); break;
    default: return {std::nullopt, DecodeError::UnknownTag};
  }
  if (bytes.size() != expected_size) return {std::nullopt, DecodeError::WrongSize};
  if ((mode != BookMode::Mbo && mode != BookMode::L2) ||
      (mode == BookMode::Mbo && tag == qlib::level_tag) ||
      (mode == BookMode::L2 && tag != qlib::level_tag && tag != qlib::trade_tag))
    return {std::nullopt, DecodeError::ModeMismatch};

  const auto payload = bytes.subspan(1);
  switch (tag) {
    case qlib::order_tag: return ReadEvent<qlib::order>(payload, recv_ns);
    case qlib::trade_tag: return ReadEvent<qlib::trade>(payload, recv_ns);
    case qlib::level_tag: return ReadEvent<qlib::level>(payload, recv_ns);
    case qlib::cancel_tag: return ReadEvent<qlib::cancel>(payload, recv_ns);
    default: return {std::nullopt, DecodeError::UnknownTag};
  }
}

}  // namespace qbook
