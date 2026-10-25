#include "ParquetWriter.h"
#include "support/FailingOutputStream.h"
#include "support/ParquetReadback.h"
#include "support/ParquetWriterTestAccess.h"
#include "support/StorageTestSupport.h"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <tuple>

namespace {
using namespace qbook;
using namespace qbook::testing;
using Catch::Matchers::ContainsSubstring;

ExpectedRun Samples(std::size_t count) {
  ExpectedRun result;
  for (std::size_t i = 0; i < count; ++i) {
    const auto n = static_cast<std::int64_t>(i);
    qlib::order order{};
    order.seq = 9007199254740993 + n;
    order.order_id = 8007199254740997 + n;
    order.price = 7007199254740991 + n;
    order.qty = 31 + n;
    order.new_qty = -2 + n;
    order.event_ns = i == 0 ? 0 : 1700000000000000001 + n;
    order.recv_ns = 1700000000000000011 + n;
    order.instrument_id = 4000000000U + static_cast<std::uint32_t>(i);
    order.side = i % 2 ? qlib::side::sell : qlib::side::buy;
    order.type = i % 2 ? qlib::order_type::market : qlib::order_type::limit;
    order.action = i % 2 ? qlib::order_action::modify : qlib::order_action::add;
    result.orders.push_back(order);
    result.trades.push_back({101 + n, 0, 501 + n, 601 + n, 701 + n,
                             801 + n, 901U + static_cast<std::uint32_t>(i),
                             qlib::side::sell, 1001 + n});
    result.cancels.push_back({1101 + n, 1201 + n, 1301 + n,
                              1401U + static_cast<std::uint32_t>(i),
                              qlib::side::buy, 1501 + n, 1601 + n});
    result.levels.push_back({1701 + n, 1801 + n, -1901 - n, 2001 + n,
                             2101U + static_cast<std::uint32_t>(i),
                             qlib::side::sell, 2201 + n});
    qlib::book book{};
    book.event_ns = 2301 + n;
    book.recv_ns = 2401 + n;
    book.instrument_id = 2501 + static_cast<std::uint32_t>(i);
    if (i % 3 != 0) { book.bid_price[0] = 2601 + n; book.bid_qty[0] = 2701 + n; }
    if (i % 3 == 2) { book.ask_price[0] = 2801 + n; book.ask_qty[0] = 2901 + n; }
    result.books.push_back(book);
  }
  return result;
}

void AddSamples(ParquetWriter& writer, const ExpectedRun& expected) {
  for (std::size_t i = 0; i < expected.orders.size(); ++i) {
    writer.Add(expected.orders[i]); writer.Add(expected.trades[i]);
    writer.Add(expected.cancels[i]); writer.Add(expected.levels[i]);
    writer.Add(expected.books[i]);
  }
}

std::string Bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void RequireReleased(const StreamProbe& probe, std::size_t opened) {
  REQUIRE_FALSE(probe.overflow);
  const auto destroyed = std::count_if(probe.calls.begin(), probe.calls.end(),
      [](const auto& call) { return call.operation == StreamOperation::Destroy; });
  REQUIRE(static_cast<std::size_t>(destroyed) == opened);
  for (const auto& call : probe.calls) REQUIRE(call.thread == std::this_thread::get_id());
}

void RequireCleanupOnly(const StreamProbe& probe, std::size_t from) {
  std::set<std::string> aborted;
  for (std::size_t i = from; i < probe.calls.size(); ++i) {
    const auto& call = probe.calls[i];
    if (call.operation == StreamOperation::Abort) aborted.insert(call.file);
    if (call.operation == StreamOperation::Write) REQUIRE(aborted.contains(call.file));
    REQUIRE(call.operation != StreamOperation::Close);
  }
}

auto OrderFields(const qlib::order& v) {
  return std::tuple(v.seq, v.order_id, v.price, v.qty, v.new_qty, v.prev, v.next,
                    v.instrument_id, v.side, v.type, v.action, v.event_ns, v.recv_ns);
}
}

TEST_CASE("Parquet keeps five independent schemas and every batch boundary", "[parquet]") {
  const auto count = GENERATE(0U, 1U, 2U, 3U, 4U, 7U);
  const auto batch_rows = GENERATE(1, 3);
  TempDirectory directory;
  const auto expected = Samples(count);
  ParquetWriter writer(directory.path(), batch_rows);
  AddSamples(writer, expected);
  writer.Close();
  REQUIRE_NOTHROW(CheckExpectedFiles(ReadRunDirectory(directory.path(), 7), expected));
}

TEST_CASE("Parquet preserves input links and successful Close is idempotent", "[parquet]") {
  TempDirectory directory;
  auto expected = Samples(1);
  qlib::order previous{}, next{};
  expected.orders[0].prev = &previous; expected.orders[0].next = &next;
  const auto original = expected.orders[0];
  ParquetWriter writer(directory.path(), 3);
  writer.Add(expected.orders[0]);
  writer.Close();
  const auto before = Bytes(directory.path() / "orders.parquet");
  writer.Close();
  REQUIRE(Bytes(directory.path() / "orders.parquet") == before);
  REQUIRE_THROWS_AS(writer.Add(original), std::logic_error);
  REQUIRE(ParquetWriterTestAccess::Closed(writer));
  REQUIRE(expected.orders[0].prev == &previous);
  REQUIRE(expected.orders[0].next == &next);
  REQUIRE(OrderFields(expected.orders[0]) == OrderFields(original));
  expected.trades.clear(); expected.cancels.clear(); expected.levels.clear(); expected.books.clear();
  REQUIRE_NOTHROW(CheckExpectedFiles(ReadRunDirectory(directory.path(), 1), expected));
}

TEST_CASE("Parquet types accumulate separate batches", "[parquet]") {
  TempDirectory directory;
  auto expected = Samples(7);
  expected.trades.resize(4); expected.cancels.resize(3);
  expected.levels.resize(2); expected.books.resize(1);
  ParquetWriter writer(directory.path(), 3);
  for (std::size_t i = 0; i < 7; ++i) {
    writer.Add(expected.orders[i]);
    if (i < 4) writer.Add(expected.trades[i]);
    if (i < 3) writer.Add(expected.cancels[i]);
    if (i < 2) writer.Add(expected.levels[i]);
    if (i < 1) writer.Add(expected.books[i]);
  }
  writer.Close();
  REQUIRE_NOTHROW(CheckExpectedFiles(ReadRunDirectory(directory.path(), 7), expected));
  REQUIRE_THROWS(ReadRunDirectory(directory.path(), 6));
}

TEST_CASE("Parquet readback rejects an independently written wrong schema", "[parquet]") {
  TempDirectory directory;
  const auto output = arrow::io::FileOutputStream::Open((directory.path() / "orders.parquet").string());
  REQUIRE(output.ok());
  auto schema = arrow::schema({arrow::field("wrong", arrow::int32(), false)});
  auto writer_result = parquet::arrow::FileWriter::Open(*schema, arrow::default_memory_pool(), *output,
      parquet::default_writer_properties(), parquet::ArrowWriterProperties::Builder().store_schema()->build());
  REQUIRE(writer_result.ok());
  auto writer = std::move(writer_result).ValueOrDie();
  REQUIRE(writer->Close().ok());
  REQUIRE((*output)->Close().ok());
  REQUIRE_THROWS_WITH(ReadRunDirectory(directory.path(), 1), ContainsSubstring("列数量不符"));
}

TEST_CASE("Parquet refuses invalid capacities and never truncates an existing file", "[parquet]") {
  TempDirectory directory;
  REQUIRE_THROWS_AS(ParquetWriter(directory.path(), 0), std::invalid_argument);
  REQUIRE_THROWS_AS(ParquetWriter(directory.path(), -1), std::invalid_argument);
  REQUIRE_THROWS_AS(ParquetWriter(directory.path(), std::numeric_limits<std::int64_t>::max()),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(ParquetWriter(directory.path(), std::numeric_limits<std::int64_t>::max() / 8 + 1),
                    std::invalid_argument);
  REQUIRE(std::filesystem::is_empty(directory.path()));
  const auto file = directory.path() / "orders.parquet";
  { std::ofstream output(file); output << "existing result"; }
  REQUIRE_THROWS(ParquetWriter(directory.path(), 2));
  REQUIRE(Bytes(file) == "existing result");
  REQUIRE_THROWS(ParquetWriter(directory.path() / "missing", 2));
}

TEST_CASE("Parquet partial construction aborts every previously opened stream", "[parquet]") {
  TempDirectory directory;
  auto probe = std::make_shared<StreamProbe>();
  StreamFaultPlan plan; plan.file = "levels.parquet"; plan.fail_open = true;
  REQUIRE_THROWS_WITH(ParquetWriterTestAccess::Create(directory.path(), 3,
      MakeOutputFactory(probe, plan)), ContainsSubstring("injected open"));
  RequireReleased(*probe, 3);
}

TEST_CASE("Parquet full batch failure marks Failed and refuses replay", "[parquet]") {
  const auto partial_bytes = GENERATE(0U, 2U);
  TempDirectory directory;
  auto probe = std::make_shared<StreamProbe>();
  StreamFaultPlan plan; plan.fail_write = true; plan.partial_write_bytes = partial_bytes;
  auto writer = ParquetWriterTestAccess::Create(directory.path(), 2, MakeOutputFactory(probe, plan));
  const auto values = Samples(2);
  writer->Add(values.orders[0]);
  probe->armed.store(true);
  REQUIRE_THROWS_WITH(writer->Add(values.orders[1]), ContainsSubstring("injected write"));
  REQUIRE(probe->failures == 1);
  REQUIRE(ParquetWriterTestAccess::Failed(*writer));
  const auto before = probe->calls.size();
  REQUIRE_THROWS_AS(writer->Add(values.orders[1]), std::logic_error);
  REQUIRE(probe->calls.size() == before);
  REQUIRE_THROWS_AS(writer->Close(), std::logic_error);
  writer.reset();
  RequireCleanupOnly(*probe, before);
  RequireReleased(*probe, 5);
}

TEST_CASE("Parquet Close reports tail footer and stream errors without losing first error", "[parquet]") {
  const auto failure = GENERATE(0, 1, 2, 3);
  TempDirectory directory;
  auto probe = std::make_shared<StreamProbe>();
  StreamFaultPlan plan;
  plan.fail_write = failure != 2; plan.fail_close = failure == 2;
  plan.fail_abort = failure == 3;
  auto writer = ParquetWriterTestAccess::Create(directory.path(), 3, MakeOutputFactory(probe, plan));
  if (failure == 0 || failure == 3) writer->Add(Samples(1).orders[0]);
  writer->Add(Samples(1).trades[0]);
  writer->Add(Samples(1).books[0]);
  probe->armed.store(true);
  const auto before_close = probe->calls.size();
  REQUIRE_THROWS_WITH(writer->Close(), ContainsSubstring(failure == 2 ? "injected close" : "injected write"));
  REQUIRE(ParquetWriterTestAccess::Failed(*writer));
  REQUIRE(probe->failures >= 1);
  REQUIRE_THROWS_AS(writer->Close(), std::logic_error);
  writer.reset();
  RequireReleased(*probe, 5);
  const auto first_abort = std::find_if(probe->calls.begin(), probe->calls.end(),
      [](const auto& call) { return call.operation == StreamOperation::Abort; });
  REQUIRE(first_abort != probe->calls.end());
  for (auto call = probe->calls.begin() + before_close; call != first_abort; ++call)
    if (call->operation == StreamOperation::Write) REQUIRE(call->file == "orders.parquet");
  RequireCleanupOnly(*probe, static_cast<std::size_t>(first_abort - probe->calls.begin()));
}

TEST_CASE("Parquet destruction aborts buffers while an outer exception unwinds", "[parquet]") {
  TempDirectory directory;
  auto probe = std::make_shared<StreamProbe>();
  std::size_t before = 0;
  REQUIRE_THROWS_WITH([&] {
    auto writer = ParquetWriterTestAccess::Create(directory.path(), 3, MakeOutputFactory(probe));
    writer->Add(Samples(1).orders[0]);
    before = probe->calls.size();
    throw std::runtime_error("outer failure");
  }(), "outer failure");
  RequireReleased(*probe, 5);
  RequireCleanupOnly(*probe, before);
}
