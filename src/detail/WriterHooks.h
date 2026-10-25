#pragma once

#include "OutputStreams.h"
#include <qlib/common.h>

namespace qbook::detail {
// 私有装配缝隙：生产使用空指针；测试可包装真实流并观察完成追加的记录。
// hooks 及其捕获对象必须存活到 Writer 线程 join，回调仅在该线程调用。
struct WriterHooks {
  OutputStreamFactory output_factory;
  std::function<void(const qlib::record&)> after_add;
};
}  // namespace qbook::detail
