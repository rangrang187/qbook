#include "support/ParquetReadback.h"

#include <arrow/api.h>

#include <array>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace {
void Expect(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
std::int64_t Integer(const arrow::Table& table, const char* name, std::int64_t row) {
  const auto values = table.GetColumnByName(name);
  if (!values) throw std::runtime_error("missing readback column");
  for (const auto& chunk : values->chunks()) {
    if (row < chunk->length()) return static_cast<const arrow::Int64Array&>(*chunk).Value(row);
    row -= chunk->length();
  }
  throw std::out_of_range("missing readback row");
}
}

// 复用独立格式读取器；业务预期在此手工固定，不调用发布端或 Orderbook 求答案。
int main(int argc, char** argv) {
  try {
    if (argc != 3) throw std::invalid_argument("usage: parquet_verify RUN_DIRECTORY phase1|phase2");
    const std::string phase{argv[2]};
    if (phase != "phase1" && phase != "phase2") throw std::invalid_argument("unknown scenario");
    const bool second = phase == "phase2";
    const auto actual = qbook::testing::ReadRunDirectory(argv[1], 100000);
    qbook::testing::ExpectedRun expected;
    // id、价格、原数量、绝对新数量、方向、action、来源时间。
    constexpr std::array<std::array<std::int64_t, 7>, 5> rows{{
        {1,100,10,0,0,0,101}, {2,100,5,0,0,0,102}, {1,100,777,6,0,1,103},
        {3,101,8,0,1,0,105}, {4,99,4,0,0,0,107}}};
    std::map<std::int64_t, std::int64_t> received;
    for (std::size_t i = 0; i < (second ? 5U : 4U); ++i) {
      const auto& r = rows[i];
      qlib::order v{};
      v.seq = 17; v.order_id = r[0]; v.price = r[1]; v.qty = r[2]; v.new_qty = r[3];
      v.side = static_cast<qlib::side>(r[4]); v.action = static_cast<qlib::order_action>(r[5]);
      v.type = qlib::order_type::limit; v.instrument_id = 1; v.event_ns = r[6];
      v.recv_ns = Integer(*actual.orders, "recv_ns", i);
      Expect(v.recv_ns > 0, "local receive time must replace sender poison");
      received[v.event_ns] = v.recv_ns;
      expected.orders.push_back(v);
    }
    qlib::cancel cancel{29, 2, 2, 1, qlib::side::sell, 104,
                        Integer(*actual.cancels, "recv_ns", 0)};
    qlib::trade trade{23, 1, 3, 888, 2, 106, 1, qlib::side::sell,
                      Integer(*actual.trades, "recv_ns", 0)};
    Expect(cancel.recv_ns > 0 && trade.recv_ns > 0, "invalid event receive time");
    expected.cancels.push_back(cancel); expected.trades.push_back(trade);
    received[104] = cancel.recv_ns; received[106] = trade.recv_ns;
    constexpr std::array<std::int64_t, 7> bids{10,15,11,9,9,7,7};
    std::int64_t last = 100;
    for (std::int64_t row = 0; row < actual.books->num_rows(); ++row) {
      const auto time = Integer(*actual.books, "event_ns", row);
      Expect(time >= 101 && time <= (second ? 107 : 106) && time >= last,
             "snapshot event time outside independent sequence");
      qlib::book value{};
      value.instrument_id = 1; value.event_ns = time; value.recv_ns = received.at(time);
      value.bid_price[0] = 100; value.bid_qty[0] = bids.at(time - 101);
      value.ask_price[0] = time >= 105 ? 101 : 0;
      value.ask_qty[0] = time >= 106 ? 6 : time == 105 ? 8 : 0;
      expected.books.push_back(value);
      last = time;
    }
    qbook::testing::CheckExpectedFiles(actual, expected);
    Expect(last == (second ? 107 : 106), "missing completed sequence snapshot after Add");
    std::cout << "{\"verified\":true,\"phase\":\"" << phase << "\",\"orders\":"
              << actual.orders->num_rows() << ",\"trades\":" << actual.trades->num_rows()
              << ",\"cancels\":" << actual.cancels->num_rows() << ",\"levels\":"
              << actual.levels->num_rows() << ",\"books\":" << actual.books->num_rows()
              << ",\"bidPrice\":100,\"bidQty\":7,\"askPrice\":101,\"askQty\":6}" << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
