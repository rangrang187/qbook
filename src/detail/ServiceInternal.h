#pragma once

#include <functional>

namespace qbook {
struct Config;
struct Metrics;

namespace detail {
struct WriterHooks;

// 测试在调用前冻结回调与借用对象，全部工作线程结束后才可销毁。
struct ServiceHooks {
  std::function<void(const char*)> before_start;
  const WriterHooks* writer_hooks = nullptr;
  std::function<void(const Metrics&)> coordinator_poll;
};

// 运行真实三阶段；可选回调只控制测试的启动边界和正常停止时机。
int RunServiceImpl(const Config& config, const ServiceHooks* hooks = nullptr);
}  // namespace detail
}  // namespace qbook
