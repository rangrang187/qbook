#pragma once

#include "PipelineTypes.h"

#include <span>
#include <stop_token>
#include <string>

namespace zmq { class socket_t; class message_t; }

namespace qbook {
struct Metrics;
class RunControl;

// 唯一生产 FeedQueue；网络资源在调用 Run 的线程创建并销毁。
class FeedThread {
public:
  // 保存已校验的端点和模式；队列须存活到 Run 返回。
  FeedThread(std::string endpoint, BookMode mode, FeedQueue& output);
  FeedThread(const FeedThread&) = delete;
  FeedThread& operator=(const FeedThread&) = delete;

  // 每个实例仅运行一次；普通停止可放弃尚未入队的事件。
  // 网络、时间或指标异常向外传播，由线程入口调用 RunControl::Fail。
  void Run(Metrics& metrics, std::stop_token stop, const RunControl& control);

private:
  void Connect(zmq::socket_t& sub);
  bool Receive(zmq::socket_t& sub, zmq::message_t& message);
  void Consume(std::span<const std::byte> bytes, std::int64_t recv_ns,
               Metrics& metrics, std::stop_token stop, const RunControl& control);
  bool Enqueue(qlib::feed_event&& event, std::stop_token stop, const RunControl& control);

  std::string endpoint_;
  BookMode mode_;
  FeedQueue& feed_queue_;
};
}  // namespace qbook
