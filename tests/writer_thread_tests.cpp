#include "Threads/WriterThread.h"
#include "Threads/MetricsThread.h"
#include "Runtime.h"
#include "detail/RunDirectory.h"
#include "support/FailingOutputStream.h"
#include "support/OrderbookTestSupport.h"
#include "support/ParquetReadback.h"
#include "support/PipelineTestSupport.h"
#include "support/StorageTestSupport.h"
#include "support/WriterThreadInspector.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <fstream>
#include <limits>
#include <map>

using namespace qbook;
using namespace qbook::testing;
using namespace std::chrono_literals;

namespace {
void Finish(std::jthread& thread, std::atomic<bool>& done, RunControl& control) {
  thread.request_stop();
  WaitUntil([&] { return done.load(); }, control, ScenarioDeadline());
  thread.join();
}
void CheckOwner(const StreamProbe& probe, std::thread::id owner) {
  REQUIRE_FALSE(probe.overflow);
  REQUIRE_FALSE(probe.calls.empty());
  for (const auto& call : probe.calls) REQUIRE(call.thread == owner);
}

std::map<std::string, std::string> Contents(const std::filesystem::path& directory) {
  std::map<std::string, std::string> result;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    std::ifstream input(entry.path(), std::ios::binary);
    result.emplace(entry.path().filename().string(),
        std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()));
  }
  return result;
}
}  // namespace

TEST_CASE("Writer construction does not create files and validates its arguments", "[writer]") {
  TempDirectory temporary;
  const auto root = temporary.path() / "output";
  RecordQueue queue(1);
  REQUIRE_THROWS_AS(WriterThread(queue, root, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(WriterThread(queue, root, -1), std::invalid_argument);
  REQUIRE_THROWS_AS(WriterThread(queue, {}, 1), std::invalid_argument);
  WriterThread writer(queue, root, 2);
  REQUIRE_FALSE(std::filesystem::exists(root));
  REQUIRE(WriterThreadInspector::Released(writer));
}

TEST_CASE("Run directory claims skip occupied candidates and never overwrite files", "[writer]") {
  TempDirectory temporary;
  const auto root = temporary.path() / "output";
  REQUIRE(detail::ClaimRunDirectory(root, "fixed", 3) == root / "run-fixed-0");
  REQUIRE(detail::ClaimRunDirectory(root, "fixed", 3) == root / "run-fixed-1");
  REQUIRE_THROWS(detail::ClaimRunDirectory(root, "fixed", 2));
  std::ofstream(root / "run-file-0") << "preserve";
  REQUIRE_THROWS(detail::ClaimRunDirectory(root, "file", 3));
  std::string original;
  std::ifstream(root / "run-file-0") >> original;
  REQUIRE(original == "preserve");
  REQUIRE_THROWS_AS(detail::ClaimRunDirectory(root, "../escape", 3), std::invalid_argument);
  REQUIRE_THROWS_AS(detail::ClaimRunDirectory(root, "", 3), std::invalid_argument);
  REQUIRE_THROWS_AS(detail::ClaimRunDirectory(root, "fixed", 0), std::invalid_argument);
}

TEST_CASE("Writer drains all five record types and counts only successful additions", "[writer][drain]") {
  TempDirectory temporary;
  RecordQueue queue(8);
  Metrics metrics;
  RunControl control;
  ExpectedRun expected;
  expected.orders = {MakeOrder(17, qlib::side::buy, 100, 10),
                     MakeModify(17, qlib::side::buy, -9, -7)};
  expected.trades = {MakeTrade(17, 0, 3)};
  expected.cancels = {MakeCancel(999, 2)};
  expected.levels = {MakeLevel(qlib::side::sell, 101, -3)};
  qlib::book book{};
  book.instrument_id = 7;
  book.event_ns = 19;
  book.recv_ns = 29;
  book.bid_price[0] = 100;
  book.bid_qty[0] = 7;
  expected.books = {book};
  for (const auto& value : expected.orders) REQUIRE(queue.try_push(qlib::record{value}));
  REQUIRE(queue.try_push(qlib::record{expected.trades[0]}));
  REQUIRE(queue.try_push(qlib::record{expected.cancels[0]}));
  REQUIRE(queue.try_push(qlib::record{expected.levels[0]}));
  REQUIRE(queue.try_push(qlib::record{book}));
  WriterThread writer(queue, temporary.path(), 3);
  std::atomic<bool> done{false};
  std::jthread worker;
  ThreadCleanup cleanup(control, {&worker});
  worker = Launch("writer", control, [&](auto stop) { writer.Run(metrics, stop, control); }, &done);
  Finish(worker, done, control);
  cleanup.Dismiss();
  REQUIRE_FALSE(control.Failed());
  REQUIRE(queue.empty());
  REQUIRE(WriterThreadInspector::Released(writer));
  CheckExpectedFiles(ReadRunDirectory(WriterThreadInspector::Directory(writer), 100), expected);
  REQUIRE(metrics.writer_orders.Read() == 3);
  REQUIRE(metrics.writer_trades.Read() == 1);
  REQUIRE(metrics.writer_levels.Read() == 1);
  REQUIRE(metrics.writer_books.Read() == 1);
  REQUIRE_THROWS_AS(writer.Run(metrics, {}, control), std::logic_error);
}

TEST_CASE("Writer releases every file on its Run thread on normal and failed exits", "[writer][failure]") {
  const bool fail = GENERATE(false, true);
  TempDirectory temporary;
  RecordQueue queue(4);
  Metrics metrics;
  RunControl control;
  auto probe = std::make_shared<StreamProbe>();
  StreamFaultPlan fault;
  fault.fail_write = fail;
  detail::WriterHooks hooks{MakeOutputFactory(probe, fault), [&](const qlib::record&) {
    probe->armed.store(true);
  }};
  auto writer = WriterThreadInspector::Create(queue, temporary.path(), 2, hooks);
  REQUIRE(queue.try_push(qlib::record{MakeOrder(1, qlib::side::buy, 100, 3)}));
  REQUIRE(queue.try_push(qlib::record{MakeOrder(2, qlib::side::buy, 100, 4)}));
  std::atomic<bool> done{false};
  std::jthread worker;
  ThreadCleanup cleanup(control, {&worker});
  worker = Launch("writer", control, [&](auto stop) { writer->Run(metrics, stop, control); }, &done);
  const auto owner = worker.get_id();
  worker.request_stop();
  // Failed 是预期结果，等待 done 时不把它当作测试自身失败。
  RunControl waiter;
  WaitUntil([&] { return done.load(); }, waiter, ScenarioDeadline());
  worker.join();
  cleanup.Dismiss();
  REQUIRE(control.Failed() == fail);
  REQUIRE(probe->failures == (fail ? 1 : 0));
  REQUIRE(metrics.writer_orders.Read() == (fail ? 1 : 2));
  REQUIRE(WriterThreadInspector::Released(*writer));
  CheckOwner(*probe, owner);
  REQUIRE(std::count_if(probe->calls.begin(), probe->calls.end(), [](const auto& call) {
    return call.operation == StreamOperation::Destroy;
  }) == 5);
  const auto calls = probe->calls.size();
  writer.reset();
  REQUIRE(probe->calls.size() == calls);
  if (!fail) {
    const auto directory = *std::filesystem::directory_iterator(temporary.path());
    ExpectedRun expected;
    expected.orders = {MakeOrder(1, qlib::side::buy, 100, 3), MakeOrder(2, qlib::side::buy, 100, 4)};
    CheckExpectedFiles(ReadRunDirectory(directory.path(), 100), expected);
  }
}

TEST_CASE("Writer fatal before Run creates nothing but ordinary pre-stop saves empty schemas", "[writer][stop]") {
  const bool failed = GENERATE(false, true);
  TempDirectory temporary;
  RecordQueue queue(1);
  Metrics metrics;
  RunControl control;
  const auto root = temporary.path() / "output";
  WriterThread writer(queue, root, 2);
  std::stop_source stop;
  stop.request_stop();
  if (failed) control.Fail("upstream", std::make_exception_ptr(std::runtime_error("failed")));
  writer.Run(metrics, stop.get_token(), control);
  REQUIRE(WriterThreadInspector::Released(writer));
  if (failed) REQUIRE_FALSE(std::filesystem::exists(root));
  else CheckExpectedFiles(ReadRunDirectory(WriterThreadInspector::Directory(writer), 1), {});
}

TEST_CASE("Writer open failure releases already opened streams in its owner thread", "[writer][failure]") {
  TempDirectory temporary;
  RecordQueue queue(1);
  Metrics metrics;
  RunControl control;
  auto probe = std::make_shared<StreamProbe>();
  StreamFaultPlan fault;
  fault.file = "levels.parquet";
  fault.fail_open = true;
  detail::WriterHooks hooks{MakeOutputFactory(probe, fault), {}};
  auto writer = WriterThreadInspector::Create(queue, temporary.path(), 2, hooks);
  std::atomic<bool> done{false};
  std::jthread worker;
  ThreadCleanup cleanup(control, {&worker});
  worker = Launch("writer", control, [&](auto stop) { writer->Run(metrics, stop, control); }, &done);
  const auto owner = worker.get_id();
  RunControl waiter;
  WaitUntil([&] { return done.load(); }, waiter, ScenarioDeadline());
  worker.join();
  cleanup.Dismiss();
  REQUIRE(control.Failed());
  REQUIRE(std::string(control.Stage()) == "writer");
  REQUIRE(WriterThreadInspector::Released(*writer));
  REQUIRE(probe->failures == 1);
  CheckOwner(*probe, owner);
}

TEST_CASE("Writer metric overflow after Add reports failure and does not call observer", "[writer][failure]") {
  TempDirectory temporary;
  RecordQueue queue(1);
  Metrics metrics;
  RunControl control;
  metrics.writer_orders.Set(std::numeric_limits<std::uint64_t>::max());
  REQUIRE(queue.try_push(qlib::record{MakeOrder(1, qlib::side::buy, 100, 1)}));
  std::size_t observed = 0;
  detail::WriterHooks hooks{{}, [&](const qlib::record&) { ++observed; }};
  auto writer = WriterThreadInspector::Create(queue, temporary.path(), 3, hooks);
  std::atomic<bool> done{false};
  std::jthread worker;
  ThreadCleanup cleanup(control, {&worker});
  worker = Launch("writer", control, [&](auto stop) { writer->Run(metrics, stop, control); }, &done);
  RunControl waiter;
  WaitUntil([&] { return done.load(); }, waiter, ScenarioDeadline());
  worker.join();
  cleanup.Dismiss();
  REQUIRE(control.Failed());
  REQUIRE_THROWS_AS(std::rethrow_exception(control.Error()), std::overflow_error);
  REQUIRE(metrics.writer_orders.Read() == std::numeric_limits<std::uint64_t>::max());
  REQUIRE(observed == 0);
  REQUIRE(WriterThreadInspector::Released(*writer));
}

TEST_CASE("successive Writer runs preserve all bytes from the previous output", "[writer]") {
  TempDirectory temporary;
  std::filesystem::path first;
  std::map<std::string, std::string> original;
  for (int run = 0; run < 2; ++run) {
    RecordQueue queue(1);
    Metrics metrics;
    RunControl control;
    auto value = MakeOrder(run + 1, qlib::side::buy, 100, 5);
    REQUIRE(queue.try_push(qlib::record{value}));
    WriterThread writer(queue, temporary.path(), 3);
    std::stop_source stop;
    stop.request_stop();
    writer.Run(metrics, stop.get_token(), control);
    const auto directory = WriterThreadInspector::Directory(writer);
    ExpectedRun expected;
    expected.orders.push_back(value);
    CheckExpectedFiles(ReadRunDirectory(directory, 10), expected);
    if (run == 0) { first = directory; original = Contents(first); }
    else {
      REQUIRE(directory != first);
      REQUIRE(original.size() == 5);
      REQUIRE(Contents(first) == original);
    }
  }
}

TEST_CASE("concurrent identical directory candidates claim distinct exclusive directories", "[writer]") {
  TempDirectory temporary;
  std::array<std::filesystem::path, 2> directories;
  std::array<std::exception_ptr, 2> errors;
  std::array<std::jthread, 2> workers;
  for (std::size_t i = 0; i < workers.size(); ++i)
    workers[i] = std::jthread([&, i] {
      try { directories[i] = detail::ClaimRunDirectory(temporary.path(), "same", 2); }
      catch (...) { errors[i] = std::current_exception(); }
    });
  for (auto& worker : workers) worker.join();
  REQUIRE_FALSE(errors[0]);
  REQUIRE_FALSE(errors[1]);
  REQUIRE(directories[0] != directories[1]);
  REQUIRE(std::filesystem::is_directory(directories[0]));
  REQUIRE(std::filesystem::is_directory(directories[1]));
}
