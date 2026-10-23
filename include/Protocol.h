#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "PipelineTypes.h"

namespace qbook {

// 拒绝原因按空帧、标签、长度、模式、字段的顺序判定。
enum class DecodeError : std::uint8_t {
  None, EmptyFrame, UnknownTag, WrongSize, ModeMismatch, InvalidField
};

struct DecodeResult {
  std::optional<qlib::feed_event> event;
  DecodeError error = DecodeError::None;
};

// 解码完整单帧；输入在调用期间有效，返回值独立拥有事件。
// recv_ns 使用调用方本机时间，订单链接清空；返回事件存在当且仅当 error 为 None。
DecodeResult DecodeFrame(std::span<const std::byte> bytes,
                         std::int64_t recv_ns, BookMode mode) noexcept;

// 检查当前事件的字段范围；返回 None 或 InvalidField，不查询订单簿。
DecodeError ValidateEvent(const qlib::feed_event& event) noexcept;

}  // namespace qbook
