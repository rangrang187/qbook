#include <qlib/common.h>

// GCC 13 的 C++23 草案模式报告 202100L；C++20 或更早模式必须失败。
static_assert(__cplusplus > 202002L);
static_assert(QLIB_CACHE_LINE == EXPECTED_CACHE_LINE);

int main() {
  qlib::cancel value{};
  value.seq = 7;
  value.order_id = 42;
  value.qty = 2 * qlib::qty_scale;
  value.instrument_id = 1;
  value.side = qlib::side::buy;
  value.event_ns = 1'000;
  value.recv_ns = 1'100;
  const qlib::feed_event event{value};
  const bool ok = std::visit(qlib::overloaded{
    [](const qlib::order&) { return false; },
    [](const qlib::trade&) { return false; },
    [](const qlib::level&) { return false; },
    [](const qlib::cancel& c) {
      return c.seq == 7 && c.order_id == 42 && c.qty == 200'000'000 &&
             c.instrument_id == 1 && c.side == qlib::side::buy &&
             c.event_ns == 1'000 && c.recv_ns == 1'100;
    }
  }, event);
  return ok ? 0 : 1;
}
