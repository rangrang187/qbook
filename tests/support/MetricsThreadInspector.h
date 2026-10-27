#pragma once

#include "Threads/MetricsThread.h"
#include <zmq.hpp>

namespace qbook::testing {
// 只能借用当前调用线程创建并拥有的 socket，不跨线程干预 Run 的局部资源。
class MetricsThreadInspector {
public:
  static void Bind(MetricsThread& stage, zmq::socket_t& socket) { stage.Bind(socket); }
  static bool Publish(MetricsThread& stage, zmq::socket_t& socket, const Metrics& metrics) {
    return stage.Publish(socket, metrics);
  }
};
}  // namespace qbook::testing
