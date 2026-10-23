#include "FeedPublisher.h"

#include <zmq.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>

namespace qbook::testing {

FeedPublisher::FeedPublisher(MessagePlan plan, PublisherSignals& signals)
    : plan_(std::move(plan)), signals_(signals) {
  constexpr std::size_t max_messages = 100'000;
  constexpr std::size_t max_bytes = 64 * 1024 * 1024;
  if (plan_.size() > max_messages) throw std::invalid_argument("测试发布消息数量超限");
  std::size_t bytes = 0;
  for (const auto& message : plan_) {
    if (message.parts.empty() || message.parts.size() > 1024)
      throw std::invalid_argument("测试消息须含 1 至 1024 段");
    for (const auto& part : message.parts) {
      if (part.size() > max_bytes - bytes) throw std::invalid_argument("测试发布字节数量超限");
      bytes += part.size();
    }
  }
}

void FeedPublisher::Run(std::stop_token stop, const RunControl& control) {
  bool endpoint_set = false;
  bool subscribed_set = false;
  auto finish_pending = [&](std::exception_ptr error) {
    if (!endpoint_set) {
      signals_.bound_endpoint.set_exception(error);
      endpoint_set = true;
    }
    if (!subscribed_set) {
      signals_.subscribed.set_exception(error);
      subscribed_set = true;
    }
  };
  auto cancelled = [&] { return stop.stop_requested() || control.Failed(); };
  auto finish_cancel = [&] {
    finish_pending(std::make_exception_ptr(std::runtime_error("测试发布者在就绪前停止")));
  };
  try {
    if (cancelled()) {
      finish_cancel();
      return;
    }
    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::xpub);
    socket.set(zmq::sockopt::linger, 0);
    socket.set(zmq::sockopt::rcvtimeo, 100);
    socket.set(zmq::sockopt::sndtimeo, 100);
    socket.set(zmq::sockopt::sndhwm, static_cast<int>(plan_.size() + 1));
    socket.set(zmq::sockopt::xpub_nodrop, 1);
    socket.bind("tcp://127.0.0.1:*");
    signals_.bound_endpoint.set_value(socket.get(zmq::sockopt::last_endpoint));
    endpoint_set = true;
    while (!subscribed_set) {
      if (cancelled()) {
        finish_cancel();
        return;
      }
      zmq::message_t subscription;
      zmq::recv_result_t result;
      try {
        result = socket.recv(subscription, zmq::recv_flags::none);
      } catch (const zmq::error_t& error) {
        if (error.num() == EINTR) continue;
        throw;
      }
      if (result && subscription.size() == 1 &&
          *static_cast<const std::uint8_t*>(subscription.data()) == 1) {
        signals_.subscribed.set_value();
        subscribed_set = true;
      }
    }
    for (const auto& message : plan_) {
      for (std::size_t i = 0; i < message.parts.size(); ++i) {
        if (cancelled()) return;
        const auto& part = message.parts[i];
        const auto flags = i + 1 < message.parts.size() ? zmq::send_flags::sndmore
                                                      : zmq::send_flags::none;
        const auto sent = socket.send(zmq::buffer(part.data(), part.size()), flags);
        if (!sent || *sent != part.size()) throw std::runtime_error("测试发布消息发送未完成");
      }
      signals_.sent_messages.fetch_add(1);
    }
    signals_.sent_all.store(true);
    while (!cancelled()) std::this_thread::sleep_for(std::chrono::microseconds(100));
  } catch (...) {
    finish_pending(std::current_exception());
    throw;
  }
}

}  // namespace qbook::testing
