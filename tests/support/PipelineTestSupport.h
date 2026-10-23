#pragma once

#include "RecordCollector.h"
#include "Runtime.h"

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <initializer_list>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

namespace qbook::testing {

inline auto ScenarioDeadline() {
  return std::chrono::steady_clock::now() + std::chrono::seconds(10);
}

template <class Predicate>
void WaitUntil(Predicate predicate, const RunControl& control,
               std::chrono::steady_clock::time_point deadline) {
  while (!predicate()) {
    if (control.Failed()) throw std::runtime_error("测试阶段失败，等待提前结束");
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("测试场景期限届满");
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}

template <class Function>
std::jthread Launch(const char* stage, RunControl& control, Function function,
                    std::atomic<bool>* done = nullptr) {
  return std::jthread([stage, &control, function = std::move(function), done]
                      (std::stop_token stop) mutable {
    try {
      function(stop);
    } catch (...) {
      control.Fail(stage, std::current_exception());
    }
    if (done) done->store(true);
  });
}

inline void StopJoin(std::jthread& thread) {
  if (!thread.joinable()) return;
  thread.request_stop();
  thread.join();
}

// 在第一条线程启动前建立；借用对象须比此守卫更早构造。
class ThreadCleanup {
public:
  ThreadCleanup(RunControl& control, std::initializer_list<std::jthread*> threads,
                CollectorControl* commands = nullptr)
      : control_(control), commands_(commands) {
    if (threads.size() > threads_.size()) throw std::invalid_argument("测试线程数量超限");
    for (auto* thread : threads) threads_[count_++] = thread;
  }
  ThreadCleanup(const ThreadCleanup&) = delete;
  ThreadCleanup& operator=(const ThreadCleanup&) = delete;
  ~ThreadCleanup() {
    if (!armed_) return;
    Abort();
  }

  void Abort() noexcept {
    if (!armed_) return;
    if (!control_.Failed()) {
      try {
        throw std::runtime_error("测试装配退出前清理未完成线程");
      } catch (...) {
        control_.Fail("test", std::current_exception());
      }
    }
    if (commands_) commands_->pause.store(false);
    for (std::size_t i = 0; i < count_; ++i)
      if (threads_[i]->joinable()) threads_[i]->request_stop();
    for (std::size_t i = 0; i < count_; ++i)
      if (threads_[i]->joinable()) threads_[i]->join();
    armed_ = false;
  }
  void Dismiss() noexcept { armed_ = false; }

private:
  RunControl& control_;
  CollectorControl* commands_;
  std::array<std::jthread*, 6> threads_{};
  std::size_t count_ = 0;
  bool armed_ = true;
};

}  // namespace qbook::testing
