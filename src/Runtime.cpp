#include "Runtime.h"

#include <utility>

namespace qbook {

void RunControl::Fail(const char* stage, std::exception_ptr error) noexcept {
  if (failed_.exchange(true, std::memory_order_relaxed)) {
    return;
  }
  // 交换选出唯一写者；调用方完成全部 join 后才能读取普通诊断字段。
  stage_ = stage;
  error_ = std::move(error);
}

bool RunControl::Failed() const noexcept {
  return failed_.load(std::memory_order_relaxed);
}

std::exception_ptr RunControl::Error() const noexcept {
  return error_;
}

const char* RunControl::Stage() const noexcept {
  return stage_;
}

}  // namespace qbook
