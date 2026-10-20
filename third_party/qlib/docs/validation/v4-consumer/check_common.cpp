#include <qlib/common.h>

#include <variant>

bool check_common() {
  qlib::order value{};
  value.seq = 7;
  value.order_id = 42;
  value.price = 3 * qlib::price_scale;
  value.qty = 2 * qlib::qty_scale;
  value.new_qty = 0;
  value.prev = nullptr;
  value.next = nullptr;
  value.instrument_id = 1;
  value.side = qlib::side::buy;
  value.type = qlib::order_type::limit;
  value.action = qlib::order_action::add;
  value.event_ns = 1'000;
  value.recv_ns = 1'100;

  const qlib::feed_event event{value};
  const auto* stored = std::get_if<qlib::order>(&event);
  const qlib::price_level empty_level{};
  return stored != nullptr && stored->seq == 7 && stored->order_id == 42 &&
      stored->price == 30'000'000'000 && stored->qty == 200'000'000 &&
      stored->new_qty == 0 && stored->prev == nullptr && stored->next == nullptr &&
      stored->instrument_id == 1 && stored->side == qlib::side::buy &&
      stored->type == qlib::order_type::limit && stored->action == qlib::order_action::add &&
      stored->event_ns == 1'000 && stored->recv_ns == 1'100 &&
      empty_level.qty == 0 && empty_level.head == nullptr && empty_level.tail == nullptr;
}
