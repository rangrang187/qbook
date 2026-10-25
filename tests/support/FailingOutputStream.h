#pragma once

#include "detail/OutputStreams.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace qbook { class RunControl; }
namespace qbook::testing {

enum class StreamOperation { Open, Write, Close, Abort, Destroy };
struct StreamCall {
  std::string file;
  StreamOperation operation;
  std::thread::id thread;
};

// 轨迹仅由流所属线程修改，并在 join 后读取；控制位允许协调者访问。
struct StreamProbe {
  StreamProbe() { calls.reserve(8192); }
  std::vector<StreamCall> calls;
  std::atomic<bool> armed{false}, entered{false}, release{false}, cancel{false};
  std::size_t failures = 0;
  std::size_t writes_after_abort = 0;
  bool overflow = false;
};

struct StreamFaultPlan {
  std::string file = "orders.parquet";
  bool fail_open = false;
  bool fail_write = false;
  bool fail_close = false;
  bool fail_abort = false;
  bool pause_write = false;
  std::size_t partial_write_bytes = 0;
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  const RunControl* control = nullptr;
};

// 包装真实排他文件流；下一次选中文件 Write 在 armed 后触发一次计划。
detail::OutputStreamFactory MakeOutputFactory(std::shared_ptr<StreamProbe> probe,
                                             StreamFaultPlan plan = {});

}  // namespace qbook::testing
