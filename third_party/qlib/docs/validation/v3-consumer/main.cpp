#include <qlib/single_queue.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <thread>

namespace {

// 删除默认构造和复制，验证队列只在成功入队时构造对象。
struct message {
  std::uint64_t sequence;
  std::uint64_t checksum;

  message() = delete;
  explicit message(std::uint64_t value) : sequence(value), checksum(value ^ 0x5a5a5a5aULL) {}
  message(const message&) = delete;
  message(message&&) noexcept = default;
};

void run() {
  constexpr std::uint64_t count = 10000;
  qlib::single_queue<message> queue(5);
  std::atomic<std::uint64_t> atomic_probe{0};
  std::stop_source stop;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  std::uint64_t produced = 0;
  std::uint64_t consumed = 0;
  std::exception_ptr producer_error;
  std::exception_ptr consumer_error;

  // 两侧每次尝试都检查截止和停止请求；jthread 自身的 token 处理部分启动失败。
  auto should_stop = [&](std::stop_token local_stop) {
    if (std::chrono::steady_clock::now() >= deadline) {
      stop.request_stop();
    }
    return local_stop.stop_requested() || stop.stop_requested();
  };
  std::jthread producer([&](std::stop_token local_stop) {
    try {
      while (produced < count && !should_stop(local_stop)) {
        if (queue.try_emplace(produced)) {
          ++produced;
        } else {
          std::this_thread::yield();
        }
      }
    } catch (...) {
      producer_error = std::current_exception();
      stop.request_stop();
    }
  });
  std::jthread consumer([&](std::stop_token local_stop) {
    try {
      while (consumed < count && !should_stop(local_stop)) {
        auto result = queue.try_pop();
        if (!result) {
          std::this_thread::yield();
          continue;
        }
        if (result->sequence != consumed || result->checksum != (consumed ^ 0x5a5a5a5aULL)) {
          throw std::runtime_error("队列载荷或顺序不匹配");
        }
        ++consumed;
      }
    } catch (...) {
      consumer_error = std::current_exception();
      stop.request_stop();
    }
  });

  // 主线程先 join，再读取各侧结果；queue 声明在工作线程之前，最后销毁。
  producer.join();
  consumer.join();
  if (producer_error) std::rethrow_exception(producer_error);
  if (consumer_error) std::rethrow_exception(consumer_error);
  if (stop.stop_requested() || produced != count || consumed != count || !queue.empty()) {
    throw std::runtime_error("交接未完成");
  }
  std::cout << "produced=" << produced << " consumed=" << consumed
            << " capacity=" << queue.capacity()
            << " atomic_always_lock_free=" << std::atomic<std::uint64_t>::is_always_lock_free
            << " atomic_runtime_lock_free=" << atomic_probe.is_lock_free()
            << " cache_line=" << QLIB_CACHE_LINE
            << " queue_alignment=" << alignof(decltype(queue))
            << " queue_size=" << sizeof(queue) << '\n';
}

}  // namespace

int main() {
  try {
    run();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "调用工程遇到未知异常\n";
    return 1;
  }
}
