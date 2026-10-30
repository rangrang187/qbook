#pragma once

#include <functional>

namespace qbook {
struct Config;
struct Metrics;

namespace detail {
struct WriterHooks;

enum class ServicePhase { StopObserved, FeedJoined, BookJoined, WriterJoined, MetricsJoined };

// 测试在调用前冻结回调与借用对象，全部工作线程结束后才可销毁。
struct ServiceHooks {
  std::function<void(const char*)> before_start;
  const WriterHooks* writer_hooks = nullptr;
  std::function<void(const Metrics&)> coordinator_poll;
  // 仅协调线程的固定生命周期边界；普通服务为空，不进入逐事件路径。
  void (*on_lifecycle)(void*, ServicePhase) noexcept = nullptr;
  void* lifecycle_context = nullptr;
};

// 运行真实四阶段；可选回调只控制测试的启动边界和正常停止时机。
int RunServiceImpl(const Config& config, const ServiceHooks* hooks = nullptr);
}  // namespace detail
}  // namespace qbook
