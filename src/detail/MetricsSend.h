#pragma once

#include <qlib/common.h>

#include <cstddef>
#include <optional>
#include <stdexcept>

namespace qbook::detail {
// cppzmq 4.10 的空 optional 唯一表示 EAGAIN；异常由调用者保留其实际错误码。
inline bool MetricsSendAccepted(std::optional<std::size_t> sent) {
  if (!sent) return false;
  if (*sent != sizeof(qlib::metrics))
    throw std::runtime_error("Metrics 发送长度不等于完整样本");
  return true;
}
}  // namespace qbook::detail
