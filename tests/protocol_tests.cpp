#include "Protocol.h"
#include "support/FeedEncoder.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

using qbook::BookMode;
using qbook::DecodeError;
using qbook::DecodeFrame;
using qbook::ValidateEvent;
using qbook::testing::EncodeFeedEvent;
constexpr std::int64_t kLocalRecv = 1'234'567'890'123;
constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
constexpr auto kMax = std::numeric_limits<std::int64_t>::max();

std::vector<std::byte> Fixture(std::string_view name) {
  std::ifstream input(std::filesystem::path(QBOOK_PROTOCOL_FIXTURE_DIR) / name,
                      std::ios::binary);
  REQUIRE(input.is_open());
  const std::vector<char> raw{std::istreambuf_iterator<char>(input), {}};
  std::vector<std::byte> bytes;
  bytes.reserve(raw.size());
  for (const auto value : raw) bytes.push_back(static_cast<std::byte>(value));
  return bytes;
}

qlib::order GoldenAdd() {
  qlib::order value{};
  value.seq = 72'623'859'790'382'856;
  value.order_id = 1'230'066'625'199'609'624;
  value.price = 10'000'000'000;
  value.qty = 100'000'000;
  value.new_qty = -9;
  value.instrument_id = 287'454'020;
  value.side = qlib::side::buy;
  value.type = qlib::order_type::limit;
  value.action = qlib::order_action::add;
  value.event_ns = 1'234'567'890;
  value.recv_ns = 9'876'543'210;
  return value;
}

qlib::order GoldenDelete() {
  auto value = GoldenAdd();
  value.seq = 9;
  value.price = -77;
  value.qty = -44;
  value.new_qty = -7;
  value.side = qlib::side::sell;
  value.type = qlib::order_type::market;
  value.action = qlib::order_action::modify;
  value.event_ns = 0;
  value.recv_ns = 321;
  return value;
}

qlib::trade GoldenTrade() {
  qlib::trade value{};
  value.seq = 10;
  value.buy_order_id = 1'230'066'625'199'609'624;
  value.sell_order_id = 2'387'509'390'608'836'392;
  value.price = 20'000'000'000;
  value.qty = 200'000'000;
  value.event_ns = 1'234'567'891;
  value.instrument_id = 1'432'778'632;
  value.side = qlib::side::sell;
  value.recv_ns = 9'876'543'211;
  return value;
}

qlib::cancel GoldenCancel() {
  qlib::cancel value{};
  value.seq = 11;
  value.order_id = 1'230'066'625'199'609'624;
  value.qty = 300'000'000;
  value.instrument_id = 287'454'020;
  value.side = qlib::side::buy;
  value.event_ns = 1'234'567'892;
  value.recv_ns = 9'876'543'212;
  return value;
}

qlib::level GoldenLevel() {
  qlib::level value{};
  value.seq = 12;
  value.price = 30'000'000'000;
  value.qty = -2;
  value.event_ns = 1'234'567'893;
  value.instrument_id = 2'864'434'397;
  value.side = qlib::side::sell;
  value.recv_ns = 9'876'543'213;
  return value;
}

struct GoldenCase {
  const char* filename;
  qlib::feed_event event;
  BookMode mode;
  std::size_t frame_size;
};

std::array<GoldenCase, 5> GoldenCases() {
  return {{{"order_add.bin", GoldenAdd(), BookMode::Mbo, 81},
           {"order_modify_delete.bin", GoldenDelete(), BookMode::Mbo, 81},
           {"trade.bin", GoldenTrade(), BookMode::Mbo, 65},
           {"cancel.bin", GoldenCancel(), BookMode::Mbo, 49},
           {"level.bin", GoldenLevel(), BookMode::L2, 49}}};
}

void CheckFields(const qlib::feed_event& actual, const qlib::feed_event& expected,
                 std::int64_t expected_recv) {
  REQUIRE(actual.index() == expected.index());
  std::visit([&](const auto& want) {
    using T = std::remove_cvref_t<decltype(want)>;
    const auto& got = std::get<T>(actual);
    CHECK(got.seq == want.seq);
    CHECK(got.instrument_id == want.instrument_id);
    CHECK(got.side == want.side);
    CHECK(got.event_ns == want.event_ns);
    CHECK(got.recv_ns == expected_recv);
    CHECK(got.qty == want.qty);
    if constexpr (!std::is_same_v<T, qlib::cancel>) CHECK(got.price == want.price);
    if constexpr (std::is_same_v<T, qlib::order>) {
      CHECK(got.order_id == want.order_id);
      CHECK(got.new_qty == want.new_qty);
      CHECK(got.type == want.type);
      CHECK(got.action == want.action);
      CHECK(got.prev == nullptr);
      CHECK(got.next == nullptr);
    } else if constexpr (std::is_same_v<T, qlib::cancel>) {
      CHECK(got.order_id == want.order_id);
    } else if constexpr (std::is_same_v<T, qlib::trade>) {
      CHECK(got.buy_order_id == want.buy_order_id);
      CHECK(got.sell_order_id == want.sell_order_id);
    }
  }, expected);
}

void CheckRejected(std::span<const std::byte> bytes, BookMode mode, DecodeError error) {
  const auto result = DecodeFrame(bytes, kLocalRecv, mode);
  CHECK(result.error == error);
  CHECK_FALSE(result.event.has_value());
}

void CheckValidation(const qlib::feed_event& event, DecodeError want) {
  CHECK(ValidateEvent(event) == want);
  const auto encoded = EncodeFeedEvent(event);
  const auto mode = std::holds_alternative<qlib::level>(event) ? BookMode::L2 : BookMode::Mbo;
  const auto result = DecodeFrame(encoded.bytes(), kLocalRecv, mode);
  CHECK(result.error == want);
  CHECK(result.event.has_value() == (want == DecodeError::None));
  if (result.event) CheckFields(*result.event, event, kLocalRecv);
}

}  // namespace

TEST_CASE("独立 golden bytes 解码为完整命名字段", "[protocol][golden]") {
  for (const auto& sample : GoldenCases()) {
    DYNAMIC_SECTION(sample.filename) {
      const auto bytes = Fixture(sample.filename);
      REQUIRE(bytes.size() == sample.frame_size);
      const auto result = DecodeFrame(bytes, kLocalRecv, sample.mode);
      REQUIRE(result.error == DecodeError::None);
      REQUIRE(result.event.has_value());
      CheckFields(*result.event, sample.event, kLocalRecv);
    }
  }
}

TEST_CASE("测试编码器逐字节符合独立固定样本", "[protocol][encoder]") {
  qlib::order neighbour{};
  for (auto sample : GoldenCases()) {
    DYNAMIC_SECTION(sample.filename) {
      if (auto* order = std::get_if<qlib::order>(&sample.event)) {
        order->prev = &neighbour;
        order->next = &neighbour;
      }
      const auto encoded = EncodeFeedEvent(sample.event);
      const auto bytes = Fixture(sample.filename);
      REQUIRE(encoded.size == sample.frame_size);
      CHECK(std::ranges::equal(encoded.bytes(), bytes));
      CHECK(std::all_of(encoded.storage.begin() + encoded.size, encoded.storage.end(),
                        [](std::byte value) { return value == std::byte{0}; }));
      if (const auto* order = std::get_if<qlib::order>(&sample.event)) {
        CHECK(order->prev == &neighbour);
        CHECK(order->next == &neighbour);
      }
    }
  }
}

TEST_CASE("协议拒绝所有截断和尾部字节且错误优先级固定", "[protocol][shape]") {
  CheckRejected({}, static_cast<BookMode>(255), DecodeError::EmptyFrame);
  for (const auto& sample : GoldenCases()) {
    auto bytes = Fixture(sample.filename);
    for (std::size_t length = 1; length < bytes.size(); ++length) {
      CAPTURE(sample.filename, length);
      CheckRejected(std::span(bytes).first(length), static_cast<BookMode>(255),
                    DecodeError::WrongSize);
    }
    bytes.push_back(std::byte{0});
    CheckRejected(bytes, sample.mode, DecodeError::WrongSize);
    bytes.resize(1'024, std::byte{0});
    CheckRejected(bytes, static_cast<BookMode>(255), DecodeError::WrongSize);
  }
  for (const auto tag : {4, 127, 255}) {
    const std::array bytes{static_cast<std::byte>(tag)};
    CheckRejected(bytes, BookMode::Mbo, DecodeError::UnknownTag);
  }
  auto bytes = Fixture("order_add.bin");
  bytes[61] = std::byte{255};
  CheckRejected(bytes, BookMode::L2, DecodeError::ModeMismatch);
  CheckRejected(bytes, BookMode::Mbo, DecodeError::InvalidField);
  bytes[0] = std::byte{255};
  CheckRejected(bytes, BookMode::L2, DecodeError::UnknownTag);
}

TEST_CASE("模式矩阵区分逐笔和 L2 并拒绝未知模式", "[protocol][mode]") {
  for (const auto& sample : GoldenCases()) {
    const auto bytes = Fixture(sample.filename);
    for (const auto mode : {BookMode::Mbo, BookMode::L2}) {
      CAPTURE(sample.filename, mode);
      const bool allowed = std::holds_alternative<qlib::trade>(sample.event) || mode == sample.mode;
      const auto result = DecodeFrame(bytes, kLocalRecv, mode);
      CHECK(result.error == (allowed ? DecodeError::None : DecodeError::ModeMismatch));
      CHECK(result.event.has_value() == allowed);
    }
    CheckRejected(bytes, static_cast<BookMode>(255), DecodeError::ModeMismatch);
  }
}

TEST_CASE("原始枚举字节在构造事件之前接受已定义值并拒绝其余值", "[protocol][enum]") {
  struct EnumCase { const char* filename; BookMode mode; std::size_t wire_offset; };
  const std::array cases{
      EnumCase{"order_add.bin", BookMode::Mbo, 61},
      EnumCase{"order_add.bin", BookMode::Mbo, 62},
      EnumCase{"order_add.bin", BookMode::Mbo, 63},
      EnumCase{"trade.bin", BookMode::Mbo, 53},
      EnumCase{"cancel.bin", BookMode::Mbo, 29},
      EnumCase{"level.bin", BookMode::L2, 37}};
  for (const auto& item : cases) {
    auto bytes = Fixture(item.filename);
    for (const auto raw : {0, 1}) {
      bytes[item.wire_offset] = static_cast<std::byte>(raw);
      const auto result = DecodeFrame(bytes, kLocalRecv, item.mode);
      CHECK(result.error == DecodeError::None);
      CHECK(result.event.has_value());
    }
    for (const auto raw : {2, 127, 255}) {
      CAPTURE(item.filename, item.wire_offset, raw);
      bytes[item.wire_offset] = static_cast<std::byte>(raw);
      CheckRejected(bytes, item.mode, DecodeError::InvalidField);
    }
  }
}

TEST_CASE("公共字段拒绝负序号和时间并允许零品种及未知来源时间", "[protocol][fields]") {
  for (const auto& sample : GoldenCases()) {
    for (const auto bad : {std::int64_t{-1}, kMin}) {
      auto event = sample.event;
      std::visit([&](auto& value) { value.seq = bad; }, event);
      CheckValidation(event, DecodeError::InvalidField);
      event = sample.event;
      std::visit([&](auto& value) { value.event_ns = bad; }, event);
      CheckValidation(event, DecodeError::InvalidField);
    }
    auto event = sample.event;
    std::visit([](auto& value) {
      value.seq = 0;
      value.event_ns = 0;
      value.instrument_id = 0;
      value.recv_ns = kMin;
    }, event);
    CheckValidation(event, DecodeError::None);
    std::visit([](auto& value) {
      value.seq = kMax;
      value.event_ns = kMax;
      value.instrument_id = std::numeric_limits<std::uint32_t>::max();
      value.side = qlib::side::sell;
    }, event);
    CheckValidation(event, DecodeError::None);
    std::visit([](auto& value) { value.side = static_cast<qlib::side>(255); }, event);
    CheckValidation(event, DecodeError::InvalidField);
  }
}

TEST_CASE("新增订单严格检查已使用字段并保留未使用字段", "[protocol][order]") {
  const auto base = GoldenAdd();
  for (const auto bad : {std::int64_t{0}, std::int64_t{-1}, kMin}) {
    auto value = base;
    value.order_id = bad;
    CheckValidation(value, DecodeError::InvalidField);
    value = base;
    value.qty = bad;
    CheckValidation(value, DecodeError::InvalidField);
    value = base;
    value.price = bad;
    CheckValidation(value, DecodeError::InvalidField);
    value.type = qlib::order_type::market;
    CheckValidation(value, DecodeError::None);
  }
  auto value = base;
  value.order_id = kMax;
  value.price = kMax;
  value.qty = kMax;
  value.new_qty = kMin;
  CheckValidation(value, DecodeError::None);
  value.type = static_cast<qlib::order_type>(255);
  CheckValidation(value, DecodeError::InvalidField);
  value = base;
  value.action = static_cast<qlib::order_action>(255);
  CheckValidation(value, DecodeError::InvalidField);
}

TEST_CASE("改单删除允许非正剩余量和未使用价格", "[protocol][modify]") {
  const auto base = GoldenDelete();
  for (const auto qty : {kMin, std::int64_t{-1}, std::int64_t{0}}) {
    for (const auto price : {kMin, std::int64_t{0}, kMax}) {
      auto value = base;
      value.new_qty = qty;
      value.price = price;
      value.qty = kMin;
      CheckValidation(value, DecodeError::None);
    }
  }
  for (const auto qty : {std::int64_t{1}, kMax}) {
    auto value = base;
    value.new_qty = qty;
    value.price = 0;
    CheckValidation(value, DecodeError::InvalidField);
    value.price = -1;
    CheckValidation(value, DecodeError::InvalidField);
    value.price = kMax;
    CheckValidation(value, DecodeError::None);
  }
  for (const auto id : {std::int64_t{0}, std::int64_t{-1}, kMin}) {
    auto value = base;
    value.order_id = id;
    CheckValidation(value, DecodeError::InvalidField);
  }
}

TEST_CASE("成交允许缺失侧和未知 ID 但拒绝重复非零 ID", "[protocol][trade]") {
  struct IdCase { std::int64_t buy; std::int64_t sell; DecodeError error; };
  const std::array cases{
      IdCase{0, 0, DecodeError::None}, IdCase{0, kMax, DecodeError::None},
      IdCase{kMax, 0, DecodeError::None}, IdCase{7, 9, DecodeError::None},
      IdCase{7, 7, DecodeError::InvalidField}, IdCase{kMax, kMax, DecodeError::InvalidField},
      IdCase{-1, 0, DecodeError::InvalidField}, IdCase{0, -1, DecodeError::InvalidField},
      IdCase{kMin, 9, DecodeError::InvalidField}, IdCase{7, kMin, DecodeError::InvalidField}};
  for (const auto& item : cases) {
    CAPTURE(item.buy, item.sell);
    auto value = GoldenTrade();
    value.buy_order_id = item.buy;
    value.sell_order_id = item.sell;
    CheckValidation(value, item.error);
  }
}

TEST_CASE("成交严格要求正数量和正价格且接受最大正值", "[protocol][trade]") {
  for (const auto bad : {std::int64_t{0}, std::int64_t{-1}, kMin}) {
    auto value = GoldenTrade();
    value.qty = bad;
    CheckValidation(value, DecodeError::InvalidField);
    value = GoldenTrade();
    value.price = bad;
    CheckValidation(value, DecodeError::InvalidField);
  }
  auto value = GoldenTrade();
  value.qty = kMax;
  value.price = kMax;
  CheckValidation(value, DecodeError::None);
}

TEST_CASE("撤单要求正 ID 和正数量且不查询已有订单", "[protocol][cancel]") {
  for (const auto bad : {std::int64_t{0}, std::int64_t{-1}, kMin}) {
    auto value = GoldenCancel();
    value.qty = bad;
    CheckValidation(value, DecodeError::InvalidField);
    value = GoldenCancel();
    value.order_id = bad;
    CheckValidation(value, DecodeError::InvalidField);
  }
  auto value = GoldenCancel();
  value.order_id = kMax;
  value.qty = kMax;
  CheckValidation(value, DecodeError::None);
}

TEST_CASE("L2 数量保留整个有符号范围且价格必须为正", "[protocol][level]") {
  for (const auto qty : {kMin, std::int64_t{-1}, std::int64_t{0}, std::int64_t{1}, kMax}) {
    auto value = GoldenLevel();
    value.qty = qty;
    CheckValidation(value, DecodeError::None);
  }
  for (const auto price : {kMin, std::int64_t{-1}, std::int64_t{0}}) {
    auto value = GoldenLevel();
    value.price = price;
    CheckValidation(value, DecodeError::InvalidField);
  }
  auto value = GoldenLevel();
  value.price = kMax;
  CheckValidation(value, DecodeError::None);
}

TEST_CASE("非对齐及指针填充污染不影响解码且输入字节保持不变", "[protocol][layout]") {
  for (const auto& sample : GoldenCases()) {
    auto bytes = Fixture(sample.filename);
    std::visit([&](const auto& value) {
      using T = std::remove_cvref_t<decltype(value)>;
      if constexpr (std::is_same_v<T, qlib::order>) {
        std::fill(bytes.begin() + 41, bytes.begin() + 57, std::byte{0xA5});
        bytes[64] = std::byte{0xCC};
      } else if constexpr (std::is_same_v<T, qlib::trade>) {
        std::fill(bytes.begin() + 54, bytes.begin() + 57, std::byte{0xCC});
      } else if constexpr (std::is_same_v<T, qlib::cancel>) {
        std::fill(bytes.begin() + 30, bytes.begin() + 33, std::byte{0xCC});
      } else {
        std::fill(bytes.begin() + 38, bytes.begin() + 41, std::byte{0xCC});
      }
      std::fill(bytes.begin() + 1 + offsetof(T, recv_ns), bytes.end(), std::byte{0xFF});
    }, sample.event);
    alignas(8) std::array<std::byte, 82> unaligned{};
    std::ranges::copy(bytes, unaligned.begin() + 1);
    const auto before = unaligned;
    const auto result = DecodeFrame({unaligned.data() + 1, bytes.size()}, kLocalRecv, sample.mode);
    REQUIRE(result.event.has_value());
    CHECK(result.error == DecodeError::None);
    CheckFields(*result.event, sample.event, kLocalRecv);
    CHECK(unaligned == before);
  }
}

TEST_CASE("返回事件拥有独立值且坏帧不污染下一次解码", "[protocol][lifetime]") {
  for (const auto& sample : GoldenCases()) {
    qbook::DecodeResult result;
    {
      auto bytes = Fixture(sample.filename);
      result = DecodeFrame(bytes, kLocalRecv, sample.mode);
      std::ranges::fill(bytes, std::byte{0xFF});
      CheckRejected(bytes, sample.mode, DecodeError::UnknownTag);
    }
    REQUIRE(result.event.has_value());
    CheckFields(*result.event, sample.event, kLocalRecv);
    const auto clean = Fixture(sample.filename);
    const auto again = DecodeFrame(clean, kLocalRecv + 1, sample.mode);
    REQUIRE(again.event.has_value());
    CheckFields(*again.event, sample.event, kLocalRecv + 1);
  }
}

TEST_CASE("纯协议不按重复序号或重复事件执行状态判断", "[protocol][stateless]") {
  auto value = GoldenAdd();
  for (const auto seq : {std::int64_t{99}, std::int64_t{99}, std::int64_t{3}, kMax}) {
    value.seq = seq;
    CheckValidation(value, DecodeError::None);
  }
}
