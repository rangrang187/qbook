#include "Runtime.h"

#include "Config.h"
#include "PipelineTypes.h"
#include "Threads/BookThread.h"
#include "Threads/FeedThread.h"
#include "Threads/MetricsThread.h"
#include "Threads/WriterThread.h"
#include "detail/ServiceInternal.h"

#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <exception>
#include <memory>
#include <optional>
#include <pthread.h>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace qbook {
namespace {
using namespace std::chrono_literals;
constexpr auto kCoordinatorPause = 10ms;
volatile std::sig_atomic_t stop_requested = 0;

void HandleStopSignal(int) noexcept { stop_requested = 1; }

void CheckMask(int result, const char* operation) {
  if (result != 0) throw std::system_error(result, std::generic_category(), operation);
}

// 新线程继承停止信号屏蔽；仅协调线程解除屏蔽并访问 stop_requested。
class SignalScope {
public:
  SignalScope() {
    if (::sigemptyset(&stops_) < 0 || ::sigaddset(&stops_, SIGINT) < 0 ||
        ::sigaddset(&stops_, SIGTERM) < 0)
      throw std::system_error(errno, std::generic_category(), "建立停止信号集合失败");
    CheckMask(::pthread_sigmask(SIG_BLOCK, &stops_, &original_mask_), "屏蔽停止信号失败");
    mask_saved_ = true;
    stop_requested = 0;
    struct sigaction action{};
    action.sa_handler = HandleStopSignal;
    action.sa_mask = stops_;
    try {
      if (::sigaction(SIGINT, &action, &original_int_) < 0)
        throw std::system_error(errno, std::generic_category(), "安装 SIGINT 处理器失败");
      int_installed_ = true;
      if (::sigaction(SIGTERM, &action, &original_term_) < 0)
        throw std::system_error(errno, std::generic_category(), "安装 SIGTERM 处理器失败");
      term_installed_ = true;
    } catch (...) {
      RestoreNoThrow();
      throw;
    }
  }
  SignalScope(const SignalScope&) = delete;
  SignalScope& operator=(const SignalScope&) = delete;
  ~SignalScope() noexcept { RestoreNoThrow(); }

  void EnableCoordinator() {
    CheckMask(::pthread_sigmask(SIG_UNBLOCK, &stops_, nullptr), "启用协调线程停止信号失败");
  }

  void Restore() {
    // 自有处理器仍安装时先恢复原 mask，已挂起且原先允许的请求由本处理器接收。
    if (mask_saved_) {
      CheckMask(::pthread_sigmask(SIG_SETMASK, &original_mask_, nullptr), "恢复原信号掩码失败");
      mask_saved_ = false;
    }
    std::exception_ptr first;
    const auto restore_action = [&](int signal, const struct sigaction& action, bool& installed) {
      if (!installed) return;
      if (::sigaction(signal, &action, nullptr) == 0) {
        installed = false;
      } else if (!first) {
        try { throw std::system_error(errno, std::generic_category(), "恢复原信号处理器失败"); }
        catch (...) { first = std::current_exception(); }
      }
    };
    restore_action(SIGINT, original_int_, int_installed_);
    restore_action(SIGTERM, original_term_, term_installed_);
    if (first) std::rethrow_exception(first);
  }

private:
  void RestoreNoThrow() noexcept {
    try { Restore(); } catch (...) {}
  }
  sigset_t stops_{};
  sigset_t original_mask_{};
  struct sigaction original_int_{};
  struct sigaction original_term_{};
  bool mask_saved_ = false;
  bool int_installed_ = false;
  bool term_installed_ = false;
};

struct Stage {
  std::jthread thread;
  std::atomic<bool> done{false};
};

void RequestAll(const std::array<Stage*, 4>& stages) noexcept {
  for (auto* stage : stages)
    if (stage->thread.joinable()) stage->thread.request_stop();
}

void StopAndJoin(Stage& target, const std::array<Stage*, 4>& stages,
                 const RunControl& control) {
  if (!target.thread.joinable()) return;
  target.thread.request_stop();
  while (!target.done.load(std::memory_order_acquire)) {
    if (control.Failed()) RequestAll(stages);
    std::this_thread::sleep_for(kCoordinatorPause);
  }
  target.thread.join();
}

// 第一条线程创建前建立；异常展开时先发布失败，再结束所有借用者。
class StageCleanup {
public:
  StageCleanup(RunControl& control, std::array<Stage*, 4> stages)
      : control_(control), stages_(stages) {}
  ~StageCleanup() noexcept {
    if (!armed_) return;
    if (!control_.Failed()) {
      try { throw std::runtime_error("服务协调未完成，取消已启动阶段"); }
      catch (...) { control_.Fail("service", std::current_exception()); }
    }
    RequestAll(stages_);
    // 句柄仅由本线程拥有，且从不 join 当前线程。
    for (auto* stage : stages_)
      if (stage->thread.joinable()) stage->thread.join();
  }
  void Dismiss() noexcept { armed_ = false; }
private:
  RunControl& control_;
  std::array<Stage*, 4> stages_;
  bool armed_ = true;
};

template<class Function>
void StartStage(Stage& stage, const char* name, RunControl& control, Function run,
                const detail::ServiceHooks* hooks) {
  if (control.Failed()) return;
  if (hooks && hooks->before_start) hooks->before_start(name);
  stage.thread = std::jthread([&stage, name, &control, run](std::stop_token stop) {
    try {
      run(stop);
      if (!stop.stop_requested() && !control.Failed())
        throw std::runtime_error("阶段在停止请求前提前结束");
    } catch (...) {
      control.Fail(name, std::current_exception());
    }
    stage.done.store(true, std::memory_order_release);
  });
}

void ReportFailure(const RunControl& control) noexcept {
  // 调用方已结束全部 Fail 写者；此处才读取普通诊断载荷。
  try {
    if (control.Error()) std::rethrow_exception(control.Error());
    spdlog::error("service_failed stage={} 未提供异常", control.Stage());
  } catch (const std::exception& error) {
    try { spdlog::error("service_failed stage={} 原因={}", control.Stage(), error.what()); }
    catch (...) {}
  } catch (...) {
    try { spdlog::error("service_failed stage={} 未知异常", control.Stage()); }
    catch (...) {}
  }
}
}  // namespace

int detail::RunServiceImpl(const Config& config, const ServiceHooks* hooks) {
  RunControl control;
  std::optional<SignalScope> signals;
  try {
    signals.emplace();
    Metrics metrics;
    FeedQueue feed_queue(config.feed_queue_capacity);
    RecordQueue record_queue(config.record_queue_capacity);
    const auto mode = config.l2 ? BookMode::L2 : BookMode::Mbo;
    FeedThread feed(config.feed_endpoint, mode, feed_queue);
    BookThread book(feed_queue, record_queue, mode, config.snapshot_period);
    std::unique_ptr<WriterThread> writer;
    if (hooks && hooks->writer_hooks) {
      writer.reset(new WriterThread(record_queue, config.out_dir, config.batch_rows,
                                     *hooks->writer_hooks));
    } else {
      writer = std::make_unique<WriterThread>(record_queue, config.out_dir, config.batch_rows);
    }
    MetricsThread monitor(config.metrics_endpoint, config.sample_period);
    Stage metrics_stage, writer_stage, book_stage, feed_stage;
    const std::array stages{&feed_stage, &book_stage, &writer_stage, &metrics_stage};
    StageCleanup cleanup(control, stages);
    try {
      spdlog::info("service_start mode={} feed={} feed_queue={}/{} record_queue={}/{} "
                   "snapshot_ms={} out={} batch_rows={} metrics_endpoint={} sample_ms={} "
                   "metrics_publish=enabled",
                   config.l2 ? "L2" : "Mbo", config.feed_endpoint,
                   config.feed_queue_capacity, feed_queue.capacity(),
                   config.record_queue_capacity, record_queue.capacity(),
                   config.snapshot_period.count(), config.out_dir.string(), config.batch_rows,
                   config.metrics_endpoint, config.sample_period.count());
      StartStage(metrics_stage, "metrics", control,
          [&](std::stop_token stop) { monitor.Run(metrics, stop, control); }, hooks);
      StartStage(writer_stage, "writer", control,
          [&](std::stop_token stop) { writer->Run(metrics, stop, control); }, hooks);
      StartStage(book_stage, "book", control,
          [&](std::stop_token stop) { book.Run(metrics, stop, control); }, hooks);
      StartStage(feed_stage, "feed", control,
          [&](std::stop_token stop) { feed.Run(metrics, stop, control); }, hooks);
      if (!control.Failed()) {
        signals->EnableCoordinator();
        spdlog::info("service_control_ready 停止协调已启用");
        while (!control.Failed() && stop_requested == 0) {
          if (hooks && hooks->coordinator_poll) hooks->coordinator_poll(metrics);
          if (!control.Failed() && stop_requested == 0)
            std::this_thread::sleep_for(kCoordinatorPause);
        }
      }
    } catch (...) {
      control.Fail("service", std::current_exception());
    }
    if (control.Failed()) RequestAll(stages);
    const auto observe = [&](ServicePhase phase) noexcept {
      if (hooks && hooks->on_lifecycle) hooks->on_lifecycle(hooks->lifecycle_context, phase);
    };
    observe(ServicePhase::StopObserved);
    const auto join_and_observe = [&](Stage& stage, ServicePhase phase) {
      const bool started = stage.thread.joinable();
      StopAndJoin(stage, stages, control);
      if (started) observe(phase);
    };
    // 下游只在上游 join 后收到普通停止；等待期间持续观察任何新故障。
    join_and_observe(feed_stage, ServicePhase::FeedJoined);
    join_and_observe(book_stage, ServicePhase::BookJoined);
    join_and_observe(writer_stage, ServicePhase::WriterJoined);
    join_and_observe(metrics_stage, ServicePhase::MetricsJoined);
    cleanup.Dismiss();
  } catch (...) {
    control.Fail("service", std::current_exception());
  }
  if (signals) {
    try { signals->Restore(); }
    catch (...) { control.Fail("signals", std::current_exception()); }
  }
  if (control.Failed()) {
    ReportFailure(control);
    return 1;
  }
  try {
    spdlog::info("service_stopped 四阶段已结束，输出正常关闭");
    return 0;
  } catch (...) {
    return 1;
  }
}

int RunService(const Config& config) { return detail::RunServiceImpl(config, nullptr); }
}  // namespace qbook
