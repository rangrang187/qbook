#include "support/FeedEncoder.h"
#include "support/MetricsFixtures.h"

#include <zmq.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

using namespace std::chrono_literals;

namespace {
std::vector<qlib::feed_event> Phase(int phase) {
  auto order = [](std::int64_t id, qlib::side side, std::int64_t price,
                  std::int64_t qty, std::int64_t time) {
    qlib::order v{};
    v.seq = 17; v.order_id = id; v.price = price; v.qty = qty;
    v.instrument_id = 1; v.side = side; v.type = qlib::order_type::limit;
    v.action = qlib::order_action::add; v.event_ns = time; v.recv_ns = -777;
    return v;
  };
  if (phase == 2) return {order(4, qlib::side::buy, 99, 4, 107)};
  auto modify = order(1, qlib::side::buy, 100, 777, 103);
  modify.action = qlib::order_action::modify;
  modify.new_qty = 6;
  return {order(1, qlib::side::buy, 100, 10, 101),
          order(2, qlib::side::buy, 100, 5, 102), modify,
          qlib::cancel{29, 2, 2, 1, qlib::side::sell, 104, -777},
          order(3, qlib::side::sell, 101, 8, 105),
          qlib::trade{23, 1, 3, 888, 2, 106, 1, qlib::side::sell, -777}};
}

void Send(zmq::socket_t& socket, const void* data, std::size_t size) {
  const auto sent = socket.send(zmq::const_buffer(data, size), zmq::send_flags::dontwait);
  if (!sent || *sent != size) throw std::runtime_error("finite publisher send failed");
}
}

// 测试专用有限命令协议；socket、stdin 和周期均由同一线程处理。
int main(int argc, char** argv) {
  try {
    if (argc != 3) throw std::invalid_argument("usage: fixture_publisher feed|metrics PERIOD_MS");
    const std::string mode{argv[1]};
    if (mode != "feed" && mode != "metrics") throw std::invalid_argument("invalid mode");
    const auto period = std::chrono::milliseconds{std::stoi(argv[2])};
    if (period < 1ms || period > 10s) throw std::invalid_argument("invalid period");
    zmq::context_t context{1};
    zmq::socket_t pub{context, zmq::socket_type::xpub};
    pub.set(zmq::sockopt::linger, 0);
    pub.set(zmq::sockopt::xpub_verbose, 1);
    pub.set(zmq::sockopt::xpub_verboser, 1);
    pub.bind("tcp://127.0.0.1:*");
    std::cout << "{\"event\":\"ready\",\"endpoint\":\""
              << pub.get(zmq::sockopt::last_endpoint) << "\"}" << std::endl;
    std::string pending, fixture = "large-a";
    bool running = true, paused = true, phase1 = false, phase2 = false;
    std::size_t subscriptions = 0, commands = 0, sent = 0, sent_bytes = 0;
    auto next = std::chrono::steady_clock::now() + period;
    const auto deadline = std::chrono::steady_clock::now() + 180s;
    while (running) {
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("publisher scenario deadline exceeded");
      std::array<zmq::pollitem_t, 2> items{{{pub.handle(), 0, ZMQ_POLLIN, 0},
                                         {nullptr, STDIN_FILENO, ZMQ_POLLIN, 0}}};
      zmq::poll(items, 10ms);
      if (items[0].revents & ZMQ_POLLIN) {
        zmq::message_t notification;
        const auto received = pub.recv(notification, zmq::recv_flags::dontwait);
        if (received && notification.size() == 1) {
          const bool added = *notification.data<unsigned char>() != 0;
          if (added) ++subscriptions;
          else if (subscriptions) --subscriptions;
          std::cout << "{\"event\":\"subscription\",\"active\":" << subscriptions
                    << ",\"added\":" << (added ? "true" : "false") << "}" << std::endl;
        }
      }
      if (items[1].revents & ZMQ_POLLIN) {
        char buffer[1024];
        const auto n = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR) continue; throw std::runtime_error("stdin read failed"); }
        pending.append(buffer, static_cast<std::size_t>(n));
        if (pending.size() > 4096) throw std::length_error("command buffer exceeded");
        for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
          const auto command = pending.substr(0, end);
          pending.erase(0, end + 1);
          if (++commands > 2000) throw std::length_error("finite command limit exceeded");
          if (command == "quit") running = false;
          else if (mode == "metrics" && command == "pause") paused = true;
          else if (mode == "metrics" && command == "resume") {
            paused = false;
            next = std::chrono::steady_clock::now();
          } else if (mode == "metrics" && command.starts_with("use ")) {
            fixture = command.substr(4);
            static_cast<void>(qbook::testing::MetricsFixture(fixture));
          } else if (mode == "metrics" && command == "malformed") {
            const std::array<char, 3> bad{'b', 'a', 'd'};
            Send(pub, bad.data(), bad.size());
          } else if (mode == "feed" && (command == "phase1" || command == "phase2")) {
            if (!subscriptions) throw std::runtime_error("send requires real XPUB subscription");
            const bool first = command == "phase1";
            if ((first && phase1) || (!first && (phase2 || !phase1)))
              throw std::runtime_error("phase order or duplicate command");
            for (const auto& event : Phase(first ? 1 : 2)) {
              const auto frame = qbook::testing::EncodeFeedEvent(event);
              Send(pub, frame.bytes().data(), frame.bytes().size());
              ++sent;
              sent_bytes += frame.bytes().size();
            }
            if (first) phase1 = true; else phase2 = true;
          } else throw std::invalid_argument("unsupported finite command");
          std::cout << "{\"event\":\"ack\",\"command\":\"" << command
                    << "\",\"sent\":" << sent << ",\"bytes\":" << sent_bytes << "}" << std::endl;
        }
      }
      const auto now = std::chrono::steady_clock::now();
      if (mode == "metrics" && !paused && now >= next) {
        const auto sample = qbook::testing::MetricsFixture(fixture);
        Send(pub, &sample, sizeof(sample));
        ++sent;
        next = now + period;
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
