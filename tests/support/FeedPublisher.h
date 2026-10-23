#pragma once

#include "Runtime.h"

#include <atomic>
#include <cstddef>
#include <future>
#include <stop_token>
#include <string>
#include <vector>

namespace qbook::testing {

struct TestMessage {
  std::vector<std::vector<std::byte>> parts;
};
using MessagePlan = std::vector<TestMessage>;

struct PublisherSignals {
  std::promise<std::string> bound_endpoint;
  std::promise<void> subscribed;
  std::atomic<std::size_t> sent_messages{0};
  std::atomic<bool> sent_all{false};
};

// 在 Run 线程拥有 XPUB，订阅就绪后发送有限计划，并保持连接至停止。
class FeedPublisher {
public:
  FeedPublisher(MessagePlan plan, PublisherSignals& signals);
  FeedPublisher(const FeedPublisher&) = delete;
  FeedPublisher& operator=(const FeedPublisher&) = delete;
  void Run(std::stop_token stop, const RunControl& control);

private:
  MessagePlan plan_;
  PublisherSignals& signals_;
};

}  // namespace qbook::testing
