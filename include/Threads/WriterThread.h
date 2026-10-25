#pragma once

#include "PipelineTypes.h"

#include <filesystem>
#include <memory>
#include <stop_token>

namespace qbook {
class ParquetWriter;
class RunControl;
struct Metrics;
struct Config;
namespace testing { struct WriterThreadInspector; }
namespace detail {
struct WriterHooks;
struct ServiceHooks;
int RunServiceImpl(const Config&, const ServiceHooks*);
}

// RecordQueue 的唯一消费者；文件始终由调用 Run 的线程打开、追加和释放。
class WriterThread {
public:
  // 只保存参数；不创建目录或文件。队列须存活到 Run 线程 join。
  WriterThread(RecordQueue& input, std::filesystem::path out_root, std::int64_t batch_rows);
  ~WriterThread() noexcept;
  WriterThread(const WriterThread&) = delete;
  WriterThread& operator=(const WriterThread&) = delete;

  // 仅运行一次。上游 join 后普通停止会排空、显式关闭；Fatal 仅清理资源。
  // 异常向线程边界传播。所有返回路径均在当前线程清空 writer_。
  void Run(Metrics& metrics, std::stop_token stop, const RunControl& control);

private:
  WriterThread(RecordQueue& input, std::filesystem::path out_root,
               std::int64_t batch_rows, const detail::WriterHooks& hooks);
  void Open();
  void Consume(const qlib::record& record, Metrics& metrics);
  void Close();

  RecordQueue& record_queue_;
  std::filesystem::path out_root_, run_dir_;
  std::int64_t batch_rows_;
  std::unique_ptr<ParquetWriter> writer_;
  bool started_ = false;
  const detail::WriterHooks* hooks_ = nullptr;
  friend struct testing::WriterThreadInspector;
  friend int detail::RunServiceImpl(const Config&, const detail::ServiceHooks*);
};
}  // namespace qbook
