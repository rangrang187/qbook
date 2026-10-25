#include "Threads/WriterThread.h"

#include "ParquetWriter.h"
#include "Runtime.h"
#include "Threads/MetricsThread.h"
#include "../detail/RunDirectory.h"
#include "../detail/WriterHooks.h"

#include <spdlog/spdlog.h>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <utility>

namespace qbook {
namespace {
constexpr auto kIdlePause = std::chrono::microseconds(100);
constexpr std::size_t kDirectoryAttempts = 128;
}

WriterThread::WriterThread(RecordQueue& input, std::filesystem::path out_root,
                           std::int64_t batch_rows)
    : record_queue_(input), out_root_(std::move(out_root)), batch_rows_(batch_rows) {
  if (out_root_.empty() || batch_rows_ <= 0)
    throw std::invalid_argument("Writer 输出路径须非空，batch_rows 须为正");
}

WriterThread::WriterThread(RecordQueue& input, std::filesystem::path out_root,
                           std::int64_t batch_rows, const detail::WriterHooks& hooks)
    : WriterThread(input, std::move(out_root), batch_rows) { hooks_ = &hooks; }

WriterThread::~WriterThread() noexcept = default;

void WriterThread::Open() {
  const auto tick = std::chrono::system_clock::now().time_since_epoch().count();
  const auto seed = std::to_string(tick) + "-" + std::to_string(::getpid());
  run_dir_ = detail::ClaimRunDirectory(out_root_, seed, kDirectoryAttempts);
  if (hooks_ && hooks_->output_factory)
    writer_.reset(new ParquetWriter(run_dir_, batch_rows_, hooks_->output_factory));
  else
    writer_ = std::make_unique<ParquetWriter>(run_dir_, batch_rows_);
  spdlog::info("writer_opened 输出资源已初始化：{}", run_dir_.string());
}

void WriterThread::Consume(const qlib::record& record, Metrics& metrics) {
  std::visit(qlib::overloaded{
      [&](const qlib::order& value) { writer_->Add(value); metrics.writer_orders.Add(); },
      [&](const qlib::cancel& value) { writer_->Add(value); metrics.writer_orders.Add(); },
      [&](const qlib::trade& value) { writer_->Add(value); metrics.writer_trades.Add(); },
      [&](const qlib::level& value) { writer_->Add(value); metrics.writer_levels.Add(); },
      [&](const qlib::book& value) { writer_->Add(value); metrics.writer_books.Add(); }}, record);
  if (hooks_ && hooks_->after_add) hooks_->after_add(record);
}

void WriterThread::Close() { if (writer_) writer_->Close(); }

void WriterThread::Run(Metrics& metrics, std::stop_token stop, const RunControl& control) {
  if (started_) throw std::logic_error("Writer 实例不能重复运行");
  started_ = true;
  struct Release {
    std::unique_ptr<ParquetWriter>& writer;
    ~Release() noexcept { writer.reset(); }
  } release{writer_};
  if (control.Failed()) return;
  Open();
  for (;;) {
    if (control.Failed()) return;
    // stop 来自上游 join 之后；在观察停止后再出队，才可确认最后的发布已排空。
    const bool draining = stop.stop_requested();
    auto record = record_queue_.try_pop();
    if (record) {
      if (control.Failed()) return;
      Consume(*record, metrics);
    } else if (draining) {
      if (control.Failed()) return;
      Close();
      spdlog::info("Writer 已排空并正常关闭：{}", run_dir_.string());
      return;
    } else {
      std::this_thread::sleep_for(kIdlePause);
    }
  }
}
}  // namespace qbook
