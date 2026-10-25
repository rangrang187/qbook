#pragma once

#include "Threads/WriterThread.h"
#include "detail/WriterHooks.h"

namespace qbook::testing {
// 仅在线程 join 后读取内部状态；构造注入不改变生产接口。
struct WriterThreadInspector {
  static std::unique_ptr<WriterThread> Create(RecordQueue& input,
      std::filesystem::path root, std::int64_t batch, const detail::WriterHooks& hooks) {
    return std::unique_ptr<WriterThread>(new WriterThread(input, std::move(root), batch, hooks));
  }
  static const std::filesystem::path& Directory(const WriterThread& writer) {
    return writer.run_dir_;
  }
  static bool Released(const WriterThread& writer) { return !writer.writer_; }
};
}  // namespace qbook::testing
